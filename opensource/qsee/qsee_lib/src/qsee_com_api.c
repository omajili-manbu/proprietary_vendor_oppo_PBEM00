/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qsee_com_api.c - rebuilt implementation of vendor libQSEEComAPI.so.
 *
 * Every exported function replicates the observable behaviour of the
 * vendor binary (evidence: REPORT.md Q1/Q4, IDA dumps in work/ida/),
 * with one systematic difference: all shared buffers are allocated from
 * dma-buf heaps (/dev/dma_heap/<name> + DMA_HEAP_IOCTL_ALLOC) instead
 * of the removed /dev/ion interface.
 *
 * Nothing here is copied from decompiled code; the ioctl sequence,
 * struct layouts and corner-case semantics are re-derived from the
 * kernel driver source (drivers/misc/qseecom.c of the 7.3 tree) and
 * the ABI observations recorded in the report.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "qsee_internal.h"

#define LOGE(fmt, ...) \
	fprintf(stderr, "libQSEECom: " fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) \
	fprintf(stderr, "libQSEECom: " fmt, ##__VA_ARGS__)

/* Vendor log tags kept for drop-in behavioural parity on logcat. */
#define QSEECOMAPI_TAG "QSEECOMAPI"

static int page_align(uint32_t len)
{
	return (int)((len + 4095u) & ~4095u);
}

/* Portable bounded copy (glibc strlcpy exists only from 2.38 on). */
static void qsee_strlcpy(char *dst, const char *src, size_t dsize)
{
	size_t srclen;

	if (!dsize)
		return;
	srclen = strlen(src);
	if (srclen >= dsize)
		srclen = dsize - 1;
	memcpy(dst, src, srclen);
	dst[srclen] = '\0';
}

#define strlcpy qsee_strlcpy

/* Forward declaration: __QSEECom_get_handle() equivalent. */
int __QSEECom_get_handle(struct qseecom_handle **handle, int unused,
			 const char *base_dir, const char *app_name,
			 const uint8_t *app_image, uint32_t image_len,
			 uint32_t sb_size, int mode);

int qsee_open_qseecom_dev(void)
{
	int fd = open("/dev/qseecom", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		LOGE("Error::Failed to open /dev/qseecom device: %s\n",
		     strerror(errno));
	return fd;
}

static int get_qseos_version(int fd, uint32_t *version)
{
	struct qsee_uapi_qseos_version_req req = { 0 };

	if (ioctl(fd, QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ, &req) < 0) {
		LOGE("Error::QSEOS version info. ret value = %d\n", -errno);
		return -1;
	}
	*version = req.qseos_version;
	return 0;
}

/*
 * Allocate the client shared buffer and register it with the driver via
 * SET_MEM_PARAM.  Vendor flow: __QSEECom_get_handle @0x3014 tail
 * ("QSEECOM_IOCTL_SET_MEM_PARAM_REQ failed..." on error).
 */
static int setup_client_sb(struct qseecom_handle *h)
{
	struct qsee_uapi_set_sb_mem_param_req req = { 0 };
	size_t map_len = page_align(h->sbuf_len);
	int ret;

	ret = qsee_heap_alloc_contig(map_len, &h->heap_dev_fd, &h->ifd_fd,
			      (void **)&h->sbuf);
	if (ret != 0) {
		LOGE("Error::shared buffer allocation failed (contig policy: "
		     "qseecom_ta_region -> default_cma_region)\n");
		return ret;
	}
	memset(h->sbuf, 0, map_len);

	req.ifd_data_fd = h->ifd_fd;
	req.virt_sb_base = h->sbuf;
	req.sb_len = h->sbuf_len;
	if (ioctl(h->fd, QSEECOM_IOCTL_SET_MEM_PARAM_REQ, &req) < 0) {
		ret = -errno;
		LOGE("Error::QSEECOM_IOCTL_SET_MEM_PARAM_REQ failed with "
		     "ret = %d, errno = %d\n", -ret, errno);
		qsee_heap_free(h->heap_dev_fd, h->ifd_fd, h->sbuf, map_len);
		h->sbuf = NULL;
		h->ifd_fd = -1;
		h->heap_dev_fd = -1;
		return ret;
	}
	return 0;
}

static void teardown_client_sb(struct qseecom_handle *h)
{
	if (h->sbuf) {
		qsee_heap_free(h->heap_dev_fd, h->ifd_fd, h->sbuf,
			       page_align(h->sbuf_len));
		h->sbuf = NULL;
		h->ifd_fd = -1;
		h->heap_dev_fd = -1;
	}
}

/*
 * Load a TA image (already assembled in a contiguous dma-buf) and hand
 * it to the driver.  ioctl is LOAD_APP (nr 7) or LOAD_EXTERNAL_ELF
 * (nr 13); the image buffer is freed right after the call either way
 * (vendor behaviour: the mirror buffer is one-shot).
 */
static int load_image_ioctl(struct qseecom_handle *h,
			    struct qsee_uapi_load_img_req *lreq,
			    int external_elf)
{
	int cmd = external_elf ? QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ :
				 QSEECOM_IOCTL_LOAD_APP_REQ;
	int ret;
	int img_heap_fd = -1, img_dmabuf_fd = -1;
	void *img_va = NULL;
	size_t map_len = page_align(lreq->img_len);

	if (lreq->img_len == 0)
		return -EINVAL;

	ret = qsee_heap_alloc_contig(map_len, &img_heap_fd,
			      &img_dmabuf_fd, &img_va);
	if (ret != 0) {
		LOGE("Error::ION_memalloc failed\n");
		return ret;
	}

	lreq->ifd_data_fd = img_dmabuf_fd;
	ret = ioctl(h->fd, cmd, lreq);
	if (ret < 0) {
		ret = -errno;
		LOGE("Error::Load image %s request failed ret = %d\n",
		     lreq->img_name, -ret);
	} else if (!external_elf) {
		h->app_id = lreq->app_id;
		LOGI("Loaded image: APP id = %u\n", lreq->app_id);
	}

	qsee_heap_free(img_heap_fd, img_dmabuf_fd, img_va, map_len);
	return ret < 0 ? ret : 0;
}

/*
 * File-based TA load, mirrors vendor __QSEECom_load_image(): probe
 * sizes, allocate once, stream segments, single ioctl, free immediately.
 */
static int load_image_from_fs(struct qseecom_handle *h, const char *base_dir,
			      const char *app_name, int external_elf)
{
	struct qsee_uapi_load_img_req lreq = { 0 };
	uint32_t total_len = 0, mdt_len = 0, arch = 0;
	size_t map_len;
	int ret;

	ret = qsee_probe_image_size(base_dir, app_name, &total_len, &arch);
	if (ret != 0) {
		LOGE("Error::Cannot probe %s/%s.mdt: %s\n", base_dir,
		     app_name, strerror(-ret));
		return ret;
	}
	map_len = page_align(total_len);
	{
		int ihfd = -1, idfd = -1;
		void *iva = NULL;

		ret = qsee_heap_alloc_contig(map_len, &ihfd, &idfd, &iva);
		if (ret != 0) {
			LOGE("Error::image buffer allocation failed\n");
			return ret;
		}

		ret = qsee_read_image_files(base_dir, app_name, iva, map_len,
					    &mdt_len, &total_len, &arch);
		if (ret == 0) {
			lreq.mdt_len = mdt_len;
			lreq.img_len = total_len;
			lreq.ifd_data_fd = idfd;
			strlcpy(lreq.img_name, app_name,
				sizeof(lreq.img_name));
			lreq.app_arch = arch;
			h->app_arch = arch;
			ret = load_image_ioctl(h, &lreq, external_elf);
		}

		qsee_heap_free(ihfd, idfd, iva, map_len);
	}
	if (ret != 0)
		LOGE("Error::Loading image failed with ret = %d\n", ret);
	return ret;
}

int QSEECom_start_app(struct qseecom_handle **handle, const char *base_dir,
		      const char *app_name, uint32_t sb_size)
{
	return __QSEECom_get_handle(handle, 1, base_dir, app_name, NULL, 0,
				    sb_size, 1);
}

/* Direct implementation of the vendor __QSEECom_get_handle() flow. */
int __QSEECom_get_handle(struct qseecom_handle **handle, int unused,
			 const char *base_dir, const char *app_name,
			 const uint8_t *app_image, uint32_t image_len,
			 uint32_t sb_size, int mode)
{
	(void)unused;
	struct qseecom_handle *h;
	uint32_t version = 0;
	int ret;

	if (!handle || *handle) {
		LOGE("Error::Client handle is not null!!!\n");
		return -1;
	}

	h = calloc(1, sizeof(*h));
	if (!h) {
		LOGE("Error::malloc failed. Could not allocate memory\n");
		return -1;
	}
	h->heap_dev_fd = -1;
	h->ifd_fd = -1;

	h->fd = qsee_open_qseecom_dev();
	if (h->fd < 0) {
		free(h);
		return -1;
	}

	if (get_qseos_version(h->fd, &version) != 0) {
		close(h->fd);
		free(h);
		return -1;
	}
	h->version = version;

	/*
	 * Vendor: for QSEE >= 20 the file-based start_app queries whether
	 * the app is already resident first (QSEECom_app_load_query);
	 * "already loaded" (ioctl succeeded) skips the load, "not loaded"
	 * continues.  The kernel returns -1/EBUSY-ish states here that the
	 * vendor lib distinguishes through errno.
	 */
	if (mode == 1 && base_dir && app_name && version >= QSEOS_VERSION_14) {
		struct qsee_uapi_app_load_query q = { 0 };
		int qret;

		strlcpy(q.app_name, app_name, sizeof(q.app_name));
		qret = ioctl(h->fd, QSEECOM_IOCTL_APP_LOADED_QUERY_REQ, &q);
		if (qret == 0) {
			/* Not loaded yet: proceed with the file load. */
		} else if (errno == EEXIST) {
			h->app_id = q.app_id;
			h->app_arch = q.app_arch;
			ret = 0;
			goto have_app;
		} else {
			LOGE("Error::ioctl call query if app is loaded "
			     "failed with ret = %d, errno = %d\n",
			     -1, errno);
		}
	}

	if (mode == 2) {
		/* In-memory trustlet (start_app_V2). */
		struct qsee_uapi_load_img_req lreq = { 0 };
		size_t map_len;

		if (!app_image || image_len == 0) {
			ret = -EINVAL;
			goto err;
		}
		lreq.mdt_len = 0;
		lreq.img_len = image_len;
		strlcpy(lreq.img_name, app_name, sizeof(lreq.img_name));
		lreq.app_arch = h->app_arch;
		map_len = page_align(image_len);
		{
			int ihfd = -1, idfd = -1;
			void *iva = NULL;

			ret = qsee_heap_alloc_contig(map_len, &ihfd, &idfd, &iva);
			if (ret != 0)
				goto err;
			memcpy(iva, app_image, image_len);
			lreq.ifd_data_fd = idfd;
			ret = load_image_ioctl(h, &lreq, 0);
			qsee_heap_free(ihfd, idfd, iva, map_len);
		}
		if (ret != 0)
			goto err;
	} else if (mode == 1) {
		ret = load_image_from_fs(h, base_dir, app_name, 0);
		if (ret != 0)
			goto err;
	} else if (mode == 0) {
		/*
		 * sb-only handle without an image load: the one-shot
		 * service commands open a temporary client context this
		 * way (QSEECom_send_service_cmd -> __QSEECom_get_handle
		 * mode 0).  The driver's SEND_CMD_SERVICE validation only
		 * requires the client sb to be registered (qseecom.c
		 * __validate_send_service_cmd_inputs: client.buf/sb_virt/
		 * user_virt_sb_base/sb_length), never a loaded app.
		 */
	} else {
		ret = -EINVAL;
		goto err;
	}

have_app:
	h->sbuf_len = sb_size;
	ret = setup_client_sb(h);
	if (ret != 0)
		goto err;

	*handle = h;
	return 0;

err:
	if (h->fd >= 0)
		close(h->fd);
	free(h);
	return ret ? ret : -1;
}

int QSEECom_start_app_V2(struct qseecom_handle **handle, const char *app_name,
			 const uint8_t *app_image, uint32_t image_len,
			 int version)
{
	uint32_t sb_size = 0;

	/*
	 * Vendor start_app_V2: version selects the SB sizing quirk
	 * (see __QSEECom_get_handle); callers pass the architecture of
	 * the embedded image and the library sizes the sb itself.
	 * TODO-VERIFY(sb_size_v2): the vendor binary derives the sb size
	 * for the V2 entry from the caller-provided "version" argument
	 * (__QSEECom_get_handle a5/n2==2 path, sb_len = n_3 + 128 when
	 * qsee version > 19); exact caller set unknown, evidence REPORT
	 * Q4.  Callers should prefer QSEECom_start_app with an explicit
	 * sb_size.
	 */
	if (version >= (int)QSEOS_VERSION_14)
		sb_size = image_len ? image_len : 4096;
	else
		sb_size = 4096;

	return __QSEECom_get_handle(handle, 1, NULL, app_name, app_image,
				    image_len, sb_size, 2);
}

int __QSEECom_dealloc_memory(struct qseecom_handle **handle, int unload_magic)
{
	struct qseecom_handle *h;
	int ret = 0;

	if (!handle || !*handle) {
		LOGE("Error::Cannot de-alloc memory. handle is NULL!!.\n");
		return -1;
	}
	h = *handle;

	if (h->version < QSEOS_VERSION_14) {
		LOGE("ERROR: UNLOAD_APP: Invalid qsee version =%d\n",
		     h->version);
		return -1;
	}
	if (unload_magic == QSEECOM_IOCTL_UNLOAD_APP_REQ) {
		LOGI("QSEECom_shutdown_app, app_id = %u\n", h->app_id);
		if (ioctl(h->fd, QSEECOM_IOCTL_UNLOAD_APP_REQ) < 0)
			ret = -errno;
	}
	teardown_client_sb(h);
	close(h->fd);
	free(h);
	*handle = NULL;
	return ret;
}

int QSEECom_shutdown_app(struct qseecom_handle **handle)
{
	return __QSEECom_dealloc_memory(handle, QSEECOM_IOCTL_UNLOAD_APP_REQ);
}

int QSEECom_app_load_query(struct qseecom_handle *handle,
			   const char *app_name)
{
	struct qsee_uapi_app_load_query q = { 0 };

	if (!handle || !app_name) {
		LOGE("Error::Handle or app_name is NULL!!.\n");
		return -2;
	}
	strlcpy(q.app_name, app_name, sizeof(q.app_name));
	if (ioctl(handle->fd, QSEECOM_IOCTL_APP_LOADED_QUERY_REQ, &q) == 0) {
		LOGI("App is not loaded in QSEE\n");
		return -5;
	}
	if (errno == EEXIST) {
		handle->app_id = q.app_id;
		handle->app_arch = q.app_arch;
		LOGI("App is already loaded QSEE and app id = %u\n",
		     q.app_id);
		return -4;
	}
	LOGE("Error::ioctl call query if app is loaded failed with ret = %d,"
	     " errno = %d\n", -1, errno);
	return -6;
}

int QSEECom_load_external_elf(struct qseecom_handle **handle,
			      const char *base_dir, const char *elf_name)
{
	struct qseecom_handle *h;
	uint32_t version = 0;
	int ret;

	if (!handle || *handle) {
		LOGE("Error::Client handle is not null!!!\n");
		return -1;
	}
	h = calloc(1, sizeof(*h));
	if (!h)
		return -1;
	h->heap_dev_fd = -1;
	h->ifd_fd = -1;
	h->fd = qsee_open_qseecom_dev();
	if (h->fd < 0) {
		free(h);
		return -1;
	}
	if (get_qseos_version(h->fd, &version) != 0) {
		close(h->fd);
		free(h);
		return -1;
	}
	h->version = version;

	ret = load_image_from_fs(h, base_dir, elf_name, 1);
	if (ret != 0) {
		close(h->fd);
		free(h);
		return ret;
	}
	*handle = h;
	return 0;
}

int QSEECom_unload_external_elf(struct qseecom_handle **handle)
{
	struct qseecom_handle *h;
	int ret;

	if (!handle || !*handle) {
		LOGE("Error::Cannot unload external elf. handle is NULL!!.\n");
		return -1;
	}
	h = *handle;
	ret = ioctl(h->fd, QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ);
	if (ret < 0)
		LOGE("Error::unload external elf req. ioctl failed with "
		     "ret = %d, errno = %d\n", -1, errno);
	/* Vendor closes elf-image fds kept at +0x14/+0x18; the rebuilt
	 * loader already freed the one-shot image buffer. */
	close(h->fd);
	free(h);
	*handle = NULL;
	return ret < 0 ? -errno : 0;
}

int QSEECom_register_listener(struct qseecom_handle **handle,
			      int listener_id, uint32_t sb_size,
			      int hierarchy)
{
	struct qseecom_handle *h;
	struct qsee_uapi_register_listener_req req = { 0 };
	uint32_t version = 0;
	int ret;

	(void)hierarchy;    /* passed as 0 by every vendor caller (OPEN-3) */

	if (!handle || *handle) {
		LOGE("Error::Listener Handle is not null!!!\n");
		return -1;
	}
	h = calloc(1, sizeof(*h));
	if (!h) {
		LOGE("Error::malloc failed. Could not allocate memory\n");
		return -1;
	}
	h->heap_dev_fd = -1;
	h->ifd_fd = -1;

	h->fd = qsee_open_qseecom_dev();
	if (h->fd < 0) {
		free(h);
		return -1;
	}
	if (get_qseos_version(h->fd, &version) != 0) {
		close(h->fd);
		free(h);
		return -1;
	}
	h->version = version;

	h->sbuf_len = sb_size;
	ret = qsee_heap_alloc_contig(page_align(sb_size), &h->heap_dev_fd, &h->ifd_fd,
			      (void **)&h->sbuf);
	if (ret != 0) {
		LOGE("Error::ION memalloc failed lstnr ID %d and ret value "
		     "= %d\n", listener_id, ret);
		close(h->fd);
		free(h);
		return -1;
	}
	memset(h->sbuf, 0, page_align(sb_size));

	req.listener_id = (uint32_t)listener_id;
	req.ifd_data_fd = h->ifd_fd;
	req.virt_sb_base = h->sbuf;
	req.sb_size = sb_size;
	if (ioctl(h->fd, QSEECOM_IOCTL_REGISTER_LISTENER_REQ, &req) < 0) {
		ret = -errno;
		if (errno == EEXIST) {
			LOGE("Warning::lstnr ID %d already registered\n",
			     listener_id);
			ret = -2;
		} else {
			LOGE("Error::Register listener ioctl failed for "
			     "lstnr ID %d & ret=%d\n", listener_id, -ret);
			ret = -1;
		}
		qsee_heap_free(h->heap_dev_fd, h->ifd_fd, h->sbuf,
			       page_align(sb_size));
		close(h->fd);
		free(h);
		return ret;
	}

	*handle = h;
	return 0;
}

int QSEECom_unregister_listener(struct qseecom_handle *handle)
{
	int ret;

	if (!handle || handle->fd <= 0) {
		LOGE("Error::Cannot Unregister. Handle is NULL!!.\n");
		return -1;
	}
	LOGI("QSEECom_unregister_listener %u \n", handle->app_id);
	ret = ioctl(handle->fd, QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ);
	if (ret < 0) {
		ret = -errno;
		LOGE("Error::Unregister listener ioctl failed. with ret = "
		     "%d, errno = %d\n", -ret, errno);
		return ret;
	}
	qsee_heap_free(handle->heap_dev_fd, handle->ifd_fd, handle->sbuf,
		       page_align(handle->sbuf_len));
	close(handle->fd);
	free(handle);
	return 0;
}

int __QSEECom_validate_send_cmd_inputs(struct qseecom_handle *handle,
				       const void *send_buf, uint32_t sbuf_len,
				       const void *resp_buf, uint32_t resp_len)
{
	if (!handle)
		return -1;
	if (!resp_len || !sbuf_len || !send_buf || !resp_buf ||
	    handle->sbuf_len < sbuf_len || handle->sbuf_len < resp_len)
		return -1;
	return 0;
}

/*
 * QSEECom_send_cmd - exact vendor semantics:
 *  - request copied into sb head unless the caller buffer already lies
 *    inside the sb (then the pointer is passed through);
 *  - response location defaults to sbuf + aligned(sbuf_len); caller
 *    pointer inside sb passes through;
 *  - QSEE 4.0 (version 20) rounds both lengths up to 64;
 *  - response copied out to the caller's buffer, sb wiped afterwards.
 */
int QSEECom_send_cmd(struct qseecom_handle *handle,
		     void *send_buf, uint32_t sbuf_len,
		     void *resp_buf, uint32_t resp_len)
{
	struct qsee_uapi_send_cmd_req req;
	uint8_t *sb = (uint8_t *)handle->sbuf;
	uint32_t send_len = sbuf_len, resp_loc_len = resp_len;
	uint8_t *resp_loc;
	int ret;

	if (!handle)
		goto inval;
	if (!resp_len || !sbuf_len || !send_buf || !resp_buf)
		goto inval;
	if (handle->sbuf_len < sbuf_len || handle->sbuf_len < resp_len)
		goto inval;

	if (sb > (uint8_t *)send_buf ||
	    sb + handle->sbuf_len <= (uint8_t *)send_buf) {
		memcpy(sb, send_buf, sbuf_len);
		req.cmd_req_buf = sb;
	} else {
		req.cmd_req_buf = send_buf;   /* already in sb */
	}
	if (handle->version == QSEOS_VERSION_14 &&
	    (send_len & QSEECOM_ALIGN_MASK) != 0) {
		send_len = QSEECOM_ALIGN(send_len);
	}
	resp_loc = sb + send_len;
	if (sb <= (uint8_t *)resp_buf &&
	    sb + handle->sbuf_len > (uint8_t *)resp_buf) {
		resp_loc = (uint8_t *)resp_buf;
		resp_loc_len = resp_len;
	} else if (handle->version == QSEOS_VERSION_14 &&
		   (resp_loc_len & QSEECOM_ALIGN_MASK) != 0) {
		resp_loc_len = QSEECOM_ALIGN(resp_loc_len);
	}

	req.cmd_req_len = send_len;
	req.resp_buf = resp_loc;
	req.resp_len = resp_loc_len;

	ret = ioctl(handle->fd, QSEECOM_IOCTL_SEND_CMD_REQ, &req);
	if (ret < 0) {
		ret = -errno;
		LOGE("Error::send command ioctl failed. ret = %d, errno = "
		     "%d\n", -ret, errno);
		return ret;
	}

	if (resp_loc != (uint8_t *)resp_buf) {
		if (handle->sbuf_len - send_len < resp_len) {
			LOGE("Error::rbuf_len = %d >  available space= "
			     "0x%x\n", resp_len,
			     handle->sbuf_len - send_len);
			return -1;
		}
		memcpy(resp_buf, resp_loc, resp_len);
		memset(handle->sbuf, 0, handle->sbuf_len);
	}
	return 0;

inval:
	LOGE("Error::Invalid input parameters: send_buf = 0x%p,sbuf_len = "
	     "%d,resp_buf = 0x%p,rbuf_len = %d", send_buf, sbuf_len,
	     resp_buf, resp_len);
	return -1;
}

/*
 * send_modified_cmd: identical packing to send_cmd plus up to 4 dma-buf
 * fds patched into the command buffer at caller-chosen offsets by the
 * kernel (sg-list embedding, qseecom.c:3310-3436).
 */
static int send_modified(struct qseecom_handle *handle,
			 void *send_buf, uint32_t sbuf_len,
			 void *resp_buf, uint32_t resp_len,
			 struct qseecom_ion_fd_info *ifd_data,
			 int use_64bit)
{
	struct qsee_uapi_send_modfd_cmd_req req;
	uint8_t *sb = (uint8_t *)handle->sbuf;
	uint32_t send_len = sbuf_len, resp_loc_len = resp_len;
	uint8_t *resp_loc;
	int i, ret;

	if (!handle || !resp_len || !sbuf_len || !send_buf || !resp_buf ||
	    !ifd_data || handle->sbuf_len < sbuf_len ||
	    handle->sbuf_len < resp_len) {
		LOGE("Error::Invalid input parameters: send_buf = 0x%p,"
		     "sbuf_len = %d,resp_buf = 0x%p,rbuf_len = %d",
		     send_buf, sbuf_len, resp_buf, resp_len);
		return -1;
	}

	if (sb > (uint8_t *)send_buf ||
	    sb + handle->sbuf_len <= (uint8_t *)send_buf) {
		memcpy(sb, send_buf, sbuf_len);
		req.cmd_req_buf = sb;
	} else {
		req.cmd_req_buf = send_buf;
	}
	if (handle->version == QSEOS_VERSION_14 &&
	    (send_len & QSEECOM_ALIGN_MASK) != 0)
		send_len = QSEECOM_ALIGN(send_len);
	resp_loc = sb + send_len;
	if (sb <= (uint8_t *)resp_buf &&
	    sb + handle->sbuf_len > (uint8_t *)resp_buf) {
		resp_loc = (uint8_t *)resp_buf;
	} else if (handle->version == QSEOS_VERSION_14 &&
		   (resp_loc_len & QSEECOM_ALIGN_MASK) != 0) {
		resp_loc_len = QSEECOM_ALIGN(resp_loc_len);
	}

	req.cmd_req_len = send_len;
	req.resp_buf = resp_loc;
	req.resp_len = resp_loc_len;
	for (i = 0; i < QSEECOM_MAX_ION_FD; i++) {
		req.ifd_data[i].fd = ifd_data[i].fd;
		req.ifd_data[i].cmd_buf_offset = ifd_data[i].cmd_buf_offset;
	}

	ret = ioctl(handle->fd, use_64bit ?
		    QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ :
		    QSEECOM_IOCTL_SEND_MODFD_CMD_REQ, &req);
	if (ret < 0) {
		ret = -errno;
		LOGE("Error::send modified command%s ioctl failed wtih ret "
		     "= %d, errno = %d\n", use_64bit ? "_64" : "", -ret,
		     errno);
		return ret;
	}

	if (resp_loc != (uint8_t *)resp_buf) {
		if (handle->sbuf_len - send_len < resp_len) {
			LOGE("Error::rbuf_len = %d >  available space= 0x%x\n",
			     resp_len, handle->sbuf_len - send_len);
			return -1;
		}
		memcpy(resp_buf, resp_loc, resp_len);
		memset(handle->sbuf, 0, handle->sbuf_len);
	}
	return 0;
}

int __QSEECom_send_modified_cmd(struct qseecom_handle *handle,
				void *send_buf, uint32_t sbuf_len,
				void *resp_buf, uint32_t resp_len,
				struct qseecom_ion_fd_info *ifd_data,
				int use_64bit)
{
	return send_modified(handle, send_buf, sbuf_len, resp_buf, resp_len,
			     ifd_data, use_64bit);
}

int QSEECom_send_modified_cmd(struct qseecom_handle *handle,
			      void *send_buf, uint32_t sbuf_len,
			      void *resp_buf, uint32_t resp_len,
			      struct qseecom_ion_fd_info *ifd_data)
{
	return send_modified(handle, send_buf, sbuf_len, resp_buf, resp_len,
			     ifd_data, 0);
}

int QSEECom_send_modified_cmd_64(struct qseecom_handle *handle,
				 void *send_buf, uint32_t sbuf_len,
				 void *resp_buf, uint32_t resp_len,
				 struct qseecom_ion_fd_info *ifd_data)
{
	return send_modified(handle, send_buf, sbuf_len, resp_buf, resp_len,
			     ifd_data, 1);
}

/* Listener-side response with fd patching. */
int __QSEECom_send_modified_resp(struct qseecom_handle *handle,
				 void *resp_buf, uint32_t resp_len,
				 struct qseecom_ion_fd_info *ifd_data,
				 int use_64bit)
{
	struct qsee_uapi_send_modfd_listener_resp resp;
	uint8_t *sb = (uint8_t *)handle->sbuf;
	int i, ret;

	if (!handle || !resp_buf || !resp_len || !ifd_data) {
		LOGE("Invalid input parameters: send_buf = 0x%p,sbuf_len = "
		     "%d", resp_buf, resp_len);
		return -1;
	}

	if (sb > (uint8_t *)resp_buf ||
	    sb + handle->sbuf_len <= (uint8_t *)resp_buf) {
		memcpy(sb, resp_buf, resp_len);
		resp.resp_buf_ptr = sb;
	} else {
		resp.resp_buf_ptr = resp_buf;
	}
	resp.resp_len = resp_len;
	for (i = 0; i < QSEECOM_MAX_ION_FD; i++) {
		resp.ifd_data[i].fd = ifd_data[i].fd;
		resp.ifd_data[i].cmd_buf_offset = ifd_data[i].cmd_buf_offset;
	}

	ret = ioctl(handle->fd, use_64bit ? QSEECOM_IOCTL_SEND_MODFD_RESP_64 :
					   QSEECOM_IOCTL_SEND_MODFD_RESP,
		    &resp);
	if (ret < 0)
		LOGE("send modified resp%s ioctl failed, ret = %d, errno = "
		     "%d\n", use_64bit ? " 64" : "", -1, errno);
	return ret < 0 ? -errno : 0;
}

int QSEECom_send_modified_resp(struct qseecom_handle *handle,
			       void *resp_buf, uint32_t resp_len,
			       struct qseecom_ion_fd_info *ifd_data)
{
	return __QSEECom_send_modified_resp(handle, resp_buf, resp_len,
					    ifd_data, 0);
}

int QSEECom_send_modified_resp_64(struct qseecom_handle *handle,
				  void *resp_buf, uint32_t resp_len,
				  struct qseecom_ion_fd_info *ifd_data)
{
	return __QSEECom_send_modified_resp(handle, resp_buf, resp_len,
					    ifd_data, 1);
}

int QSEECom_receive_req(struct qseecom_handle *handle,
			void *rcv_buf, uint32_t len)
{
	int ret;

	if (!handle || !rcv_buf || !len) {
		LOGE("Error::Invalid input parameters: rcv_buf = 0x%p,len = "
		     "%d", rcv_buf, len);
		return -1;
	}
	ret = ioctl(handle->fd, QSEECOM_IOCTL_RECEIVE_REQ);
	if (ret < 0) {
		ret = -errno;
		if (ret == -512) {
			/* ERESTARTSYS: vendor callers spin on this. */
			LOGE("Error::receive req ioctl failed with ret = %d,"
			     " errno = %d\n", ret, errno);
			return ret;
		}
		LOGE("Error::receive request ioctl failed with ret = %d, "
		     "errno = %d\n", -ret, errno);
		return ret;
	}
	/* QSEE 4.0: data lives at the sb head; hand a copy out. */
	if (handle->version == QSEOS_VERSION_14) {
		if (handle->sbuf_len < len) {
			LOGE("Error: len = %d > share buf_len = %d\n", len,
			     handle->sbuf_len);
			return -1;
		}
		if (rcv_buf != handle->sbuf) {
			memcpy(rcv_buf, handle->sbuf, len);
			memset(handle->sbuf, 0, handle->sbuf_len);
		}
	}
	return 0;
}

int QSEECom_send_resp(struct qseecom_handle *handle, void *buf, uint32_t len)
{
	int ret;

	if (!handle || !buf || !len || handle->sbuf_len < len) {
		LOGE("Error::Invalid input parameters: send_buf = 0x%p, "
		     "len = %d", buf, len);
		return -1;
	}
	if ((uint8_t *)handle->sbuf > (uint8_t *)buf ||
	    handle->sbuf + handle->sbuf_len <= (uint8_t *)buf)
		memcpy(handle->sbuf, buf, len);
	if (handle->version == 19)
		*(uint32_t *)handle->sbuf = 0;
	ret = ioctl(handle->fd, QSEECOM_IOCTL_SEND_RESP_REQ);
	if (ret < 0)
		LOGE("Error::send response request ioctl failed with ret = "
		     "%d, errno = %d\n", -1, errno);
	return ret < 0 ? -errno : 0;
}

int __QSEECom_issue_send_service_cmd(struct qseecom_handle *handle,
				     void *send_buf, uint32_t sbuf_len,
				     void *resp_buf, uint32_t resp_len,
				     uint32_t cmd_id)
{
	struct qsee_uapi_send_svc_cmd_req req;
	uint32_t send_len = sbuf_len, resp_len_loc = resp_len;
	int ret;

	if (!handle) {
		LOGE("priv handle is NULL!!.\n");
		return -1;
	}
	if (handle->version == QSEOS_VERSION_14) {
		if ((sbuf_len & QSEECOM_ALIGN_MASK) != 0)
			send_len = QSEECOM_ALIGN(sbuf_len);
		if ((resp_len & QSEECOM_ALIGN_MASK) != 0)
			resp_len_loc = QSEECOM_ALIGN(resp_len);
	}
	if (send_len > handle->sbuf_len) {
		LOGE("Error: cmd_req_len = %d > ion share buf_len = %d\n",
		     send_len, handle->sbuf_len);
		return 0;    /* vendor returns 0 in this branch */
	}

	memset(&req, 0, sizeof(req));
	req.cmd_id = cmd_id;
	req.cmd_req_buf = handle->sbuf;
	req.cmd_req_len = send_len;
	req.resp_buf = handle->sbuf + send_len;
	req.resp_len = resp_len_loc;
	memcpy(handle->sbuf, send_buf, sbuf_len);

	ret = ioctl(handle->fd, QSEECOM_IOCTL_SEND_CMD_SERVICE_REQ, &req);
	if (ret < 0) {
		ret = -errno;
		LOGE("Error::send service command ioctl failed. ret = %d, "
		     "errno = %d\n", -ret, errno);
	} else if (resp_buf) {
		if (resp_len_loc + send_len > handle->sbuf_len) {
			LOGE("Error::req.resp_len = %d >  available space= "
			     "%d\n", resp_len, handle->sbuf_len);
			return ret;
		}
		memcpy(resp_buf, handle->sbuf + send_len, resp_len);
	}
	return ret < 0 ? ret : 0;
}

int QSEECom_send_service_cmd(void *send_buf, uint32_t sbuf_len,
			     void *resp_buf, uint32_t resp_len,
			     uint32_t cmd_id)
{
	struct qseecom_handle *h = NULL;
	uint32_t sb_len;
	uint8_t inline_req[4];
	void *req_buf = send_buf;
	uint32_t req_len = sbuf_len;
	int ret;

	/* Argument order verified from the disassembly: cmd_id is the
	 * fifth parameter (REPORT.md Q4). */
	switch (cmd_id) {
	case 15:    /* QSEOS_RPMB_PROVISION_KEY_COMMAND */
	case 16:    /* QSEOS_RPMB_ERASE_COMMAND */
		if (!send_buf || !sbuf_len)
			return -1;
		sb_len = sbuf_len;
		break;
	case 27:    /* QSEOS_RPMB_CHECK_PROV_STATUS_COMMAND */
		if (!resp_buf || !resp_len)
			return -1;
		req_buf = inline_req;
		req_len = 4;
		sb_len = resp_len;
		break;
	case 0x109: case 0x203: case 0x301: case 0x403:
		if (!send_buf || !sbuf_len)
			return -1;
		sb_len = sbuf_len;
		break;
	case 0x110: case 0x204: case 0x302: case 0x404:
		if (!send_buf || !sbuf_len || !resp_buf || !resp_len)
			return -1;
		sb_len = sbuf_len + resp_len;
		break;
	default:
		LOGE("Error: Unsupported CMD_ID %d", cmd_id);
		return -1;
	}

	if (__QSEECom_get_handle(&h, 1, NULL, NULL, NULL, 0, sb_len, 0)
			!= 0) {
		LOGE("QSEECom_get_ion_handle failed, exiting\n");
		return -1;
	}
	ret = __QSEECom_issue_send_service_cmd(h, req_buf, req_len, resp_buf,
					       resp_len, cmd_id);
	/* One-shot: release the temporary handle without UNLOAD_APP. */
	if (h) {
		teardown_client_sb(h);
		close(h->fd);
		free(h);
	}
	return ret;
}

int QSEECom_set_bandwidth(struct qseecom_handle *handle, bool high)
{
	int ret;

	if (!handle)
		return -1;
	ret = ioctl(handle->fd, high ? QSEECOM_IOCTL_PERF_ENABLE_REQ :
				       QSEECOM_IOCTL_PERF_DISABLE_REQ);
	if (ret < 0)
		LOGE("Error::ioctl call to enable/disable performance "
		     "failed with ret = %d, errno = %d\n", -1, errno);
	return ret < 0 ? -errno : 0;
}

int QSEECom_scale_bus_bandwidth(struct qseecom_handle *handle, int level)
{
	int ret;

	if (!handle) {
		LOGE("priv handle is NULL!!.\n");
		return -1;
	}
	ret = ioctl(handle->fd, QSEECOM_IOCTL_SET_BUS_SCALING_REQ, &level);
	if (ret < 0)
		LOGE("Error::ioctl call to scale bus bandwidth to level %d "
		     "failed with ret = %d, errno = %d\n", level, -1,
		     errno);
	return ret < 0 ? -errno : 0;
}

/* Key management ioctls run on a dedicated /dev/qseecom fd per call,
 * exactly like the vendor functions. */
int QSEECom_create_key(enum qseecom_key_management_usage_type usage,
		       uint8_t hash32[QSEECOM_HASH_SIZE])
{
	struct qsee_uapi_create_key_req req;
	int fd, ret;

	if (usage < QSEOS_KM_USAGE_DISK_ENCRYPTION ||
	    usage > QSEOS_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION) {
		LOGE("Error:: unsupported usage %d\n", usage);
		return -7;
	}
	fd = qsee_open_qseecom_dev();
	if (fd < 0)
		return -7;
	memset(&req, 0, sizeof(req));
	if (hash32)
		memcpy(req.hash32, hash32, QSEECOM_HASH_SIZE);
	req.usage = usage;
	ret = ioctl(fd, QSEECOM_IOCTL_CREATE_KEY_REQ, &req);
	if (ret < 0) {
		int e = errno;

		LOGE("Error::ioctl call to create encryption key for usage "
		     "%d failed with ret = %d, errno = %d\n", usage, -1, e);
		ret = (e == ERANGE) ? -10 : -7;
	}
	close(fd);
	return ret;
}

int __QSEECom_wipe_clear_key(enum qseecom_key_management_usage_type usage,
			     int wipe_flag)
{
	struct qsee_uapi_wipe_key_req req;
	int fd, ret;

	if (usage < QSEOS_KM_USAGE_DISK_ENCRYPTION ||
	    usage > QSEOS_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION) {
		LOGE("Error:: unsupported usage %d\n", usage);
		return -1;
	}
	fd = qsee_open_qseecom_dev();
	if (fd < 0)
		return -1;
	req.usage = usage;
	req.wipe_key_flag = wipe_flag;
	ret = ioctl(fd, QSEECOM_IOCTL_WIPE_KEY_REQ, &req);
	close(fd);
	return ret < 0 ? -errno : 0;
}

int QSEECom_wipe_key(enum qseecom_key_management_usage_type usage)
{
	if (usage < QSEOS_KM_USAGE_DISK_ENCRYPTION ||
	    usage > QSEOS_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION) {
		LOGE("Error:: unsupported usage %d\n", usage);
		return -1;
	}
	if (__QSEECom_wipe_clear_key(usage, 1) == 0) {
		LOGI("SUCCESS::ioctl call to wipe the encryption key for "
		     "usage %d success with ret = %d\n", usage, 0);
		return 0;
	}
	return -8;
}

int QSEECom_clear_key(enum qseecom_key_management_usage_type usage)
{
	if (usage < QSEOS_KM_USAGE_DISK_ENCRYPTION ||
	    usage > QSEOS_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION) {
		LOGE("Error:: unsupported usage %d\n", usage);
		return -1;
	}
	if (__QSEECom_wipe_clear_key(usage, 0) == 0)
		return 0;
	return -11;
}

int QSEECom_update_key_user_info(enum qseecom_key_management_usage_type usage,
				 uint8_t current_hash32[QSEECOM_HASH_SIZE],
				 uint8_t new_hash32[QSEECOM_HASH_SIZE])
{
	struct qsee_uapi_update_key_userinfo_req req;
	int fd, ret;

	if (usage < QSEOS_KM_USAGE_DISK_ENCRYPTION ||
	    usage > QSEOS_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION) {
		LOGE("Error:: unsupported usage %d\n", usage);
		return -9;
	}
	fd = qsee_open_qseecom_dev();
	if (fd < 0)
		return -9;
	memset(&req, 0, sizeof(req));
	if (current_hash32)
		memcpy(req.current_hash32, current_hash32, QSEECOM_HASH_SIZE);
	if (new_hash32)
		memcpy(req.new_hash32, new_hash32, QSEECOM_HASH_SIZE);
	req.usage = usage;
	ret = ioctl(fd, QSEECOM_IOCTL_UPDATE_KEY_USER_INFO_REQ, &req);
	if (ret < 0) {
		int e = errno;

		LOGE("Error::ioctl call to update the encryption key for "
		     "usage %d failed with ret = %d, errno = %d\n", usage,
		     -1, e);
		ret = (e == ERANGE) ? -10 : -9;
	}
	close(fd);
	return ret;
}

int QSEECom_get_app_info(struct qseecom_handle *handle,
			 struct qseecom_app_info *info)
{
	if (!handle) {
		LOGE("priv handle is NULL!!.\n");
		return -1;
	}
	if (handle->app_arch == 2) {
		info->is_64bit = 1;
		info->app_capacity = 0xC000;
		return 0;
	}
	if (handle->app_arch == 1) {
		info->is_64bit = 0;
		info->app_capacity = 0x8000;
		return 0;
	}
	LOGE("Invalid app arch type %d!!.\n", handle->app_arch);
	return -1;
}
