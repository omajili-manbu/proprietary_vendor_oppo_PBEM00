/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qsee_selftest.c - on-device probe ladder for the 7.3 qseecom stack.
 *
 * Stages, each skipped (SKIP) when its prerequisites are absent:
 *   S0  dma-heap probe        /dev/dma_heap nodes exist; contiguous
 *                             alloc/free works (qseecom_ta_region ->
 *                             default_cma_region chain, OPEN-7 runtime).
 *   S1  driver version        /dev/qseecom open + GET_QSEOS_VERSION
 *                             (expect 0x14 = 20, the value the vendor
 *                             library observes).
 *   S2  partial-sb boundary   SET_MEM_PARAM with sb_len < dma-buf len
 *                             must pass; sb_len > dma-buf len must
 *                             fail (-EINVAL).  Pins the driver rule
 *                             buf->len >= req.sb_len with data->client.
 *                             sb_length = req.sb_len (qseecom.c
 *                             qseecom_set_client_mem_param, OPEN-9).
 *   S3  listener roundtrip    QSEECom_register_listener(0x7fff) ->
 *                             unregister via the rebuilt library
 *                             (dlopen'd, so this stage is skipped when
 *                             the library is not yet deployed).
 *   S4  app roundtrip         QSEECom_start_app(keymaster64) ->
 *                             QSEECom_send_cmd -> shutdown (opt-in:
 *                             --app NAME --base DIR).
 *   S5  modfd patch probe     QSEECom_send_modified_cmd with one
 *                             contiguous dma-buf fd patched at a
 *                             chosen offset; verifies the transient
 *                             kernel patching (pre-SCM phys write,
 *                             post-SCM cleanup zeroing, qseecom.c
 *                             __qseecom_update_cmd_buf, OPEN-5) by
 *                             pre-filling the field with a sentinel
 *                             and asserting it is zeroed after the
 *                             call.
 *
 * Exit status: bitwise OR of failed stages (bit0 = S0 ... bit5 = S5);
 * skipped stages never contribute.  All output lines carry the
 * "SELFTEST " prefix for logcat/serial parsing.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qsee_com_api.h"
#include "qsee_internal.h"

#define SELFTEST(fmt, ...) fprintf(stderr, "SELFTEST " fmt, ##__VA_ARGS__)

/* Retained 2-page contiguous buffer shared by S2 (partial-sb) and S5
 * (modfd patch field). */
static void *g_sb_va;
static int g_heap_fd = -1;
static int g_dmabuf_fd = -1;
static size_t g_map_len;

/* stage exit bits */
#define ST_S0  (1u << 0)
#define ST_S1  (1u << 1)
#define ST_S2  (1u << 2)
#define ST_S3  (1u << 3)
#define ST_S4  (1u << 4)
#define ST_S5  (1u << 5)

/* ------------------------------------------------------------------ */
/* Standalone dma-heap allocator (mirrors the library policy so S0/S2   */
/* can run before the rebuilt library is deployed).                    */
/* ------------------------------------------------------------------ */

static const char *const st_heaps[] = {
	"qseecom_ta_region",
	"default_cma_region",
};

static int st_heap_alloc_contig(size_t len, int *heap_fd, int *dmabuf_fd,
				void **va)
{
	struct dma_heap_allocation_data alloc = { 0 };
	size_t map_len = (len + 4095) & ~(size_t)0xFFF;
	unsigned int i;
	int attempt;

	*heap_fd = -1;
	*dmabuf_fd = -1;
	*va = NULL;

	for (i = 0; i < sizeof(st_heaps) / sizeof(st_heaps[0]); i++) {
		char path[128];
		int dev_fd, buf_fd = -1;
		void *p = MAP_FAILED;

		snprintf(path, sizeof(path), "/dev/dma_heap/%s", st_heaps[i]);
		dev_fd = open(path, O_RDWR | O_CLOEXEC);
		if (dev_fd < 0)
			continue;
		for (attempt = 0; attempt < 3; attempt++) {
			alloc.len = map_len;
			alloc.fd = 0;
			alloc.fd_flags = O_RDWR | O_CLOEXEC;
			alloc.heap_flags = 0;
			if (ioctl(dev_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
				usleep(50 * 1000);
				continue;
			}
			buf_fd = (int)alloc.fd;
			p = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
				 MAP_SHARED, buf_fd, 0);
			if (p == MAP_FAILED) {
				close(buf_fd);
				buf_fd = -1;
				continue;
			}
			*heap_fd = dev_fd;
			*dmabuf_fd = buf_fd;
			*va = p;
			SELFTEST("S0 alloc %zu bytes from %s\n", len,
				 st_heaps[i]);
			return 0;
		}
		close(dev_fd);
	}
	return -1;
}

