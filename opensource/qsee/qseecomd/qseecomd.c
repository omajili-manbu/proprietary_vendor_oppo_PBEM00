/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qseecomd.c - rebuilt Qualcomm QSEE daemon skeleton for PBEM00.
 *
 * Faithful to the vendor binary's observable structure (REPORT.md Q3,
 * full disassembly of vendor bin/qseecomd, .text 0x2000-0x2578):
 *
 *  main():
 *    - log "qseecom listener services process entry PPID = %d"
 *    - signal(SIGPIPE, SIG_IGN)
 *    - init phase:  rpmb_init_service / ssd_init_service /
 *      secure_ui_init   (services 0, 1, 2)
 *    - prctl(PR_SET_NO_NEW_PRIVS, 1)
 *    - setgid(1000) + setuid(1000)  (error => abort)
 *    - capset({version 0x20080522, pid 0},
 *             {effective 0x20000, permitted 0x20000, inheritable 0})
 *      -> keeps CAP_SYS_RAWIO after dropping root
 *    - start phase: listener threads for all 10 services
 *    - property_set("vendor.sys.listeners.registered", "true")
 *    - log "QSEECOM DAEMON RUNNING"
 *    - pause()
 *
 * The service table constants (listener ids, dlopen names, symbol names,
 * start order) were reconstructed from the vendor .data:0x3000 table via
 * its R_AARCH64_RELATIVE relocations (work/reloc_table.py) and are
 * reproduced verbatim.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "qsee_com_api.h"
#include "qseecomd_internal.h"

/*
 * capset() has no libc wrapper declaration portable across glibc and
 * bionic; invoke the syscall directly (SYS_capset on both).
 */

#define LOG_TAG "QSEECOMD"
#define LOGI(fmt, ...) fprintf(stderr, LOG_TAG ": " fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, LOG_TAG ": E " fmt, ##__VA_ARGS__)

/*
 * Service table - vendor bin/qseecomd .data:0x3000, 10 entries x 0x38
 * bytes.  Field order and constants are per the reconstruction in
 * REPORT.md Q3; [E] marks values read directly from the relocated table.
 */
static const struct qseecomd_service service_table[] = {
	/* name                          so                   init_fn              start_fn              close_fn               id       flags  */
	{ "RPMB system services",       "librpmb.so",        "rpmb_init_service", "rpmb_start",         "rpmb_close",          0x2000,  0x2000 },  /* [E] */
	{ "SSD system services",        "libssd.so",         "ssd_init_service",  "ssd_start",          "ssd_close",           0x3000,  0x3000 },  /* [E] */
	{ "secure UI services",         "libsecureui.so",    "secure_ui_init",    "secure_ui_start",    "secure_ui_close",     0x4000,  0x4000 },  /* [E] */
	{ "time services",              "libdrmtime.so",     NULL,                "atime_start",        "atime_close",         0xB,     0xB    },  /* [E] */
	{ "file system services",       "libdrmfs.so",       NULL,                "fs_start",           "fs_close",            0xA,     0xA    },  /* [E] */
	{ "gpfile system services",     "libdrmfs.so",       NULL,                "gpfs_start",         "gpfs_close",          0x7000,  0x7000 },  /* [E] */
	{ "interrupt services",         "libqisl.so",        NULL,                "qisl_start",         NULL,                  0x9000,  0x9000 },  /* [E] */
	{ "secure processor services",  "libspl.so",         NULL,                "spl_start",          "spl_stop",            0xB000,  0xB000 },  /* [E] libspl.so never shipped: absent from
								     * the lineage blob AND the ColorOS 7.1
								     * vendor image -> vendor daemon logged
								     * the dlopen failure and skipped; the
								     * rebuild does the same (OPEN-1 closed) */
	{ "gp request cancellation services", "libGPreqcancel.so", NULL,         "gp_reqcancel_start", "gp_reqcancel_close",  0x1100,  0x1100 },  /* [E] */
	{ "output protection service listener", "libops.so", NULL,              "ops_start",          "ops_close",           0xC000,  0xC000 },  /* [E] libops.so never shipped, same as
								     * libspl.so above (OPEN-1 closed) */
};

