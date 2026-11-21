/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qsee_dma_heap.c - dma-buf heaps based shared-buffer allocator.
 *
 * Replaces the vendor __QSEECom_ION_memalloc()/__QSEECom_ION_memfree()
 * pair (vendor .so @0x3cec / @0x3f2c) which used /dev/ion with
 * ION_IOC_ALLOC(heap 27 = "QSEECOM HEAP", phys-continuous qseecom_mem
 * region) + ION_IOC_MAP + mmap.
 *
 * The 7.3 kernel (CONFIG_DMABUF_HEAPS_SYSTEM=y + DMABUF_HEAPS_CMA=y)
 * exposes the equivalent allocation sources as dma-buf heaps; cma_heap
 * enumerates every CMA area under its DT name (REPORT.md section 0):
 *
 *   /dev/dma_heap/qseecom_ta_region
 *       16MiB reusable CMA reserved-memory region (DT qseecom_ta_mem,
 *       pbem00/sdm670.dtsi).  Successor of the 4.9 QSEECOM ION heap
 *       role: FIRST CHOICE for every allocation the driver must see as
 *       one physical run (LOAD_APP / LOAD_EXTERNAL_ELF image fds,
 *       REGISTER_LISTENER sb fd, SET_MEM_PARAM client sb fd -- the
 *       driver rejects nents > 1 for all of those).
 *
 *   /dev/dma_heap/default_cma_region
 *       32MiB linux,cma (linux,cma-default); also the backing store of
 *       the driver's own dma_alloc_coherent() work buffers.  Fallback
 *       for the contiguous consumers above when qseecom_ta_region is
 *       absent or exhausted.
 *
 *   /dev/dma_heap/system
 *       May return non-contiguous buffers.  Only usable for the
 *       discrete fds embedded via SEND_MODFD ifd_data[] (allocated by
 *       callers, not by this library), never for image/sb fds.
 *
 * Selection policy (unless overridden with QSEE_DMA_HEAP=<name>):
 *   contiguous consumers: qseecom_ta_region -> default_cma_region.
 *
 * Every allocation is retried QSEE_TA_ALLOC_MAX_ATTEMPTS times with
 * QSEE_TA_ALLOC_DELAY_MS spacing per heap because CMA can be
 * momentarily fragmented; this mirrors the retry knobs the 4.9 UAPI
 * shipped (QSEECOM_TA_ION_ALLOCATE_DELAY / _MAX_ATTEMP,
 * qseecom.h:12-13).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "qsee_internal.h"

static const char *const qsee_dma_heap_names[QSEE_HEAP_NUM] = {
	[QSEE_HEAP_CMA_DEFAULT] = "default_cma_region",
	[QSEE_HEAP_CMA_TA]      = "qseecom_ta_region",
	[QSEE_HEAP_SYSTEM]      = "system",
};

const char *qsee_heap_name(int heap_idx)
{
	if (heap_idx < 0 || heap_idx >= QSEE_HEAP_NUM)
		return "invalid";
	return qsee_dma_heap_names[heap_idx];
}

static int heap_index_from_env(void)
{
	const char *env = getenv("QSEE_DMA_HEAP");
	int i;

	if (!env || !env[0])
		return -1;
	for (i = 0; i < QSEE_HEAP_NUM; i++) {
		if (strcmp(env, qsee_dma_heap_names[i]) == 0)
			return i;
	}
	return -1;
}

/*
 * Single-heap allocation attempt.  Returns 0 on success, -ENOENT when
 * the heap device node does not exist (so callers can walk a fallback
 * chain), or another -errno after exhausting the retry budget.
 */
