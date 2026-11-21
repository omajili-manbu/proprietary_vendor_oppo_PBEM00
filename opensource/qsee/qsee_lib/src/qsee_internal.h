/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qsee_internal.h - internal declarations of the rebuilt libQSEEComAPI.
 *
 * Kernel UAPI structures are re-declared here with the exact natural
 * alignment of include/uapi/linux/qseecom.h (aarch64 LP64).  Do NOT pack
 * them: e.g. qseecom_send_modfd_listener_resp places ifd_data at offset
 * 12 and is 48 bytes only under natural alignment, which is what the
 * ioctl size encoding expects.
 */
#ifndef QSEE_INTERNAL_H
#define QSEE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "qsee_com_api.h"

/* ------------------------------------------------------------------ */
/* /dev/qseecom UAPI (mirrors include/uapi/linux/qseecom.h)            */
/* ------------------------------------------------------------------ */

struct qsee_uapi_register_listener_req {
	uint32_t listener_id;
	int32_t  ifd_data_fd;
	void    *virt_sb_base;
	uint32_t sb_size;
};                              /* 24 bytes, ifd@4, virt_sb_base@8 */

struct qsee_uapi_send_cmd_req {
	void    *cmd_req_buf;
	uint32_t cmd_req_len;
	void    *resp_buf;
	uint32_t resp_len;
};                              /* 32 bytes: buf@0, len@8, buf@16, len@24 */

struct qsee_uapi_ion_fd_info {
	int32_t  fd;
	uint32_t cmd_buf_offset;
};                              /* 8 bytes */

struct qsee_uapi_send_modfd_cmd_req {
	void    *cmd_req_buf;
	uint32_t cmd_req_len;
	void    *resp_buf;
	uint32_t resp_len;
	struct qsee_uapi_ion_fd_info ifd_data[QSEECOM_MAX_ION_FD];
};                              /* 64 bytes: ifd_data@28 */

struct qsee_uapi_load_img_req {
	uint32_t mdt_len;
	uint32_t img_len;
	int32_t  ifd_data_fd;
	char     img_name[QSEECOM_MAX_APP_NAME_SIZE];
	uint32_t app_arch;
	uint32_t app_id;            /* out */
};                              /* 84 bytes */

struct qsee_uapi_set_sb_mem_param_req {
	int32_t  ifd_data_fd;
	void    *virt_sb_base;
	uint32_t sb_len;
};                              /* 24 bytes: ifd@0, virt@8, len@16 */

struct qsee_uapi_qseos_version_req {
	uint32_t qseos_version;
};                              /* 4 bytes */

struct qsee_uapi_app_load_query {
	char     app_name[QSEECOM_MAX_APP_NAME_SIZE];
	uint32_t app_id;            /* out */
	uint32_t app_arch;
};                              /* 72 bytes */

struct qsee_uapi_send_svc_cmd_req {
	uint32_t cmd_id;
	void    *cmd_req_buf;
	uint32_t cmd_req_len;
	void    *resp_buf;
	uint32_t resp_len;
};                              /* 40 bytes: id@0, buf@8, len@16, buf@24, len@32 */

struct qsee_uapi_create_key_req {
	uint8_t  hash32[QSEECOM_HASH_SIZE];
	uint32_t usage;
};                              /* 36 bytes */

struct qsee_uapi_wipe_key_req {
	uint32_t usage;
	int32_t  wipe_key_flag;
};                              /* 8 bytes */

struct qsee_uapi_update_key_userinfo_req {
	uint8_t  current_hash32[QSEECOM_HASH_SIZE];
	uint8_t  new_hash32[QSEECOM_HASH_SIZE];
	uint32_t usage;
};                              /* 68 bytes */

struct qsee_uapi_send_modfd_listener_resp {
	void    *resp_buf_ptr;
	uint32_t resp_len;
	struct qsee_uapi_ion_fd_info ifd_data[QSEECOM_MAX_ION_FD];
};                              /* 48 bytes: ifd_data@12 */

/*
 * The following structures are not used by libQSEEComAPI; they are
 * mirrored from include/uapi/linux/qseecom.h of the 7.3 tree so the
 * ioctl encodings below are source-pinned instead of computed
 * (REPORT OPEN-8 closed 2026-11-21).  aarch64 LP64 natural alignment.
 */

