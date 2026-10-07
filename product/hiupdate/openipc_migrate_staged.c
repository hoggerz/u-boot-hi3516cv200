/*
 * Guarded two-stage factory -> OpenIPC migration for HI3518EV200 / 8 MiB NOR.
 *
 * IMPORTANT: destructive execution is intentionally disabled in this patch.
 * The complete state machine and flash helpers are present for review, but
 * MIG_REAL_WRITES_ENABLED remains 0 until the flash ranges, state transitions
 * and final OpenIPC boot handoff have been audited on the target tree.
 *
 * FAT ordering follows the physically proven MIGTEST rule:
 *   all FAT reads for a stage -> NOR work -> one durable marker write ->
 *   marker readback as the final FAT operation -> reset.
 */

#include <common.h>
#include <fat.h>
#include <malloc.h>
#include <spi_flash.h>

extern int spi_flash_erase_op(struct spi_flash *flash,
	unsigned long offset, unsigned long len);
extern int spi_flash_write_op(struct spi_flash *flash,
	unsigned long offset, unsigned long len, void *buf);

#define MIG_REAL_WRITES_ENABLED	0

#define MIG_ARM_MARKER		"MIGARM.CFG"
#define MIG_MANIFEST		"MIGRATE.CFG"
#define MIG_KERNEL		"KERNEL.BIN"
#define MIG_ROOTFS		"ROOTFS.BIN"
#define MIG_STAGE2		"STAGE2.OK"
#define MIG_DONE		"MIGDONE.OK"

#define MIG_TARGET		"hi3518ev200-openipc-8m"

#define MIG_LOAD_ADDR		((unsigned char *)0x82000000)
#define MIG_CFG_MAX		1024
#define MIG_MARKER_MAX		256
#define MIG_VERIFY_CHUNK	0x00010000UL
#define MIG_ERASE_ALIGN	0x00010000UL

#define MIG_FLASH_SIZE		0x00800000UL
#define MIG_PROTECT_END		0x00050000UL
#define MIG_KERNEL_OFF		0x00050000UL
#define MIG_KERNEL_SIZE		0x00200000UL
#define MIG_ROOTFS_OFF		0x00250000UL
#define MIG_ROOTFS_SIZE		0x00500000UL
#define MIG_DATA_OFF		0x00750000UL
#define MIG_DATA_SIZE		0x000b0000UL

static char mig_cfg[MIG_CFG_MAX];

static const char mig_arm_text[] =
	"format=1\n"
	"target=" MIG_TARGET "\n"
	"arm=YES_REWRITE_NOR\n";

static int mig_file_read_text(const char *name, char *dst, unsigned long dstsz)
{
	long sz;

	if (!dst || dstsz < 2)
		return -1;

	sz = file_fat_read(name, MIG_LOAD_ADDR, dstsz - 1);
	if (sz <= 0 || sz >= dstsz)
		return -1;

	memcpy(dst, MIG_LOAD_ADDR, sz);
	dst[sz] = '\0';
	return (int)sz;
}

static int mig_file_matches(const char *name, const char *expected)
{
	long sz;
	unsigned long len;

	len = strlen(expected);
	if (len + 1 > MIG_MARKER_MAX)
		return -1;

	sz = file_fat_read(name, MIG_LOAD_ADDR, len + 1);
	if (sz <= 0)
		return 0;
	if (sz != (long)len)
		return -1;
	if (memcmp(MIG_LOAD_ADDR, expected, len) != 0)
		return -1;

	return 1;
}

static int mig_write_and_final_verify(const char *name, const char *text)
{
	long wr;
	long rd;
	unsigned long len;

	len = strlen(text);
	memcpy(MIG_LOAD_ADDR, text, len);

	wr = file_fat_write(name, MIG_LOAD_ADDR, len);
	if (wr != (long)len) {
		printf("MIGRATE: FAT write failed for %s (%ld/%lu)\n",
			name, wr, len);
		return -1;
	}

	memset(MIG_LOAD_ADDR, 0, len + 1);
	rd = file_fat_read(name, MIG_LOAD_ADDR, len + 1);
	if (rd != (long)len) {
		printf("MIGRATE: final FAT readback failed for %s (%ld/%lu)\n",
			name, rd, len);
		return -1;
	}

	if (memcmp(MIG_LOAD_ADDR, text, len) != 0) {
		printf("MIGRATE: final FAT compare failed for %s\n", name);
		return -1;
	}

	return 0;
}

