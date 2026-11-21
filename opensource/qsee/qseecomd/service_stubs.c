/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * service_stubs.c - listener service loading + built-in stubs.
 *
 * The vendor daemon dlopens each service library and dlsym()s three
 * entry points.  This rebuild keeps the dlopen path unchanged (so the
 * real vendor service libraries are used exactly like in the vendor
 * daemon, and a missing library is logged and skipped -- vendor
 * behaviour, and what the stock product did for libspl.so/libops.so
 * which never shipped, see REPORT.md OPEN-1).  A built-in generic
 * listener loop is available as an explicit opt-in via
 * QSEECOMD_BUILTIN_SERVICES=1 for lab bring-up only.
 *
 * The generic loop reproduces the observed listener pattern of all
 * vendor services (see librpmb dispatch @0x4ca4 and the identical
 * structure found in libssd/libsecureui/libdrmtime/libdrmfs/libqisl/
 * libGPreqcancel):
 *     while (1) {
 *         ret = QSEECom_receive_req(h, sbuf, sb_size);
 *         if (ret == -512) continue;         // ERESTARTSYS retry
 *         if (ret != 0) pthread_exit();
 *         ... parse command at sbuf[0], handle, respond ...
 *         QSEECom_send_resp(h, resp, resp_len);
 *     }
 *
 * Protocol details beyond the generic frame are TODO-VERIFY items: each
 * carries the evidence pointer from REPORT.md.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qseecomd_internal.h"

#define LOG_TAG "QSEECOMD"
#define LOGI(fmt, ...) fprintf(stderr, LOG_TAG ": " fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, LOG_TAG ": E " fmt, ##__VA_ARGS__)

struct stub_ctx {
	struct qseecom_handle *handle;
	uint32_t listener_id;
	uint32_t sb_size;
	pthread_t thread;
};

/* ------------------------------------------------------------------ */
/* Generic listener thread                                             */
/* ------------------------------------------------------------------ */

static void *listener_thread(void *arg)
{
	struct stub_ctx *ctx = arg;
	struct qseecom_handle *h = ctx->handle;
	int ret;

	LOGI("%s: listener thread (Listener ID = %u) started\n",
	     LOG_TAG, ctx->listener_id);

	for (;;) {
		ret = QSEECom_receive_req(h, h->sbuf, ctx->sb_size);
		while (ret == -512)
			ret = QSEECom_receive_req(h, h->sbuf, ctx->sb_size);
		if (ret != 0) {
			LOGE(" QSEECom_rcv_req failed: ret value: %d\n", ret);
			break;
		}

		/*
		 * TODO-VERIFY(cmd_dispatch): the first dword of the sb is
		 * the QSEE command id; per-service dispatch tables are
		 * only partially known:
		 *   - RPMB (0x2000): cmd 257/258/259, 20/40-byte replies
		 *     (librpmb dispatch @0x4ca4, REPORT Q3) [实拆]
		 *   - SSD (0x3000): ssd partition relay, protocol offsets
		 *     not extracted [OPEN]
		 *   - others: not extracted.
		 * This stub logs the command word and replies with a
		 * status-only frame so the driver-side flow stays alive.
		 */
		LOGI("listener %u: QSEE cmd 0x%x\n", ctx->listener_id,
		     *(const uint32_t *)h->sbuf);

		/* Status-only reply: first dword = command echo. */
		((uint32_t *)h->sbuf)[0] =
			((const uint32_t *)h->sbuf)[0];
		ret = QSEECom_send_resp(h, h->sbuf, sizeof(uint32_t));
		if (ret != 0)
			LOGE(" QSEECom_send_resp failed: ret = %d\n", ret);
	}
	return NULL;
}

static int stub_init(int idx, uint32_t listener_id)
{
	/* No init-time behaviour in the vendor build for services without
	 * an init entry point; services 0/1/2 run their library's real
	 * init when the .so is present. */
	(void)idx;
	(void)listener_id;
	return 0;
}