#define SERVICE_COUNT (sizeof(service_table) / sizeof(service_table[0]))

static void *service_handles[SERVICE_COUNT];
static const struct qseecomd_service_ops *service_ops[SERVICE_COUNT];

/*
 * One service entry, vendor init_service() @0x2050 semantics:
 *   flag != 0  -> dlsym(start_fn) and call start(listener_id)
 *   flag == 0  -> dlsym(init_fn) and call init(listener_id) for services
 *                 whose table flag is 0x2000/0x3000/0x4000; for the rest
 *                 it behaves like the start phase.
 * Failures are logged and skipped, except the main-phase init of the
 * first three services where the daemon aborts.
 */
static int init_service(int idx, int flag)
{
	const struct qseecomd_service *svc = &service_table[idx];
	const struct qseecomd_service_ops *ops;
	int ret;

	if (service_ops[idx]) {
		ops = service_ops[idx];
	} else {
		ops = qseecomd_load_service(svc, idx, service_handles);
		if (!ops) {
			LOGE("Init dlopen(%s, RLTD_NOW) is failed....\n",
			     svc->so_name);
			return -1;
		}
		service_ops[idx] = ops;
	}

	if (flag != 0 || (svc->flags & 0xF000) == 0)
		ret = ops->start(idx, svc->listener_id);
	else
		ret = ops->init(idx, svc->listener_id);

	if (ret != 0) {
		/*
		 * Vendor keeps going (start_services loop ignores return
		 * values); only main's rpmb/ssd/secureui init aborts.
		 */
		LOGE("Init %s ret = %d\n", svc->name, ret);
	}
	return ret;
}

static void start_services(void)
{
	unsigned int i;

	LOGI("Total listener services to start = %d\n", (int)SERVICE_COUNT);
	for (i = 0; i < SERVICE_COUNT; i++)
		init_service((int)i, 1);
}

static int drop_privileges(void)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
		.pid = 0,
	};
	struct __user_cap_data_struct data[2] = { { 0 } };

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		LOGE("prctl(PR_SET_NO_NEW_PRIVS) failed: %s\n",
		     strerror(errno));

	if (setgid(1000) != 0) {
		LOGE("setgid error\n");
		return -1;
	}
	if (setuid(1000) != 0) {
		LOGE("setuid error\n");
		return -1;
	}

	/* Keep CAP_SYS_RAWIO (bit 17 -> 0x20000), vendor capset payload
	 * from .rodata:0xC10.  TODO-VERIFY(capmask): vendor blob sets
	 * effective=permitted=0x20000, inheritable=0 in the first 32-bit
	 * word pair; confirm on-device with /proc/<pid>/status. */
	data[0].effective = 0x20000;
	data[0].permitted = 0x20000;
	if (syscall(SYS_capset, &hdr, data) != 0)
		LOGE("qseecomd setcap failed. retvalue=%d, errno=%d\n", -1,
		     errno);
	return 0;
}

int main(int argc, char **argv)
{
	int ppid;

	(void)argc;
	(void)argv;

	ppid = (int)getppid();
	LOGI("qseecom listener services process entry PPID = %d\n", ppid);
	signal(SIGPIPE, SIG_IGN);

	/*
	 * Vendor main() runs the three init-phase services first and
	 * aborts when any of them fails ("... shall not start listener
	 * services" / "not starting services").
	 */
	if (init_service(0, 0) != 0) {
		LOGE("ERROR: RPMB_INIT failed, shall not start listener "
		     "services\n");
		return -1;
	}
	if (init_service(1, 0) != 0) {
		LOGE("ERROR: SSD_INIT failed, shall not start listener "
		     "services\n");
		return -1;
	}
	if (init_service(2, 0) != 0) {
		LOGE("SEC_UI_INIT failed, shall not start secure ui "
		     "listener\n");
		return -1;
	}

	if (drop_privileges() != 0)
		return -1;

	LOGI("qseecom listener service threads starting!!! \n");
	start_services();

	qseecomd_property_set("vendor.sys.listeners.registered", "true");
	LOGI("QSEECOM DAEMON RUNNING \n");
	pause();

	LOGI("qseecom listener services process exiting!!! \n");
	return 0;
}