static int mig_cfg_get(const char *key, char *out, unsigned long outsz)
{
	const char *p;
	const char *line;
	unsigned long keylen;
	unsigned long n;

	keylen = strlen(key);
	line = mig_cfg;

	while (*line) {
		p = line;
		if (!strncmp(p, key, keylen) && p[keylen] == '=') {
			p += keylen + 1;
			n = 0;
			while (*p && *p != '\n' && *p != '\r' && n + 1 < outsz)
				out[n++] = *p++;
			out[n] = '\0';
			return n ? 0 : -1;
		}

		while (*line && *line != '\n')
			line++;
		if (*line == '\n')
			line++;
	}

	return -1;
}

static int mig_cfg_ulong(const char *key, unsigned long *value)
{
	char tmp[32];
	char *endp;

	if (mig_cfg_get(key, tmp, sizeof(tmp)))
		return -1;

	*value = simple_strtoul(tmp, &endp, 0);
	if (!endp || *endp != '\0')
		return -1;

	return 0;
}

static int mig_load_manifest(void)
{
	return mig_file_read_text(MIG_MANIFEST, mig_cfg, sizeof(mig_cfg)) > 0 ?
		0 : -1;
}

static int mig_cfg_crc32(const char *key, unsigned long *value)
{
	char tmp[16];
	char *endp;

	if (mig_cfg_get(key, tmp, sizeof(tmp)))
		return -1;
	if (strlen(tmp) != 8)
		return -1;

	*value = simple_strtoul(tmp, &endp, 16);
	if (!endp || *endp != '\0')
		return -1;

	return 0;
}

static int mig_validate_manifest(unsigned long *kernel_crc,
	unsigned long *rootfs_crc)
{
	char tmp[64];
	unsigned long value;

	if (mig_cfg_get("format", tmp, sizeof(tmp)) || strcmp(tmp, "1"))
		return -1;
	if (mig_cfg_get("target", tmp, sizeof(tmp)) || strcmp(tmp, MIG_TARGET))
		return -1;

#define CHECK_HEX(key, expected) \
	do { \
		if (mig_cfg_ulong((key), &value) || value != (expected)) \
			return -1; \
	} while (0)

	CHECK_HEX("flash_size", MIG_FLASH_SIZE);
	CHECK_HEX("kernel_offset", MIG_KERNEL_OFF);
	CHECK_HEX("kernel_size", MIG_KERNEL_SIZE);
	CHECK_HEX("rootfs_offset", MIG_ROOTFS_OFF);
	CHECK_HEX("rootfs_size", MIG_ROOTFS_SIZE);
	CHECK_HEX("rootfs_data_offset", MIG_DATA_OFF);
	CHECK_HEX("rootfs_data_size", MIG_DATA_SIZE);

	if (mig_cfg_crc32("kernel_crc32", kernel_crc))
		return -1;
	if (mig_cfg_crc32("rootfs_crc32", rootfs_crc))
		return -1;

#undef CHECK_HEX
	return 0;
}

static int mig_load_payload(const char *name, unsigned long expected_size,
	unsigned long expected_crc)
{
	long sz;
	unsigned long actual_crc;

	sz = file_fat_read(name, MIG_LOAD_ADDR, expected_size + 1);
	if (sz != (long)expected_size) {
		printf("MIGRATE: %s size 0x%lx, expected 0x%lx\n",
			name, sz < 0 ? 0UL : (unsigned long)sz, expected_size);
		return -1;
	}

	actual_crc = crc32(0, MIG_LOAD_ADDR, expected_size);
	if (actual_crc != expected_crc) {
		printf("MIGRATE: %s CRC %08lx, expected %08lx\n",
			name, actual_crc, expected_crc);
		return -1;
	}

	return 0;
}