static int stub_start(int idx, uint32_t listener_id)
{
	struct stub_ctx *ctx;
	uint32_t sb_size;
	int ret;

	/*
	 * sb sizes observed in the vendor service libraries
	 * (REPORT.md Q3): rpmb 25600, ssd/fs/time/gpreqcancel 20480,
	 * secureui 4264, gpfs 516096, qisl 500.
	 * libspl.so / libops.so never shipped on PBEM00 (OPEN-1 closed:
	 * absent from the lineage blob and the ColorOS 7.1 vendor image),
	 * so their sb sizes are unobservable; the stub default of 20480
	 * matches the dominant 20 KiB family and only matters in
	 * QSEECOMD_BUILTIN_SERVICES=1 lab mode.
	 */
	(void)idx;
	switch (listener_id) {
	case 0x2000:  sb_size = 25600;  break;
	case 0x4000:  sb_size = 4264;   break;
	case 0x7000:  sb_size = 516096; break;
	case 0x9000:  sb_size = 500;    break;
	default:      sb_size = 20480;  break;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;
	ctx->listener_id = listener_id;
	ctx->sb_size = sb_size;

	ret = QSEECom_register_listener(&ctx->handle, (int)listener_id,
					sb_size, 0);
	if (ret == -2) {
		LOGI("listener %u already registered\n", listener_id);
		free(ctx);
		return 0;
	}
	if (ret != 0) {
		LOGE("Error: %s ioctl failed! with ret = %d\n", "register",
		     ret);
		free(ctx);
		return -1;
	}

	if (pthread_create(&ctx->thread, NULL, listener_thread, ctx) != 0) {
		LOGE("Error: Creating a pthread is failed!\n");
		QSEECom_unregister_listener(ctx->handle);
		free(ctx);
		return -1;
	}
	LOGI("%s: Started (Listener ID = %u)\n", LOG_TAG, listener_id);
	return 0;
}

static void stub_close(int idx)
{
	(void)idx;
	/* TODO-VERIFY(close_path): vendor services only unregister on
	 * daemon shutdown, which never happens (main sits in pause()).
	 * Left unimplemented on purpose. */
}

/* ------------------------------------------------------------------ */
/* dlopen-based loader with stub fallback                              */
/* ------------------------------------------------------------------ */

struct lib_ops {
	int (*init)(uint32_t listener_id);
	int (*start)(uint32_t listener_id);
	void (*close)(void);
};

struct service_lib {
	void *dl;
	struct lib_ops ops;
};

static struct service_lib libs[10];

static int lib_init(int idx, uint32_t listener_id)
{
	struct lib_ops *ops = &libs[idx].ops;

	return ops->init ? ops->init(listener_id) : 0;
}

static int lib_start(int idx, uint32_t listener_id)
{
	struct lib_ops *ops = &libs[idx].ops;

	return ops->start ? ops->start(listener_id) : -1;
}

static void lib_close(int idx)
{
	struct lib_ops *ops = &libs[idx].ops;

	/* Vendor close fns (rpmb_close/ssd_close/...) take no arguments. */
	if (ops->close)
		ops->close();
}

const struct qseecomd_service_ops *qseecomd_load_service(
	const struct qseecomd_service *svc, int idx, void **handle_slot)
{
	static const struct qseecomd_service_ops lib_ops = {
		.init = lib_init,
		.start = lib_start,
		.close = lib_close,
	};
	static const struct qseecomd_service_ops stub_ops = {
		.init = stub_init,
		.start = stub_start,
		.close = stub_close,
	};
	const char *force_stub = getenv("QSEECOMD_BUILTIN_SERVICES");
	void *dl;

	(void)handle_slot;

	/* Built-in stub is opt-in only (QSEECOMD_BUILTIN_SERVICES=1). */
	if (force_stub && force_stub[0] == '1') {
		LOGI("QSEECOMD_BUILTIN_SERVICES=1: built-in listener stub "
		     "for %s\n", svc->so_name);
		return &stub_ops;
	}

	dl = dlopen(svc->so_name, RTLD_NOW);
	if (!dl) {
		/*
		 * Vendor semantics: a missing service library is only
		 * logged and skipped (init_service prints the vendor
		 * "Init dlopen(...)" line).  libspl.so / libops.so never
		 * shipped on PBEM00 -- they are absent from the lineage
		 * blob AND the ColorOS 7.1 vendor image, so on the original
		 * product those two listeners never registered either.
		 * Do not substitute a stub here; that would expose TZ
		 * listeners the stock world never had (OPEN-1 closed).
		 */
		return NULL;
	}
	libs[idx].dl = dl;
	libs[idx].ops.init = (int (*)(uint32_t))dlsym(dl, svc->init_fn);
	libs[idx].ops.start = (int (*)(uint32_t))dlsym(dl, svc->start_fn);
	libs[idx].ops.close = (void (*)(void))dlsym(dl, svc->close_fn);
	if (!libs[idx].ops.start) {
		LOGE("Init dlsym(g_FSHandle, %s) fail\n", svc->start_fn);
		return NULL;
	}
	return &lib_ops;
}

/* ------------------------------------------------------------------ */
/* property_set shim                                                   */
/* ------------------------------------------------------------------ */

void qseecomd_property_set(const char *key, const char *value)
{
#if defined(__ANDROID__)
	extern int __system_property_set(const char *, const char *);
	__system_property_set(key, value);
#else
	LOGI("property_set(\"%s\", \"%s\") [host build: ignored]\n", key,
	     value);
#endif
}