struct qsee_uapi_save_partition_hash_req {
	int32_t  partition_id;
	uint8_t  digest[32];            /* SHA256_DIGEST_LENGTH */
};                              /* 36 bytes */

struct qsee_uapi_is_es_activated_req {
	int32_t  is_activated;          /* out */
};                              /* 4 bytes */

struct qsee_uapi_qteec_req {
	void    *req_ptr;
	uint32_t req_len;
	void    *resp_ptr;
	uint32_t resp_len;
};                              /* 32 bytes: req_ptr@0, resp_ptr@16 */

struct qsee_uapi_qteec_modfd_req {
	void    *req_ptr;
	uint32_t req_len;
	void    *resp_ptr;
	uint32_t resp_len;
	struct qsee_uapi_ion_fd_info ifd_data[QSEECOM_MAX_ION_FD];
};                              /* 64 bytes: ifd_data@28 */

struct qsee_uapi_mdtp_cipher_dip_req {
	uint8_t *in_buf;
	uint32_t in_buf_size;
	uint8_t *out_buf;
	uint32_t out_buf_size;
	uint32_t direction;             /* 0=encrypt, 1=decrypt */
};                              /* 32 bytes */

struct qsee_uapi_ce_pipe_entry {
	int32_t  valid;
	uint32_t ce_num;
	uint32_t ce_pipe_pair;
};                              /* 12 bytes */

#define QSEE_MAX_CE_PIPE_PAIR_PER_UNIT 3
#define QSEE_MAX_CE_INFO_HANDLE_SIZE   32

struct qsee_uapi_ce_info_req {
	uint8_t  handle[QSEE_MAX_CE_INFO_HANDLE_SIZE];
	uint32_t usage;
	uint32_t unit_num;
	uint32_t num_ce_pipe_entries;
	struct qsee_uapi_ce_pipe_entry
		 ce_pipe_entry[QSEE_MAX_CE_PIPE_PAIR_PER_UNIT];
};                              /* 80 bytes */

struct qsee_uapi_ice_data {
	int32_t  flag;
};                              /* 4 bytes: UAPI nr 43 has no driver case
				 * (qseecom_ioctl dispatch has no SET_ICE_INFO
				 * entry); provided for encoding parity only. */

/* Layout guards: the ioctl size encodings below depend on these. */
#define QSEE_CAT_(a, b) a##b
#define QSEE_CAT(a, b)  QSEE_CAT_(a, b)
#define QSEE_STATIC_ASSERT(cond) \
	typedef char QSEE_CAT(qsee_assert_line_, __LINE__)[(cond) ? 1 : -1]

QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_register_listener_req) == 24);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_send_cmd_req) == 32);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_send_modfd_cmd_req) == 64);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_load_img_req) == 84);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_set_sb_mem_param_req) == 24);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_app_load_query) == 72);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_send_svc_cmd_req) == 40);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_create_key_req) == 36);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_wipe_key_req) == 8);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_update_key_userinfo_req) == 68);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_send_modfd_listener_resp) == 48);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_save_partition_hash_req) == 36);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_is_es_activated_req) == 4);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_qteec_req) == 32);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_qteec_modfd_req) == 64);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_mdtp_cipher_dip_req) == 32);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_ce_pipe_entry) == 12);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_ce_info_req) == 80);
QSEE_STATIC_ASSERT(sizeof(struct qsee_uapi_ice_data) == 4);
QSEE_STATIC_ASSERT(sizeof(struct qseecom_handle) == 40);

/* ------------------------------------------------------------------ */
/* ioctl numbers, magic 0x97.  Values verified against the decompiled  */
/* vendor library; see REPORT.md Q1.                                   */
/* ------------------------------------------------------------------ */

#define QSEECOM_IOC_MAGIC 0x97

#define QSEE_IOC(dir, size, nr) \
	((int)((dir) | (((unsigned int)(size)) << 16) | (QSEECOM_IOC_MAGIC << 8) | (nr)))

#define QSEE_IOC_RWR(size, nr)  QSEE_IOC(0xC0000000u, size, nr)
#define QSEE_IOC_SIMPLE(nr)     QSEE_IOC(0x00000000u, 0, nr)