static int mig_range_valid(unsigned long off, unsigned long len)
{
	if (!len)
		return 0;
	if ((off & (MIG_ERASE_ALIGN - 1)) ||
	    (len & (MIG_ERASE_ALIGN - 1)))
		return 0;
	if (off < MIG_PROTECT_END)
		return 0;
	if (off >= MIG_FLASH_SIZE)
		return 0;
	if (len > MIG_FLASH_SIZE - off)
		return 0;
	return 1;
}

static int mig_flash_crc(struct spi_flash *flash, unsigned long off,
	unsigned long len, unsigned long *out_crc)
{
	unsigned char *buf;
	unsigned long done = 0;
	unsigned long chunk;
	unsigned long crc = 0;

	if (!flash || !out_crc || !mig_range_valid(off, len))
		return -1;

	buf = malloc(MIG_VERIFY_CHUNK);
	if (!buf) {
		printf("MIGRATE: verify buffer allocation failed\n");
		return -1;
	}

	while (done < len) {
		chunk = len - done;
		if (chunk > MIG_VERIFY_CHUNK)
			chunk = MIG_VERIFY_CHUNK;

		if (spi_flash_read(flash, off + done, chunk, buf)) {
			printf("MIGRATE: flash read failed at 0x%08lx\n",
				off + done);
			free(buf);
			return -1;
		}

		crc = crc32(crc, buf, chunk);
		done += chunk;
	}

	free(buf);
	*out_crc = crc;
	return 0;
}

static int mig_program_loaded(struct spi_flash *flash, unsigned long off,
	unsigned long len, unsigned long expected_crc)
{
	unsigned long done = 0;
	unsigned long chunk;
	unsigned long actual_crc;

	if (!flash || !mig_range_valid(off, len)) {
		printf("MIGRATE: invalid program range 0x%08lx + 0x%08lx\n",
			off, len);
		return -1;
	}

	printf("MIGRATE: erase 0x%08lx .. 0x%08lx\n", off, off + len - 1);
	if (spi_flash_erase_op(flash, off, len)) {
		printf("MIGRATE: erase failed\n");
		return -1;
	}

	while (done < len) {
		chunk = len - done;
		if (chunk > MIG_VERIFY_CHUNK)
			chunk = MIG_VERIFY_CHUNK;

		if (spi_flash_write_op(flash, off + done, chunk,
			MIG_LOAD_ADDR + done)) {
			printf("MIGRATE: write failed at 0x%08lx\n", off + done);
			return -1;
		}
		done += chunk;
	}

	if (mig_flash_crc(flash, off, len, &actual_crc))
		return -1;

	printf("MIGRATE: NOR CRC32 0x%08lx expected 0x%08lx\n",
		actual_crc, expected_crc);
	if (actual_crc != expected_crc) {
		printf("MIGRATE: NOR readback CRC mismatch\n");
		return -1;
	}

	return 0;
}

static int mig_flash_verify_ff(struct spi_flash *flash, unsigned long off,
	unsigned long len)
{
	unsigned char *buf;
	unsigned long done = 0;
	unsigned long chunk;
	unsigned long i;

	if (!flash || !mig_range_valid(off, len))
		return -1;

	buf = malloc(MIG_VERIFY_CHUNK);
	if (!buf) {
		printf("MIGRATE: ff-verify buffer allocation failed\n");
		return -1;
	}

	while (done < len) {
		chunk = len - done;
		if (chunk > MIG_VERIFY_CHUNK)
			chunk = MIG_VERIFY_CHUNK;

		if (spi_flash_read(flash, off + done, chunk, buf)) {
			printf("MIGRATE: ff-verify read failed at 0x%08lx\n",
				off + done);
			free(buf);
			return -1;
		}

		for (i = 0; i < chunk; i++) {
			if (buf[i] != 0xff) {
				printf("MIGRATE: expected ff at 0x%08lx\n",
					off + done + i);
				free(buf);
				return -1;
			}
		}
		done += chunk;
	}

	free(buf);
	return 0;
}

static int mig_erase_verify_ff(struct spi_flash *flash, unsigned long off,
	unsigned long len)
{
	if (!flash || !mig_range_valid(off, len))
		return -1;

	printf("MIGRATE: erase rootfs_data 0x%08lx .. 0x%08lx\n",
		off, off + len - 1);
	if (spi_flash_erase_op(flash, off, len)) {
		printf("MIGRATE: rootfs_data erase failed\n");
		return -1;
	}

	return mig_flash_verify_ff(flash, off, len);
}

