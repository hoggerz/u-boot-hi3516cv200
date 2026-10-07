/*
 * Non-destructive OpenIPC migration preflight for HI3518EV200 / 8 MiB NOR.
 *
 * MIGTEST.CFG is the explicit opt-in marker. This code deliberately has
 * no SPI erase/write calls. It validates the real migration inputs, writes
 * FAT progress files, then returns so the existing factory boot can continue.
 */

#include <common.h>
#include <fat.h>
#include <spi_flash.h>

#define MIG_TEST_MARKER		"MIGTEST.CFG"
#define MIG_MANIFEST		"MIGRATE.CFG"
#define MIG_KERNEL		"KERNEL.BIN"
#define MIG_ROOTFS		"ROOTFS.BIN"

#define MIG_LOAD_ADDR		((unsigned char *)0x82000000)
#define MIG_CFG_MAX		1024
#define MIG_STATUS_MAX		512

#define MIG_FLASH_SIZE		0x00800000UL
#define MIG_KERNEL_OFF		0x00050000UL
#define MIG_KERNEL_SIZE		0x00200000UL
#define MIG_ROOTFS_OFF		0x00250000UL
#define MIG_ROOTFS_SIZE		0x00500000UL
#define MIG_DATA_OFF		0x00750000UL
#define MIG_DATA_SIZE		0x000b0000UL

static char mig_status[MIG_STATUS_MAX];
static char mig_cfg[MIG_CFG_MAX];

static int mig_file_exists(const char *name)
{
	long sz;

	sz = file_fat_read(name, MIG_LOAD_ADDR, 1);
	return sz > 0;
}

static int mig_write_file(const char *name, const char *text)
{
	long wr;
	unsigned long len;

	len = strlen(text);
	memcpy(MIG_LOAD_ADDR, text, len);

	wr = file_fat_write(name, MIG_LOAD_ADDR, len);
	if (wr != (long)len) {
		printf("MIGTEST: FAT write failed for %s (%ld/%lu)\n",
			name, wr, len);
		return -1;
	}

	return 0;
}

static int mig_read_verify(const char *name, const char *text)
{
	long rd;
	unsigned long len;

	len = strlen(text);
	memset(MIG_LOAD_ADDR, 0, len + 1);

	rd = file_fat_read(name, MIG_LOAD_ADDR, len + 1);
	if (rd != (long)len) {
		printf("MIGTEST: FAT final re-read failed for %s (%ld/%lu)\n",
			name, rd, len);
		return -1;
	}

	if (memcmp(MIG_LOAD_ADDR, text, len) != 0) {
		printf("MIGTEST: FAT final compare failed for %s\n", name);
		return -1;
	}

	return 0;
}

static int mig_checkpoint(const char *name, const char *detail)
{
	char buf[128];

	sprintf(buf, "format=1\nmode=test\ncheckpoint=%s\n%s",
		name, detail ? detail : "");
	return mig_write_file(name, buf);
}

static void mig_status_write(const char *result, const char *last,
	const char *error, unsigned long kcrc, unsigned long rcrc,
	const char *kstate, const char *rstate, const char *fatstate)
{
	sprintf(mig_status,
		"format=1\n"
		"mode=test\n"
		"result=%s\n"
		"last_checkpoint=%s\n"
		"nor_size=0x%08lx\n"
		"manifest=%s\n"
		"kernel_size=0x%08lx\n"
		"kernel_crc32=%08lx\n"
		"kernel_crc=%s\n"
		"rootfs_size=0x%08lx\n"
		"rootfs_crc32=%08lx\n"
		"rootfs_crc=%s\n"
		"fat_write=%s\n"
		"flash_writes=0\n"
		"error=%s\n"
		"next_action=factory_boot\n",
		result, last, MIG_FLASH_SIZE,
		!strcmp(last, "T20CFG") || !strcmp(last, "T30KER") ||
			!strcmp(last, "T40ROOT") || !strcmp(last, "T50FAT") ||
			!strcmp(last, "T90PASS") ? "pass" : "unknown",
		MIG_KERNEL_SIZE, kcrc, kstate,
		MIG_ROOTFS_SIZE, rcrc, rstate, fatstate,
		error ? error : "none");

	mig_write_file("MIGSTAT.TXT", mig_status);
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
	long sz;

	sz = file_fat_read(MIG_MANIFEST, MIG_LOAD_ADDR, MIG_CFG_MAX - 1);
	if (sz <= 0 || sz >= MIG_CFG_MAX)
		return -1;

	memcpy(mig_cfg, MIG_LOAD_ADDR, sz);
	mig_cfg[sz] = '\0';
	return 0;
}

static int mig_validate_manifest(unsigned long *kernel_crc,
	unsigned long *rootfs_crc)
{
	char tmp[64];
	unsigned long value;

	if (mig_cfg_get("format", tmp, sizeof(tmp)) || strcmp(tmp, "1"))
		return -1;

	if (mig_cfg_get("target", tmp, sizeof(tmp)) ||
	    strcmp(tmp, "hi3518ev200-openipc-8m"))
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

	if (mig_cfg_get("kernel_crc32", tmp, sizeof(tmp)))
		return -1;
	*kernel_crc = simple_strtoul(tmp, NULL, 16);

	if (mig_cfg_get("rootfs_crc32", tmp, sizeof(tmp)))
		return -1;
	*rootfs_crc = simple_strtoul(tmp, NULL, 16);

#undef CHECK_HEX
	return 0;
}