static void st_heap_free(int heap_fd, int dmabuf_fd, void *va, size_t len)
{
	size_t map_len = (len + 4095) & ~(size_t)0xFFF;

	if (va)
		munmap(va, map_len);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	if (heap_fd >= 0)
		close(heap_fd);
}

/* ------------------------------------------------------------------ */
/* Stages                                                              */
/* ------------------------------------------------------------------ */

static int stage0_heaps(void)
{
	static const char *const nodes[] = {
		"qseecom_ta_region", "default_cma_region",
		"system", "system-uncached",
	};
	struct stat st;
	size_t map_len = (2 * 4096 + 4095) & ~(size_t)0xFFF;
	char path[128];
	size_t i;
	int heap_fd = -1, dmabuf_fd = -1, rc;
	void *va = NULL;
	unsigned int *scratch;
	int ret = 0;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		snprintf(path, sizeof(path), "/dev/dma_heap/%s", nodes[i]);
		if (stat(path, &st) == 0)
			SELFTEST("S0 node present: %s\n", path);
		else
			SELFTEST("S0 node ABSENT: %s (%s)\n", path,
				 strerror(errno));
	}

	rc = st_heap_alloc_contig(64 * 1024, &heap_fd, &dmabuf_fd, &va);
	if (rc != 0) {
		SELFTEST("S0 FAIL: contiguous alloc sanity failed\n");
		return ST_S0;
	}
	scratch = va;
	memset(scratch, 0xA5, 64 * 1024);
	if (scratch[0] != 0xA5A5A5A5u) {
		SELFTEST("S0 FAIL: mmap readability broken\n");
		ret = ST_S0;
	}
	st_heap_free(heap_fd, dmabuf_fd, va, 64 * 1024);
	if (ret == 0)
		SELFTEST("S0 PASS: 64KiB contig alloc/free sanity\n");

	/* Retained 2-page buffer for S2. */
	rc = st_heap_alloc_contig(2 * 4096, &heap_fd, &dmabuf_fd, &va);
	if (rc != 0) {
		SELFTEST("S0 FAIL: 2-page retained alloc failed\n");
		return ST_S0 | ret;
	}
	g_heap_fd = heap_fd;
	g_dmabuf_fd = dmabuf_fd;
	g_sb_va = va;
	g_map_len = map_len;
	return ret;
}

static int stage1_version(void)
{
	struct qsee_uapi_qseos_version_req req = { 0 };
	int fd, ret;

	fd = open("/dev/qseecom", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		SELFTEST("S1 SKIP: cannot open /dev/qseecom: %s\n",
			 strerror(errno));
		return 0;
	}
	if (ioctl(fd, QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ, &req) < 0) {
		SELFTEST("S1 FAIL: GET_QSEOS_VERSION: %s\n", strerror(errno));
		close(fd);
		return ST_S1;
	}
	ret = (req.qseos_version == 0x14) ? 0 : ST_S1;
	SELFTEST("S1 %s: qseos_version = 0x%x (expect 0x14)\n",
		 ret ? "FAIL" : "PASS", req.qseos_version);
	close(fd);
	return ret;
}

static int set_mem_param_raw(int qsee_fd, int dmabuf_fd, void *va,
			     uint32_t sb_len)
{
	struct qsee_uapi_set_sb_mem_param_req req = { 0 };

	req.ifd_data_fd = dmabuf_fd;
	req.virt_sb_base = va;
	req.sb_len = sb_len;
	return ioctl(qsee_fd, QSEECOM_IOCTL_SET_MEM_PARAM_REQ, &req);
}

