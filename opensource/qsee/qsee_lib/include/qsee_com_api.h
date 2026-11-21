/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qsee_com_api.h - Rebuilt public ABI for libQSEEComAPI.so (PBEM00 / SDM670).
 *
 * This header reproduces the exported contract of the vendor
 * vendor/lib64/libQSEEComAPI.so (lineage-20 blob). Symbol names, argument
 * orders and the layout of struct qseecom_handle are ABI-frozen; see
 * REPORT.md section Q4 for the offsets extracted from the vendor binary.
 *
 * The implementation behind these declarations talks to the 4.9-ABI
 * /dev/qseecom character driver and allocates every shared buffer from
 * upstream dma-buf heaps (/dev/dma_heap/...) instead of the removed
 * /dev/ion interface.
 */
#ifndef QSEE_COM_API_H
#define QSEE_COM_API_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QSEECOM_MAX_ION_FD          4   /* MAX_ION_FD in linux/qseecom.h */
#define QSEECOM_MAX_APP_NAME_SIZE   64  /* MAX_APP_NAME_SIZE             */
#define QSEECOM_HASH_SIZE           32  /* QSEECOM_HASH_SIZE             */
#define QSEECOM_ALIGN_SIZE          0x40
#define QSEECOM_ALIGN_MASK          (QSEECOM_ALIGN_SIZE - 1)
#define QSEECOM_ALIGN(x)            (((x) + QSEECOM_ALIGN_MASK) & \
                                     (~QSEECOM_ALIGN_MASK))

/* qseos version reported by the 7.3 ported driver (qseecom.c:8585). */
#define QSEOS_VERSION_14 0x14

enum qseecom_key_management_usage_type {
	QSEOS_KM_USAGE_DISK_ENCRYPTION         = 0x01,
	QSEOS_KM_USAGE_FILE_ENCRYPTION         = 0x02,
	QSEOS_KM_USAGE_UFS_ICE_DISK_ENCRYPTION = 0x03,
	QSEOS_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION = 0x04,
};

/*
 * struct qseecom_handle - ABI-frozen client handle (40 bytes).
 * Field offsets verified against the vendor binary (REPORT.md Q4):
 *   sbuf(0x00) sbuf_len(0x08) app_id(0x0C) fd(0x10)
 *   heap_dev_fd(0x14) ifd_fd(0x18) version(0x1C) reserved(0x20) app_arch(0x24)
 */
struct qseecom_handle {
	unsigned char *sbuf;            /* 0x00: mmap VA of the shared buffer */
	uint32_t sbuf_len;              /* 0x08 */
	uint32_t app_id;                /* 0x0C */
	int32_t  fd;                    /* 0x10: /dev/qseecom fd              */
	int32_t  heap_dev_fd;           /* 0x14: /dev/dma_heap/<name> fd      */
	int32_t  ifd_fd;                /* 0x18: dma-buf fd of the sb         */
	uint32_t version;               /* 0x1C: GET_QSEOS_VERSION value      */
	uint32_t heap_id;               /* 0x20: allocation heap index (was
						 ION handle in vendor build).
						 Vendor consumers: an objdump
						 scan of all 18 libQSEEComAPI
						 consumers found no direct
						 [xN,#32] access in any
						 QSEECom-calling function
						 (REPORT OPEN-4 closed).     */
	uint32_t app_arch;              /* 0x24: ELFCLASS32 / ELFCLASS64      */
};

struct qseecom_ion_fd_info {
	int32_t fd;                     /* dma-buf fd                         */
	uint32_t cmd_buf_offset;
};

struct qseecom_app_info {
	uint8_t  is_64bit;
	uint32_t app_capacity;          /* 0x8000 for 32-bit, 0xC000 for 64-bit */
};

/* ------------------------------------------------------------------ */
/* Exported API (symbol set identical to the vendor .so)               */
/* ------------------------------------------------------------------ */

/*
 * Load a TA and create a client context.
 *   base_dir: directory holding <app_name>.mdt/.bXX (e.g.
 *             "/vendor/firmware_mnt/image")
 *   app_name: TA name without extension ("keymaster64")
 *   sb_size:  requested shared buffer size
 */
int QSEECom_start_app(struct qseecom_handle **handle, const char *base_dir,
		      const char *app_name, uint32_t sb_size);

/*
 * Load a TA from an in-memory image (ELF segments concatenated exactly as
 * the on-disk .mdt+.bXX concatenation) and create a client context.
 */
int QSEECom_start_app_V2(struct qseecom_handle **handle, const char *app_name,
			 const uint8_t *app_image, uint32_t image_len,
			 int version);

int QSEECom_shutdown_app(struct qseecom_handle **handle);

int QSEECom_app_load_query(struct qseecom_handle *handle,
			   const char *app_name);

int QSEECom_load_external_elf(struct qseecom_handle **handle,
			      const char *base_dir, const char *elf_name);
int QSEECom_unload_external_elf(struct qseecom_handle **handle);