static int heap_alloc_once(int heap_idx, size_t len, int *dma_heap_fd,
			   int *dmabuf_fd, void **mmap_addr)
{
	char path[128];
	struct dma_heap_allocation_data alloc = { 0 };
	int attempt;
	int dev_fd = -1;
	int buf_fd = -1;
	void *va = MAP_FAILED;
	size_t map_len = (len + 4095) & ~(size_t)0xFFF;
	int rc = -EINVAL;

	*dma_heap_fd = -1;
	*dmabuf_fd = -1;
	*mmap_addr = NULL;

	snprintf(path, sizeof(path), "/dev/dma_heap/%s",
		 qsee_heap_name(heap_idx));

	dev_fd = open(path, O_RDWR | O_CLOEXEC);
	if (dev_fd < 0) {
		rc = -errno;
		if (rc == -ENOENT || rc == -ENODEV || rc == -EACCES)
			return -ENOENT;    /* heap not present: skip over */
		fprintf(stderr,
			"libQSEECom: open %s failed: %s\n", path,
			strerror(errno));
		return rc;
	}

	for (attempt = 0; attempt < QSEE_TA_ALLOC_MAX_ATTEMPTS; attempt++) {
		alloc.len = map_len;
		alloc.fd = 0;
		alloc.fd_flags = O_RDWR | O_CLOEXEC;
		alloc.heap_flags = 0;

		if (ioctl(dev_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
			rc = -errno;
			usleep(QSEE_TA_ALLOC_DELAY_MS * 1000);
			continue;
		}

		buf_fd = (int)alloc.fd;
		va = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
			  buf_fd, 0);
		if (va == MAP_FAILED) {
			rc = -errno;
			close(buf_fd);
			buf_fd = -1;
			usleep(QSEE_TA_ALLOC_DELAY_MS * 1000);
			continue;
		}

		*dma_heap_fd = dev_fd;
		*dmabuf_fd = buf_fd;
		*mmap_addr = va;
		return 0;
	}

	fprintf(stderr,
		"libQSEECom: alloc %zu bytes from heap %s failed after %d "
		"attempts: %s\n",
		len, qsee_heap_name(heap_idx), attempt, strerror(-rc));
	close(dev_fd);
	return rc;
}

int qsee_heap_alloc(int heap_idx, size_t len, int *dma_heap_fd,
		    int *dmabuf_fd, void **mmap_addr)
{
	if (!dma_heap_fd || !dmabuf_fd || !mmap_addr || !len)
		return -EINVAL;
	if (heap_idx < 0)
		heap_idx = heap_index_from_env();
	if (heap_idx < 0)
		heap_idx = QSEE_HEAP_CMA_TA;
	return heap_alloc_once(heap_idx, len, dma_heap_fd, dmabuf_fd,
			       mmap_addr);
}

/*
 * Contiguous-consumer policy: qseecom_ta_region first (successor of the
 * 4.9 QSEECOM ION heap 27 role), then default_cma_region.  An explicit
 * QSEE_DMA_HEAP= pin selects exactly one heap and disables the chain.
 */
int qsee_heap_alloc_contig(size_t len, int *dma_heap_fd, int *dmabuf_fd,
			   void **mmap_addr)
{
	static const int chain[] = { QSEE_HEAP_CMA_TA, QSEE_HEAP_CMA_DEFAULT };
	int rc = -EINVAL;
	size_t i;

	if (!dma_heap_fd || !dmabuf_fd || !mmap_addr || !len)
		return -EINVAL;

	if (heap_index_from_env() >= 0)
		return qsee_heap_alloc(-1, len, dma_heap_fd, dmabuf_fd,
				       mmap_addr);

	for (i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
		rc = heap_alloc_once(chain[i], len, dma_heap_fd, dmabuf_fd,
				     mmap_addr);
		if (rc == 0)
			return 0;
		if (rc != -ENOENT)
			break;    /* heap present but allocation failed */
	}

	fprintf(stderr,
		"libQSEECom: no contiguous dma-heap could serve %zu bytes "
		"(tried %s, %s); system heap is NOT eligible for "
		"image/sb fds (driver nents==1 check)\n",
		len, qsee_heap_name(chain[0]), qsee_heap_name(chain[1]));
	return rc;
}

void qsee_heap_free(int dma_heap_fd, int dmabuf_fd, void *mmap_addr,
		    size_t len)
{
	size_t map_len = (len + 4095) & ~(size_t)0xFFF;

	if (mmap_addr && mmap_addr != MAP_FAILED)
		munmap(mmap_addr, map_len);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	if (dma_heap_fd >= 0)
		close(dma_heap_fd);
}