#define QSEECOM_IOCTL_REGISTER_LISTENER_REQ     QSEE_IOC_RWR(24, 1)     /* 0xC0189701 */
#define QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ   QSEE_IOC_SIMPLE(2)      /* 0x9702     */
#define QSEECOM_IOCTL_SEND_CMD_REQ              QSEE_IOC_RWR(32, 3)     /* 0xC0209703 */
#define QSEECOM_IOCTL_SEND_MODFD_CMD_REQ        QSEE_IOC_RWR(64, 4)     /* 0xC0409704 */
#define QSEECOM_IOCTL_RECEIVE_REQ               QSEE_IOC_SIMPLE(5)      /* 0x9705     */
#define QSEECOM_IOCTL_SEND_RESP_REQ             QSEE_IOC_SIMPLE(6)      /* 0x9706     */
#define QSEECOM_IOCTL_LOAD_APP_REQ              QSEE_IOC_RWR(84, 7)     /* 0xC0549707 */
#define QSEECOM_IOCTL_SET_MEM_PARAM_REQ         QSEE_IOC_RWR(24, 8)     /* 0xC0189708 */
#define QSEECOM_IOCTL_UNLOAD_APP_REQ            QSEE_IOC_SIMPLE(9)      /* 0x9709     */
#define QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ     QSEE_IOC_RWR(4, 10)     /* 0xC004970A */
#define QSEECOM_IOCTL_PERF_ENABLE_REQ           QSEE_IOC_SIMPLE(11)     /* 0x970B     */
#define QSEECOM_IOCTL_PERF_DISABLE_REQ          QSEE_IOC_SIMPLE(12)     /* 0x970C     */
#define QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ     QSEE_IOC_RWR(84, 13)    /* 0xC054970D */
#define QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ   QSEE_IOC_SIMPLE(14)     /* 0x970E     */
#define QSEECOM_IOCTL_APP_LOADED_QUERY_REQ      QSEE_IOC_RWR(72, 15)    /* 0xC048970F */
#define QSEECOM_IOCTL_SEND_CMD_SERVICE_REQ      QSEE_IOC_RWR(40, 16)    /* 0xC0289710 */
#define QSEECOM_IOCTL_CREATE_KEY_REQ            QSEE_IOC_RWR(36, 17)    /* 0xC0249711 */
#define QSEECOM_IOCTL_WIPE_KEY_REQ              QSEE_IOC_RWR(8, 18)     /* 0xC0089712 */
#define QSEECOM_IOCTL_SEND_MODFD_RESP           QSEE_IOC_RWR(48, 21)    /* 0xC0309715 */
#define QSEECOM_IOCTL_SET_BUS_SCALING_REQ       QSEE_IOC_RWR(4, 23)     /* 0xC0049717 */
#define QSEECOM_IOCTL_UPDATE_KEY_USER_INFO_REQ  QSEE_IOC_RWR(68, 24)    /* 0xC0449718 */
#define QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ     QSEE_IOC_RWR(64, 35)    /* 0xC0409723 */
#define QSEECOM_IOCTL_SEND_MODFD_RESP_64        QSEE_IOC_RWR(48, 36)    /* 0xC0309724 */

/* Defined in the kernel UAPI but unused by libQSEEComAPI (OPEN-8 closed:
 * sizes pinned from include/uapi/linux/qseecom.h of the 7.3 tree). */
#define QSEECOM_IOCTL_SAVE_PARTITION_HASH_REQ   QSEE_IOC_RWR(36, 19)    /* 0xC0249713 */
#define QSEECOM_IOCTL_IS_ES_ACTIVATED_REQ       QSEE_IOC_RWR(4, 20)     /* 0xC0049714 */
#define QSEECOM_QTEEC_IOCTL_OPEN_SESSION_REQ    QSEE_IOC_RWR(64, 30)    /* 0xC040971E */
#define QSEECOM_QTEEC_IOCTL_CLOSE_SESSION_REQ   QSEE_IOC_RWR(32, 31)    /* 0xC020971F */
#define QSEECOM_QTEEC_IOCTL_INVOKE_MODFD_CMD_REQ QSEE_IOC_RWR(64, 32)   /* 0xC0409720 */
#define QSEECOM_QTEEC_IOCTL_REQUEST_CANCELLATION_REQ QSEE_IOC_RWR(64, 33) /* 0xC0409721 */
#define QSEECOM_IOCTL_MDTP_CIPHER_DIP_REQ       QSEE_IOC_RWR(32, 34)    /* 0xC0209722 */
#define QSEECOM_IOCTL_GET_CE_PIPE_INFO          QSEE_IOC_RWR(80, 40)    /* 0xC0509728 */
#define QSEECOM_IOCTL_FREE_CE_PIPE_INFO         QSEE_IOC_RWR(80, 41)    /* 0xC0509729 */
#define QSEECOM_IOCTL_QUERY_CE_PIPE_INFO        QSEE_IOC_RWR(80, 42)    /* 0xC050972A */
#define QSEECOM_IOCTL_SET_ICE_INFO              QSEE_IOC_RWR(4, 43)     /* 0xC004972B */