/*
 * Register a TZ listener.  sb_size bytes of dma-buf memory are allocated,
 * mmapped and handed to the kernel together with the mapping address.
 * Returns 0 on success, -2 when the listener id is already registered.
 *
 * The 4th argument is the vendor's "hierarchy" parameter (ColorOS 7.1
 * qseecomd .rodata string: "Listener: index = %d, hierarchy = %d").  It
 * is a dead parameter: the kernel UAPI struct
 * qseecom_register_listener_req has exactly four fields (listener_id /
 * ifd_data_fd / virt_sb_base / sb_size) and no slot for it, and every
 * vendor caller passes 0.  Kept in the signature for source-level
 * compatibility only (REPORT OPEN-3 closed).
 */
int QSEECom_register_listener(struct qseecom_handle **handle,
			      int listener_id, uint32_t sb_size,
			      int hierarchy);
int QSEECom_unregister_listener(struct qseecom_handle *handle);

int QSEECom_send_cmd(struct qseecom_handle *handle,
		     void *send_buf, uint32_t sbuf_len,
		     void *resp_buf, uint32_t resp_len);

int QSEECom_send_modified_cmd(struct qseecom_handle *handle,
			      void *send_buf, uint32_t sbuf_len,
			      void *resp_buf, uint32_t resp_len,
			      struct qseecom_ion_fd_info *ifd_data);
int QSEECom_send_modified_cmd_64(struct qseecom_handle *handle,
				 void *send_buf, uint32_t sbuf_len,
				 void *resp_buf, uint32_t resp_len,
				 struct qseecom_ion_fd_info *ifd_data);
int __QSEECom_send_modified_cmd(struct qseecom_handle *handle,
				void *send_buf, uint32_t sbuf_len,
				void *resp_buf, uint32_t resp_len,
				struct qseecom_ion_fd_info *ifd_data,
				int use_64bit);

int QSEECom_send_modified_resp(struct qseecom_handle *handle,
			       void *resp_buf, uint32_t resp_len,
			       struct qseecom_ion_fd_info *ifd_data);
int QSEECom_send_modified_resp_64(struct qseecom_handle *handle,
				  void *resp_buf, uint32_t resp_len,
				  struct qseecom_ion_fd_info *ifd_data);
int __QSEECom_send_modified_resp(struct qseecom_handle *handle,
				 void *resp_buf, uint32_t resp_len,
				 struct qseecom_ion_fd_info *ifd_data,
				 int use_64bit);

/* Listener-side blocking receive; returns -512 (ERESTARTSYS) untranslated
 * when the ioctl was interrupted, callers retry on that value. */
int QSEECom_receive_req(struct qseecom_handle *handle,
			void *rcv_buf, uint32_t len);
int QSEECom_send_resp(struct qseecom_handle *handle,
		      void *buf, uint32_t len);

/*
 * One-shot service command (RPMB provision / FSM / ENCFS).  Vendor ABI
 * verified from the disassembly: cmd_id is the FIFTH argument.
 */
int QSEECom_send_service_cmd(void *send_buf, uint32_t sbuf_len,
			     void *resp_buf, uint32_t resp_len,
			     uint32_t cmd_id);
int __QSEECom_issue_send_service_cmd(struct qseecom_handle *handle,
				     void *send_buf, uint32_t sbuf_len,
				     void *resp_buf, uint32_t resp_len,
				     uint32_t cmd_id);

int QSEECom_set_bandwidth(struct qseecom_handle *handle, bool high);
int QSEECom_scale_bus_bandwidth(struct qseecom_handle *handle, int level);

int QSEECom_create_key(enum qseecom_key_management_usage_type usage,
		       uint8_t hash32[QSEECOM_HASH_SIZE]);
int QSEECom_clear_key(enum qseecom_key_management_usage_type usage);
int QSEECom_wipe_key(enum qseecom_key_management_usage_type usage);
int __QSEECom_wipe_clear_key(enum qseecom_key_management_usage_type usage,
			     int wipe_flag);
int QSEECom_update_key_user_info(enum qseecom_key_management_usage_type usage,
				 uint8_t current_hash32[QSEECOM_HASH_SIZE],
				 uint8_t new_hash32[QSEECOM_HASH_SIZE]);

int QSEECom_get_app_info(struct qseecom_handle *handle,
			 struct qseecom_app_info *info);

/* Internal helpers exported by the vendor .so as well.
 * __QSEECom_get_handle arg order mirrors the vendor binary:
 * (handle_out, mode, base_dir, app_name, image, image_len, sb_size, mode2)
 * mode 1 = file-based load, mode 2 = in-memory trustlet. */
int __QSEECom_get_handle(struct qseecom_handle **handle, int unused,
			 const char *base_dir, const char *app_name,
			 const uint8_t *app_image, uint32_t image_len,
			 uint32_t sb_size, int mode);

int __QSEECom_validate_send_cmd_inputs(struct qseecom_handle *handle,
				       const void *send_buf, uint32_t sbuf_len,
				       const void *resp_buf, uint32_t resp_len);

#ifdef __cplusplus
}
#endif

#endif /* QSEE_COM_API_H */