static int mig_validate_payload(const char *name, unsigned long expected_size,
	unsigned long expected_crc, unsigned long *actual_crc)
{
	long sz;

	sz = file_fat_read(name, MIG_LOAD_ADDR, expected_size + 1);
	if (sz != (long)expected_size) {
		printf("MIGTEST: %s size 0x%lx, expected 0x%lx\n",
			name, sz < 0 ? 0UL : (unsigned long)sz, expected_size);
		return -1;
	}

	*actual_crc = crc32(0, MIG_LOAD_ADDR, expected_size);
	if (*actual_crc != expected_crc) {
		printf("MIGTEST: %s CRC %08lx, expected %08lx\n",
			name, *actual_crc, expected_crc);
		return -1;
	}

	return 0;
}


/*
 * Return 0 when test mode is absent.
 * Return 1 whenever MIGTEST.CFG is present, even if checks fail, so the
 * legacy autoupdate path is suppressed for that boot.
 */
int openipc_migrate_test(struct spi_flash *flash)
{
	unsigned long expected_kcrc = 0;
	unsigned long expected_rcrc = 0;
	unsigned long actual_kcrc = 0;
	unsigned long actual_rcrc = 0;
	static const char pass_text[] =
		"format=1\n"
		"mode=test\n"
		"result=pass\n"
		"flash_writes=0\n";

	if (!mig_file_exists(MIG_TEST_MARKER))
		return 0;

	printf("\n=== OpenIPC 8 MiB migration remote dry-run ===\n");
	printf("MIGTEST: SPI NOR erase/write functions are NOT used\n");
	printf("MIGTEST: read phase begins before any FAT output writes\n");

	/*
	 * IMPORTANT:
	 *
	 * This old FAT implementation has been physically proven to tolerate
	 * write -> write and write -> read, but not write -> read -> write.
	 *
	 * Therefore every migration input is read and validated before the
	 * first output/checkpoint file is written.
	 */

	if (!flash || flash->size != MIG_FLASH_SIZE) {
		printf("MIGTEST: NOR size mismatch (got 0x%08lx)\n",
			flash ? (unsigned long)flash->size : 0UL);

		mig_status_write("fail", "T00START", "nor_size_mismatch",
			0, 0, "not_tested", "not_tested", "not_tested");
		mig_write_file("T90FAIL.TXT",
			"format=1\nmode=test\nerror=nor_size_mismatch\nflash_writes=0\n");
		return 1;
	}

	if (mig_load_manifest() ||
	    mig_validate_manifest(&expected_kcrc, &expected_rcrc)) {
		printf("MIGTEST: manifest validation failed\n");

		mig_status_write("fail", "T10NOR", "manifest_invalid",
			0, 0, "not_tested", "not_tested", "not_tested");
		mig_write_file("T90FAIL.TXT",
			"format=1\nmode=test\nerror=manifest_invalid\nflash_writes=0\n");
		return 1;
	}

	if (mig_validate_payload(MIG_KERNEL, MIG_KERNEL_SIZE,
		expected_kcrc, &actual_kcrc)) {
		printf("MIGTEST: kernel validation failed\n");

		mig_status_write("fail", "T20CFG", "kernel_invalid",
			actual_kcrc, 0, "fail", "not_tested", "not_tested");
		mig_write_file("T90FAIL.TXT",
			"format=1\nmode=test\nerror=kernel_invalid\nflash_writes=0\n");
		return 1;
	}

	if (mig_validate_payload(MIG_ROOTFS, MIG_ROOTFS_SIZE,
		expected_rcrc, &actual_rcrc)) {
		printf("MIGTEST: rootfs validation failed\n");

		mig_status_write("fail", "T30KER", "rootfs_invalid",
			actual_kcrc, actual_rcrc, "pass", "fail", "not_tested");
		mig_write_file("T90FAIL.TXT",
			"format=1\nmode=test\nerror=rootfs_invalid\nflash_writes=0\n");
		return 1;
	}

	printf("MIGTEST: all source reads and CRC checks passed\n");
	printf("MIGTEST: beginning write-only checkpoint phase\n");

	/*
	 * From here until the final T90PASS re-read there must be NO FAT reads.
	 */

	if (mig_checkpoint("T20CFG.OK",
		"flash_writes=0\n"))
		return 1;

	if (mig_write_file("T00START.TXT",
		"format=1\nmode=test\ncheckpoint=T00START\nflash_writes=0\n"))
		return 1;

	if (mig_checkpoint("T10NOR.OK",
		"nor_size=0x00800000\nflash_writes=0\n"))
		return 1;

	if (mig_checkpoint("T30KER.OK",
		"kernel_size=0x00200000\nkernel_crc=pass\nflash_writes=0\n"))
		return 1;

	if (mig_checkpoint("T40ROOT.OK",
		"rootfs_size=0x00500000\nrootfs_crc=pass\nflash_writes=0\n"))
		return 1;

	if (mig_write_file("FATTEST.TXT",
		"OpenIPC migration FAT sequential-write test\n"
		"flash_writes=0\n"))
		return 1;

	if (mig_checkpoint("T50FAT.OK",
		"fat_sequential_write=pass\nflash_writes=0\n"))
		return 1;

	mig_status_write("pass", "T90PASS", "none",
		actual_kcrc, actual_rcrc, "pass", "pass", "pass");

	if (mig_write_file("T90PASS.TXT", pass_text))
		return 1;

	/*
	 * Final FAT operation of this boot.
	 *
	 * The old FAT stack is known to become unsafe for a later write after
	 * a read. No FAT writes are permitted after this verification.
	 */
	if (mig_read_verify("T90PASS.TXT", pass_text)) {
		printf("MIGTEST: final FAT readback verification FAILED\n");
		printf("MIGTEST: no further FAT writes will be attempted\n");
		return 1;
	}

	printf("MIGTEST: final FAT readback verified\n");
	printf("MIGTEST: PASS - no SPI NOR writes performed\n");
	printf("MIGTEST: returning to normal factory boot\n\n");

	return 1;
}