static void mig_make_stage2(char *buf, unsigned long kcrc)
{
	sprintf(buf,
		"format=1\n"
		"target=" MIG_TARGET "\n"
		"kernel_crc32=%08lx\n",
		kcrc);
}

static void mig_make_done(char *buf, unsigned long kcrc, unsigned long rcrc)
{
	sprintf(buf,
		"format=1\n"
		"target=" MIG_TARGET "\n"
		"kernel_crc32=%08lx\n"
		"rootfs_crc32=%08lx\n",
		kcrc, rcrc);
}

static int mig_preflight(struct spi_flash *flash, unsigned long *kcrc,
	unsigned long *rcrc)
{
	if (!flash || flash->size != MIG_FLASH_SIZE) {
		printf("MIGRATE: NOR size mismatch (got 0x%08lx)\n",
			flash ? (unsigned long)flash->size : 0UL);
		return -1;
	}

	if (mig_load_manifest() || mig_validate_manifest(kcrc, rcrc)) {
		printf("MIGRATE: manifest validation failed\n");
		return -1;
	}

	if (mig_load_payload(MIG_KERNEL, MIG_KERNEL_SIZE, *kcrc))
		return -1;
	if (mig_load_payload(MIG_ROOTFS, MIG_ROOTFS_SIZE, *rcrc))
		return -1;

	return 0;
}

/*
 * Return 0 when real migration is not requested.
 * Return 1 when MIGARM.CFG is present so FULLNOR/legacy update paths are
 * suppressed even while destructive execution is compile-time disabled.
 */