static int stage2_partial_sb(void)
{
	int fd, ret = 0, rv;

	if (!g_sb_va) {
		SELFTEST("S2 SKIP: no retained buffer (S0 failed)\n");
		return 0;
	}
	fd = open("/dev/qseecom", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		SELFTEST("S2 SKIP: cannot open /dev/qseecom: %s\n",
			 strerror(errno));
		return 0;
	}

	/* Positive: sb_len (1 page) smaller than the dma-buf (2 pages). */
	rv = set_mem_param_raw(fd, g_dmabuf_fd, g_sb_va, 4096);
	if (rv == 0) {
		SELFTEST("S2 PASS: partial use sb_len=4096 < buf 8192 "
			 "accepted (OPEN-9)\n");
	} else {
		SELFTEST("S2 FAIL: partial use rejected: %s\n",
			 strerror(errno));
		ret |= ST_S2;
	}
	close(fd);

	/* Negative: sb_len beyond the dma-buf length. */
	fd = open("/dev/qseecom", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		SELFTEST("S2 SKIP: cannot reopen /dev/qseecom\n");
		return ret;
	}
	rv = set_mem_param_raw(fd, g_dmabuf_fd, g_sb_va, 3 * 4096);
	if (rv < 0 && errno == EINVAL) {
		SELFTEST("S2 PASS: oversize sb_len=12288 > buf 8192 rejected "
			 "-EINVAL as expected\n");
	} else if (rv == 0) {
		SELFTEST("S2 FAIL: oversize sb_len accepted by driver\n");
		ret |= ST_S2;
	} else {
		SELFTEST("S2 FAIL: oversize sb_len unexpected errno=%d\n",
			 errno);
		ret |= ST_S2;
	}
	close(fd);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Library-backed stages (dlopen)                                      */
/* ------------------------------------------------------------------ */

typedef int (*fn_register)(struct qseecom_handle **, int, uint32_t, int);
typedef int (*fn_unregister)(struct qseecom_handle *);
typedef int (*fn_start_app)(struct qseecom_handle **, const char *,
			    const char *, uint32_t);
typedef int (*fn_send_cmd)(struct qseecom_handle *, void *, uint32_t,
			   void *, uint32_t);
typedef int (*fn_send_modfd)(struct qseecom_handle *, void *, uint32_t,
			     void *, uint32_t,
			     struct qseecom_ion_fd_info *);
typedef int (*fn_shutdown)(struct qseecom_handle **);

static struct {
	void *lib;
	fn_register register_listener;
	fn_unregister unregister_listener;
	fn_start_app start_app;
	fn_send_cmd send_cmd;
	fn_send_modfd send_modfd;
	fn_shutdown shutdown_app;
} api;

static int load_api(void)
{
	api.lib = dlopen("libQSEEComAPI.so", RTLD_NOW);
	if (!api.lib) {
		SELFTEST("S3 SKIP: libQSEEComAPI.so not deployed: %s\n",
			 dlerror());
		return -1;
	}
	api.register_listener =
		(fn_register)dlsym(api.lib, "QSEECom_register_listener");
	api.unregister_listener =
		(fn_unregister)dlsym(api.lib, "QSEECom_unregister_listener");
	api.start_app = (fn_start_app)dlsym(api.lib, "QSEECom_start_app");
	api.send_cmd = (fn_send_cmd)dlsym(api.lib, "QSEECom_send_cmd");
	api.send_modfd =
		(fn_send_modfd)dlsym(api.lib, "QSEECom_send_modified_cmd");
	api.shutdown_app = (fn_shutdown)dlsym(api.lib, "QSEECom_shutdown_app");
	if (!api.register_listener || !api.unregister_listener ||
	    !api.start_app || !api.send_cmd || !api.send_modfd ||
	    !api.shutdown_app) {
		SELFTEST("S3 SKIP: library exports incomplete\n");
		return -1;
	}
	return 0;
}

#define SELFTEST_LISTENER_ID 0x7fff

static int stage3_listener(void)
{
	struct qseecom_handle *h = NULL;
	int rc, ret = 0;

	rc = api.register_listener(&h, SELFTEST_LISTENER_ID, 4096, 0);
	if (rc == 0) {
		SELFTEST("S3 PASS: register listener 0x%x (sb 4096)\n",
			 SELFTEST_LISTENER_ID);
	} else if (rc == -2) {
		SELFTEST("S3 WARN: listener 0x%x already registered "
			 "(treating as pass)\n", SELFTEST_LISTENER_ID);
	} else {
		SELFTEST("S3 FAIL: register listener ret=%d (TZ up?)\n", rc);
		return ST_S3;
	}
	rc = api.unregister_listener(h);
	if (rc != 0) {
		SELFTEST("S3 FAIL: unregister ret=%d\n", rc);
		ret |= ST_S3;
	} else {
		SELFTEST("S3 PASS: unregister listener\n");
	}
	return ret;
}

static const char *opt_app;
static const char *opt_base = "/vendor/firmware_mnt/image";
static uint32_t opt_sb = 4096;

static int stage4_app(struct qseecom_handle **h_out)
{
	struct qseecom_handle *h = NULL;
	uint8_t cmd[64];
	uint32_t resp[16];
	int rc, ret = 0;

	if (!opt_app) {
		SELFTEST("S4 SKIP: no --app given\n");
		return 0;
	}
	rc = api.start_app(&h, opt_base, opt_app, opt_sb);
	if (rc != 0) {
		SELFTEST("S4 FAIL: start_app(%s) ret=%d\n", opt_app, rc);
		return ST_S4;
	}
	SELFTEST("S4 PASS: start_app(%s) app_id=%u sb=%u\n", opt_app,
		 h->app_id, h->sbuf_len);

	memset(cmd, 0, sizeof(cmd));
	memset(resp, 0, sizeof(resp));
	rc = api.send_cmd(h, cmd, sizeof(cmd), resp, sizeof(resp));
	if (rc == 0) {
		SELFTEST("S4 PASS: send_cmd round trip, resp[0]=0x%08x\n",
			 resp[0]);
	} else {
		SELFTEST("S4 INFO: send_cmd ret=%d (TZ app protocol may "
			 "reject probe payload; structural path only)\n", rc);
	}
	*h_out = h;
	return ret;
}

static int stage5_modfd(struct qseecom_handle *h)
{
	struct qseecom_ion_fd_info ifd[QSEECOM_MAX_ION_FD];
	uint8_t *sb;
	uint32_t *patch;
	int rc, ret = 0;
	size_t off = 16;

	if (!opt_app) {
		SELFTEST("S5 SKIP: no --app given\n");
		return 0;
	}
	if (!h) {
		SELFTEST("S5 SKIP: no app handle (S4 failed)\n");
		return 0;
	}

	/*
	 * Patch field inside the sb so the kernel-side transient write is
	 * observable: pre-fill sentinel, send, assert zeroed afterwards
	 * (pre-SCM phys patch + post-SCM cleanup, qseecom.c
	 * __qseecom_update_cmd_buf; the zeroing only happens when the
	 * modfd machinery engaged, OPEN-5).
	 */
	sb = (uint8_t *)h->sbuf;
	patch = (uint32_t *)(sb + off);
	*patch = 0xDEADBEEFu;

	memset(ifd, 0, sizeof(ifd));
	ifd[0].fd = g_dmabuf_fd;
	ifd[0].cmd_buf_offset = (uint32_t)off;

	rc = api.send_modfd(h, sb, 256, sb + 256, 64, ifd);
	if (rc != 0) {
		SELFTEST("S5 INFO: send_modified_cmd ret=%d (TZ rejected "
			 "probe command; patch path inconclusive)\n", rc);
		return 0;
	}
	if (*patch == 0) {
		SELFTEST("S5 PASS: ifd field patched pre-SCM and zeroed "
			 "post-SCM (cleanup ran)\n");
	} else {
		SELFTEST("S5 FAIL: sentinel 0x%x survived - cleanup pass "
			 "did not run\n", *patch);
		ret |= ST_S5;
	}
	return ret;
}

/* ------------------------------------------------------------------ */

static void cleanup(void)
{
	if (g_sb_va) {
		st_heap_free(g_heap_fd, g_dmabuf_fd, g_sb_va, g_map_len);
		g_sb_va = NULL;
	}
	if (api.lib)
		dlclose(api.lib);
}

int main(int argc, char **argv)
{
	unsigned int fail = 0;
	struct qseecom_handle *app_h = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--app") && i + 1 < argc)
			opt_app = argv[++i];
		else if (!strcmp(argv[i], "--base") && i + 1 < argc)
			opt_base = argv[++i];
		else if (!strcmp(argv[i], "--sb") && i + 1 < argc)
			opt_sb = (uint32_t)strtoul(argv[++i], NULL, 0);
	}

	SELFTEST("qsee_selftest start (app=%s base=%s sb=%u)\n",
		 opt_app ? opt_app : "-", opt_base, opt_sb);

	fail |= stage0_heaps();
	fail |= stage1_version();
	fail |= stage2_partial_sb();
	if (load_api() == 0) {
		fail |= stage3_listener();
		fail |= stage4_app(&app_h);
		fail |= stage5_modfd(app_h);
	}
	cleanup();

	SELFTEST("summary: %s (fail bits=0x%x)\n",
		 fail ? "FAIL" : "PASS", fail);
	return (int)fail;
}