/* ------------------------------------------------------------------ */
/* dma-buf heaps UAPI (linux/dma-heap.h)                               */
/* ------------------------------------------------------------------ */

#define DMA_HEAP_IOC_MAGIC 'H'

struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd;
	uint32_t fd_flags;
	uint64_t heap_flags;
};                              /* 24 bytes */

#define DMA_HEAP_IOCTL_ALLOC \
	QSEE_IOC(0xC0000000u, (int)sizeof(struct dma_heap_allocation_data), 0)
/* = 0xC0184800 */

QSEE_STATIC_ASSERT(sizeof(struct dma_heap_allocation_data) == 24);

/* ------------------------------------------------------------------ */
/* Internal helpers (hidden visibility: not part of the ABI surface)   */
/* ------------------------------------------------------------------ */

#define QSEE_INTERNAL_API_HID __attribute__((visibility("hidden")))

/* Heap selection: index into qsee_dma_heap_names[] (see qsee_dma_heap.c).
 *
 * Roles (kernel-side policy, REPORT.md section 0):
 *   QSEE_HEAP_CMA_TA       /dev/dma_heap/qseecom_ta_region - 16MiB
 *                          reusable CMA area, successor of the 4.9
 *                          QSEECOM ION heap role; FIRST CHOICE for the
 *                          contiguous fds (TA images, listener sb,
 *                          client sb).
 *   QSEE_HEAP_CMA_DEFAULT  /dev/dma_heap/default_cma_region - 32MiB
 *                          linux,cma; fallback for the same uses.
 *   QSEE_HEAP_SYSTEM       /dev/dma_heap/system - may be non-contiguous;
 *                          only fit for the discrete SEND_MODFD
 *                          ifd_data[] fds (allocated by callers), never
 *                          for image/sb fds (driver nents==1 check). */
#define QSEE_HEAP_CMA_DEFAULT   0
#define QSEE_HEAP_CMA_TA        1
#define QSEE_HEAP_SYSTEM        2
#define QSEE_HEAP_NUM           3

/* Default retry cadence, mirroring QSEECOM_TA_ION_ALLOCATE_* of the 4.9
 * UAPI header (qseecom.h:12-13). */
#define QSEE_TA_ALLOC_DELAY_MS     50
#define QSEE_TA_ALLOC_MAX_ATTEMPTS 20

/* Single-heap allocation (heap_idx < 0: QSEE_DMA_HEAP env override or
 * qseecom_ta_region default). */
int qsee_heap_alloc(int heap_idx, size_t len,
			    int *dma_heap_fd, int *dmabuf_fd,
			    void **mmap_addr) QSEE_INTERNAL_API_HID;

/* Contiguous-consumer policy: qseecom_ta_region -> default_cma_region
 * chain; QSEE_DMA_HEAP= pins one heap and disables the chain. */
int qsee_heap_alloc_contig(size_t len, int *dma_heap_fd,
				   int *dmabuf_fd, void **mmap_addr)
	QSEE_INTERNAL_API_HID;

void qsee_heap_free(int dma_heap_fd, int dmabuf_fd,
			    void *mmap_addr, size_t len) QSEE_INTERNAL_API_HID;
const char *qsee_heap_name(int heap_idx) QSEE_INTERNAL_API_HID;

int qsee_read_image_files(const char *base_dir, const char *app_name,
			  void *dst, size_t dst_len, uint32_t *mdt_len,
			  uint32_t *total_len, uint32_t *app_arch) QSEE_INTERNAL_API_HID;
int qsee_probe_image_size(const char *base_dir, const char *app_name,
			  uint32_t *total_len, uint32_t *app_arch)
	QSEE_INTERNAL_API_HID;

int qsee_open_qseecom_dev(void) QSEE_INTERNAL_API_HID;

#endif /* QSEE_INTERNAL_H */
