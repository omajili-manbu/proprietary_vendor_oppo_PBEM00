/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * qsee_image.c - TA image (.mdt + .bXX) reader.
 *
 * Reimplements the vendor __QSEECom_load_image() file-access half
 * (vendor .so @0x35ec): read the 64-byte .mdt header, take e_phnum
 * (ELF32 @0x2C / ELF64 @0x38), open "%s/%s.b%02d" for every segment
 * plus the .mdt itself, and stream them sequentially into the caller's
 * contiguous buffer.  Total segment count is capped at 100 exactly like
 * the vendor code ("Number of files reported is %d, max allowed it %d").
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qsee_internal.h"

#define QSEE_MAX_SEG_FILES 100

static int build_path(char *buf, size_t buflen, const char *base,
		      const char *name, const char *ext)
{
	int r = snprintf(buf, buflen, "%s/%s%s", base, name, ext ? ext : "");
	return (r < 0 || (size_t)r >= buflen) ? -ENAMETOOLONG : 0;
}

/*
 * Parse the MDT header: returns e_phnum (number of .bXX segments) and
 * the ELF class, or -EINVAL when the class is neither 1 nor 2 (vendor
 * log: "Error:: Invalid tz app architecture %d").
 */
static int parse_mdt_header(const uint8_t *hdr, uint32_t *phnum,
			    uint32_t *app_arch)
{
	uint8_t elf_class = hdr[4];    /* e_ident[EI_CLASS] */

	if (elf_class == 2) {          /* ELFCLASS64: e_phnum at 0x38 */
		uint16_t v = (uint16_t)(hdr[0x38] | (hdr[0x39] << 8));
		*phnum = v;
	} else if (elf_class == 1) {   /* ELFCLASS32: e_phnum at 0x2C */
		uint16_t v = (uint16_t)(hdr[0x2C] | (hdr[0x2D] << 8));
		*phnum = v;
	} else {
		return -EINVAL;
	}

	if (*phnum == 0xFFFF)
		return -E2BIG;
	*app_arch = elf_class;
	return 0;
}

int qsee_probe_image_size(const char *base_dir, const char *app_name,
			  uint32_t *total_len, uint32_t *app_arch)
{
	char path[256];
	uint8_t mdt[64];
	uint32_t phnum = 0;
	uint64_t total = 0;
	int rc, fd;
	uint32_t i;
	struct stat st;

	rc = build_path(path, sizeof(path), base_dir, app_name, ".mdt");
	if (rc)
		return rc;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (read(fd, mdt, sizeof(mdt)) != (ssize_t)sizeof(mdt)) {
		close(fd);
		return -EIO;
	}
	rc = parse_mdt_header(mdt, &phnum, app_arch);
	close(fd);
	if (rc)
		return rc;
	if (phnum + 1 > QSEE_MAX_SEG_FILES)
		return -E2BIG;

	/* The .mdt itself plus phnum .bXX segments. */
	total = sizeof(mdt);
	for (i = 0; i < phnum; i++) {
		rc = snprintf(path, sizeof(path), "%s/%s.b%02u",
			      base_dir, app_name, i);
		if (rc < 0 || (size_t)rc >= sizeof(path))
			return -ENAMETOOLONG;
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return -errno;
		if (fstat(fd, &st) < 0) {
			close(fd);
			return -errno;
		}
		total += (uint64_t)st.st_size;
		close(fd);
	}

	if (total > 0xFFFFFFFFull)
		return -EOVERFLOW;
	*total_len = (uint32_t)total;
	return 0;
}

int qsee_read_image_files(const char *base_dir, const char *app_name,
			  void *dst, size_t dst_len, uint32_t *mdt_len,
			  uint32_t *total_len, uint32_t *app_arch)
{
	char path[256];
	uint8_t mdt[64];
	uint32_t phnum = 0;
	uint32_t arch = 0;
	uint64_t total = 0;
	size_t offset = 0;
	int rc, fd;
	uint32_t i;

	if (!dst || dst_len < sizeof(mdt))
		return -EINVAL;

	rc = build_path(path, sizeof(path), base_dir, app_name, ".mdt");
	if (rc)
		return rc;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (read(fd, mdt, sizeof(mdt)) != (ssize_t)sizeof(mdt)) {
		close(fd);
		return -EIO;
	}
	rc = parse_mdt_header(mdt, &phnum, &arch);
	if (rc) {
		close(fd);
		return rc;
	}
	if (phnum + 1 > QSEE_MAX_SEG_FILES) {
		close(fd);
		return -E2BIG;
	}

	memcpy(dst, mdt, sizeof(mdt));
	offset = sizeof(mdt);
	total = sizeof(mdt);
	close(fd);

	for (i = 0; i < phnum; i++) {
		ssize_t got;
		size_t seg_off = offset;

		rc = snprintf(path, sizeof(path), "%s/%s.b%02u",
			      base_dir, app_name, i);
		if (rc < 0 || (size_t)rc >= sizeof(path))
			return -ENAMETOOLONG;
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return -errno;
		while ((got = read(fd, (char *)dst + offset,
				   dst_len - offset)) > 0)
			offset += (size_t)got;
		close(fd);
		if (got < 0)
			return -errno;
		if (offset == seg_off)
			return -EIO;    /* empty segment */
		total += offset - seg_off;
	}

	*mdt_len = (uint32_t)sizeof(mdt);
	*total_len = (uint32_t)total;
	*app_arch = arch;
	return 0;
}
