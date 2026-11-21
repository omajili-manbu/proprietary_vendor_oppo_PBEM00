/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qseecomd_internal.h - rebuilt qseecomd daemon internals.
 */
#ifndef QSEECOMD_INTERNAL_H
#define QSEECOMD_INTERNAL_H

#include "qsee_com_api.h"

/*
 * One listener service.  The vendor daemon resolves init/start/close
 * entry points by dlsym() from the named shared object and always calls
 * them with the table's listener id as the single argument
 * (qseecomd .text init_service @0x2050).
 *
 * Vendor .data table byte layout (0x38-byte stride, reconstructed from
 * R_AARCH64_RELATIVE relocations): name@+0x00, listener_id/flags@+0x08,
 * so_name@+0x10, start_fn@+0x18, dlopen handle@+0x28 (runtime),
 * init_fn@+0x30.  The C struct below groups the same fields for
 * readability; the byte layout lives in REPORT.md Q3.
 */
struct qseecomd_service {
	const char *name;       /* vendor .data+0x00: log name       */
	const char *so_name;    /* vendor .data+0x10: dlopen() arg   */
	const char *init_fn;    /* vendor .data+0x30: init phase     */
	const char *start_fn;   /* vendor .data+0x18: start phase    */
	const char *close_fn;   /* vendor .data+0x20                 */
	uint32_t    listener_id;/* vendor .data+0x08: listener id    */
	uint32_t    flags;      /* phase discriminator from +0x08    */
};

/*
 * Built-in service implementations.  The vendor build resolves them
 * from vendor/lib64/<so_name> via dlopen; the rebuild prefers that
 * path and falls back to the internal stubs below so the daemon still
 * functions (with reduced protocol coverage) when a vendor library is
 * unavailable (e.g. libspl.so / libops.so, see REPORT OPEN-1).
 */
struct qseecomd_service_ops {
	int (*init)(int idx, uint32_t listener_id);
	int (*start)(int idx, uint32_t listener_id);
	void (*close)(int idx);
};

/* Load a service library (dlopen + dlsym), or produce the built-in stub
 * when the library cannot be opened.  Returns NULL on fatal failure. */
const struct qseecomd_service_ops *qseecomd_load_service(
	const struct qseecomd_service *svc, int idx, void **handle_slot);

/* property_set shim (link against libcutils on Android; weak fallback
 * that only logs on host builds). */
void qseecomd_property_set(const char *key, const char *value);

#endif /* QSEECOMD_INTERNAL_H */