int openipc_migrate_handle(struct spi_flash *flash)
{
	unsigned long kcrc = 0;
	unsigned long rcrc = 0;
	unsigned long installed_kcrc = 0;
	unsigned long installed_rcrc = 0;
	char stage2_expected[MIG_MARKER_MAX];
	char done_expected[MIG_MARKER_MAX];
	int arm_state;
	int stage2_state;
	int done_state;

	arm_state = mig_file_matches(MIG_ARM_MARKER, mig_arm_text);
	if (arm_state == 0)
		return 0;

	printf("\n=== Guarded OpenIPC 8 MiB staged migration ===\n");

	if (arm_state < 0) {
		printf("MIGRATE: MIGARM.CFG exists but contents are invalid - refusing\n");
		return 1;
	}

	if (mig_preflight(flash, &kcrc, &rcrc)) {
		printf("MIGRATE: preflight failed - no NOR writes performed\n");
		return 1;
	}

	mig_make_stage2(stage2_expected, kcrc);
	mig_make_done(done_expected, kcrc, rcrc);

	/*
	 * All stage-marker reads occur before any write in this boot.
	 * Invalid marker contents are fatal; existence alone is never enough.
	 */
	done_state = mig_file_matches(MIG_DONE, done_expected);
	stage2_state = mig_file_matches(MIG_STAGE2, stage2_expected);

	if (done_state < 0 || stage2_state < 0) {
		printf("MIGRATE: invalid stage marker contents - refusing\n");
		return 1;
	}
	if (done_state == 1 && stage2_state != 1) {
		printf("MIGRATE: MIGDONE.OK without valid STAGE2.OK - refusing\n");
		return 1;
	}

	if (!MIG_REAL_WRITES_ENABLED) {
		printf("MIGRATE: destructive engine is COMPILE-TIME DISABLED\n");
		printf("MIGRATE: audited stage would be: %s\n",
			done_state == 1 ? "DONE/BOOT HANDOFF" :
			stage2_state == 1 ? "STAGE 2" : "STAGE 1");
		printf("MIGRATE: no SPI NOR writes performed\n");
		return 1;
	}

	if (done_state == 1) {
		/*
		 * Never trust the SD completion marker alone. Before a future
		 * OpenIPC handoff, prove that the installed kernel/rootfs still
		 * match the manifest and rootfs_data remains erased.
		 */
		if (mig_flash_crc(flash, MIG_KERNEL_OFF, MIG_KERNEL_SIZE,
			&installed_kcrc) || installed_kcrc != kcrc) {
			printf("MIGRATE: DONE kernel verification failed - refusing handoff\n");
			return 1;
		}
		if (mig_flash_crc(flash, MIG_ROOTFS_OFF, MIG_ROOTFS_SIZE,
			&installed_rcrc) || installed_rcrc != rcrc) {
			printf("MIGRATE: DONE rootfs verification failed - refusing handoff\n");
			return 1;
		}
		if (mig_flash_verify_ff(flash, MIG_DATA_OFF, MIG_DATA_SIZE)) {
			printf("MIGRATE: DONE rootfs_data verification failed - refusing handoff\n");
			return 1;
		}

		/*
		 * Deliberately refuse here until the OpenIPC boot handoff is
		 * explicitly defined and audited. Enabling destructive writes
		 * without a deterministic post-migration boot path is forbidden.
		 */
		printf("MIGRATE: installed OpenIPC layout verifies, but boot handoff is disabled\n");
		return 1;
	}

	if (stage2_state == 0) {
		/*
		 * Stage 1: both payloads have already been validated. Re-read
		 * KERNEL.BIN now so the loaded buffer is exactly the bytes about
		 * to be programmed. This remains before the first FAT write.
		 */
		if (mig_load_payload(MIG_KERNEL, MIG_KERNEL_SIZE, kcrc)) {
			printf("MIGRATE: final kernel reload failed\n");
			return 1;
		}

		if (mig_program_loaded(flash, MIG_KERNEL_OFF, MIG_KERNEL_SIZE, kcrc)) {
			printf("MIGRATE: Stage 1 flash operation failed; resetting to recovery\n");
			reset_cpu(0);
			return 1;
		}

		/*
		 * STAGE2.OK is the only FAT write of Stage 1. Its readback is
		 * the final FAT operation, then reset immediately.
		 */
		if (mig_write_and_final_verify(MIG_STAGE2, stage2_expected)) {
			printf("MIGRATE: Stage 1 marker persistence failed; resetting to recovery\n");
			reset_cpu(0);
			return 1;
		}

		printf("MIGRATE: Stage 1 complete; resetting into recovery U-Boot\n");
		reset_cpu(0);
		return 1;
	}

	/*
	 * Stage 2: marker reads reused MIG_LOAD_ADDR, so ROOTFS.BIN must be
	 * loaded again before programming. This is still a FAT read and occurs
	 * before the first FAT write of this boot.
	 */
	if (mig_load_payload(MIG_ROOTFS, MIG_ROOTFS_SIZE, rcrc)) {
		printf("MIGRATE: final rootfs reload failed; resetting to recovery\n");
		reset_cpu(0);
		return 1;
	}

	/* Verify the installed Stage-1 kernel before changing rootfs. */
	if (mig_flash_crc(flash, MIG_KERNEL_OFF, MIG_KERNEL_SIZE,
		&installed_kcrc)) {
		printf("MIGRATE: installed kernel verification read failed; resetting\n");
		reset_cpu(0);
		return 1;
	}
	if (installed_kcrc != kcrc) {
		printf("MIGRATE: installed kernel CRC %08lx expected %08lx - resetting\n",
			installed_kcrc, kcrc);
		reset_cpu(0);
		return 1;
	}

	if (mig_program_loaded(flash, MIG_ROOTFS_OFF, MIG_ROOTFS_SIZE, rcrc)) {
		printf("MIGRATE: Stage 2 rootfs flash failed; resetting to recovery\n");
		reset_cpu(0);
		return 1;
	}

	if (mig_erase_verify_ff(flash, MIG_DATA_OFF, MIG_DATA_SIZE)) {
		printf("MIGRATE: Stage 2 rootfs_data erase/verify failed; resetting\n");
		reset_cpu(0);
		return 1;
	}

	/*
	 * MIGDONE.OK is the only FAT write of Stage 2. Its readback is the
	 * final FAT operation, then reset immediately.
	 */
	if (mig_write_and_final_verify(MIG_DONE, done_expected)) {
		printf("MIGRATE: Stage 2 marker persistence failed; resetting to recovery\n");
		reset_cpu(0);
		return 1;
	}

	printf("MIGRATE: Stage 2 complete; resetting for OpenIPC boot handoff\n");
	reset_cpu(0);
	return 1;
}
