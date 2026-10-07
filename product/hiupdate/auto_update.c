/*
 * (C) Copyright 2003
 * Gary Jennejohn, DENX Software Engineering, gj@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <environment.h>
#include <command.h>
#include <malloc.h>
#include <image.h>
#include <asm/byteorder.h>
#include <asm/io.h>
#include <spi_flash.h>
#include <linux/mtd/mtd.h>
#include <fat.h>

#ifdef CONFIG_AUTO_UPDATE  /* cover the whole file */

#ifdef CONFIG_AUTO_SD_UPDATE
#ifndef CONFIG_MMC
#error "should have defined CONFIG_MMC"
#endif
#include <mmc.h>
#include "mmc_init.c"
#endif

#if defined CONFIG_AUTO_USB_UPDATE
#if !defined CONFIG_USB_OHCI && !defined CONFIG_USB_XHCI
#error "should have defined CONFIG_USB_OHCI or CONFIG_USB_XHCI"
#endif
#ifndef CONFIG_USB_STORAGE
#error "should have defined CONFIG_USB_STORAGE"
#endif
#include <usb.h>
#include "usb_init.c"
#endif

extern int openipc_migrate_test(struct spi_flash *flash);
extern int openipc_migrate_handle(struct spi_flash *flash);

#undef AU_DEBUG
#undef debug
#ifdef	AU_DEBUG
#define debug(fmt, args...)	printf(fmt, ##args)
#else
#define debug(fmt, args...)
#endif	/* AU_DEBUG */

/* possible names of files on the medium. */
#define AU_FIRMWARE	"autoupdate-uboot.img"
#define AU_KERNEL	"autoupdate-kernel.img"
#define AU_ROOTFS	"autoupdate-rootfs.img"

struct flash_layout {
	long start;
	long end;
};
static struct spi_flash *flash;

struct medium_interface {
	char name[20];
	int (*init) (void);
	void (*exit) (void);
};

#define MAX_UPDATE_INTF	3
static struct medium_interface s_intf[MAX_UPDATE_INTF] = {
#ifdef CONFIG_AUTO_SD_UPDATE
	{.name = "mmc",	.init = mmc_stor_init,	.exit = mmc_stor_exit,},
#endif
#ifdef CONFIG_AUTO_USB_UPDATE
	{.name = "usb",	.init = usb_stor_init,	.exit = usb_stor_exit,},
#endif
};

/* OpenIPC flash layout
0x000000000000-0x000000040000 : "boot"
0x000000040000-0x000000050000 : "env"
0x000000050000-0x000000250000 : "kernel"
0x000000250000-0x000000750000 : "rootfs"
0x000000750000-0x000001000000 : "rootfs_data" */

/* layout of the FLASH. ST = start address, ND = end address. */
#define AU_FL_FIRMWARE_ST	0x00000000
#define AU_FL_FIRMWARE_ND	0x0004FFFF
#define AU_FL_KERNEL_ST		0x00050000
#define AU_FL_KERNEL_ND		0x0024FFFF
#define AU_FL_ROOTFS_ST		0x00250000
#define AU_FL_ROOTFS_ND		0x0074FFFF

static int au_stor_curr_dev; /* current device */

/* index of each file in the following arrays */
#define IDX_FIRMWARE	0
#define IDX_KERNEL	1
#define IDX_ROOTFS	2

/* max. number of files which could interest us */
#define AU_MAXFILES 3

/* pointers to file names */
char *aufile[AU_MAXFILES] = {
	AU_FIRMWARE,
	AU_KERNEL,
	AU_ROOTFS
};

/* sizes of flash areas for each file */
long ausize[AU_MAXFILES] = {
	(AU_FL_FIRMWARE_ND + 1) - AU_FL_FIRMWARE_ST,
	(AU_FL_KERNEL_ND + 1) - AU_FL_KERNEL_ST,
	(AU_FL_ROOTFS_ND + 1) - AU_FL_ROOTFS_ST,
};

/* array of flash areas start and end addresses */
struct flash_layout aufl_layout[AU_MAXFILES] = {
	{ AU_FL_FIRMWARE_ST,	AU_FL_FIRMWARE_ND, },
	{ AU_FL_KERNEL_ST,	AU_FL_KERNEL_ND,   },
	{ AU_FL_ROOTFS_ST,	AU_FL_ROOTFS_ND,   },
};

/* where to load files into memory */
#if defined(CONFIG_HI3536) || defined(CONFIG_HI3531A) || defined(CONFIG_HI3531D)
#define LOAD_ADDR ((unsigned char *)0x42000000)
#else
#define LOAD_ADDR ((unsigned char *)0x82000000)
#endif
/* the app is the largest image */
#define MAX_LOADSZ ausize[IDX_ROOTFS]

static int au_check_cksum_valid(int idx, long nbytes)
{
	image_header_t *hdr;
	unsigned long checksum;

	hdr = (image_header_t *)LOAD_ADDR;

	if (nbytes != (sizeof(*hdr) + ntohl(hdr->ih_size))) {
		printf("Image %s bad total SIZE\n", aufile[idx]);
		return -1;
	}
	/* check the data CRC */
	checksum = ntohl(hdr->ih_dcrc);

	if (crc32(0, (unsigned char const *)(LOAD_ADDR + sizeof(*hdr)),
			ntohl(hdr->ih_size)) != checksum) {
		printf("Image %s bad data checksum\n", aufile[idx]);
		return -1;
	}

	return 0;
}

static int au_check_header_valid(int idx, long nbytes)
{
	image_header_t *hdr;
	unsigned long checksum;

	char env[20] = {0};
	char auversion[20] = {0};

	hdr = (image_header_t *)LOAD_ADDR;
	/* check the easy ones first */
#if 0
	#define CHECK_VALID_DEBUG
#else
	#undef CHECK_VALID_DEBUG
#endif

#ifdef CHECK_VALID_DEBUG
	printf("\nmagic %#x %#x\n", ntohl(hdr->ih_magic), IH_MAGIC);
	printf("arch %#x %#x\n", hdr->ih_arch, IH_ARCH_ARM);
	printf("size %#x %#lx\n", ntohl(hdr->ih_size), nbytes);
	printf("type %#x %#x\n", hdr->ih_type, IH_TYPE_KERNEL);
#endif
	if (nbytes < sizeof(*hdr)) {
		printf("Image %s bad header SIZE\n", aufile[idx]);
		return -1;
	}
	if (ntohl(hdr->ih_magic) != IH_MAGIC || hdr->ih_arch != IH_ARCH_ARM) {
		printf("Image %s bad MAGIC or ARCH\n", aufile[idx]);
		return -1;
	}
	/* check the hdr CRC */
	checksum = ntohl(hdr->ih_hcrc);
	hdr->ih_hcrc = 0;

	if (crc32(0, (unsigned char const *)hdr, sizeof(*hdr)) != checksum) {
		printf("Image %s bad header checksum\n", aufile[idx]);
		return -1;
	}
	hdr->ih_hcrc = htonl(checksum);
	/* check the type - could do this all in one gigantic if() */
	if ((idx == IDX_FIRMWARE) && (hdr->ih_type != IH_TYPE_FIRMWARE)) {
		printf("Image %s wrong type\n", aufile[idx]);
		return -1;
	}
	if ((idx == IDX_KERNEL) && (hdr->ih_type != IH_TYPE_KERNEL)) {
		printf("Image %s wrong type\n", aufile[idx]);
		return -1;
	}
	if ((idx == IDX_ROOTFS) &&
			(hdr->ih_type != IH_TYPE_RAMDISK) &&
			(hdr->ih_type != IH_TYPE_FILESYSTEM)) {
		printf("Image %s wrong type\n", aufile[idx]);
		ausize[idx] = 0;
		return -1;
	}

	/* recycle checksum */
	checksum = ntohl(hdr->ih_size);
	/* for kernel and app the image header must also fit into flash */
	if ((idx == IDX_KERNEL) || (hdr->ih_type == IH_TYPE_RAMDISK))
		checksum += sizeof(*hdr);

	/* check the size does not exceed space in flash. HUSH scripts */
	/* all have ausize[] set to 0 */
	if ((ausize[idx] != 0) && (ausize[idx] < checksum)) {
		printf("Image %s is bigger than FLASH\n", aufile[idx]);
		return -1;
	}

	sprintf(env, "%lx", (unsigned long)ntohl(hdr->ih_time));
	setenv(auversion, env);

	return 0;
}

static void schedule_notify(unsigned long offset, unsigned long len,
		unsigned long off_start)
{
	int percent_complete = -1;

	do {
		unsigned long long n = (unsigned long long)
			(offset - off_start) * 100;
		int percent;

		do_div(n, len);
		percent = (int)n;

		/* output progress message only at whole percent
		 * steps to reduce the number of messages
		 * printed on (slow) serial consoles
		 */
		if (percent != percent_complete) {
			percent_complete = percent;

			printf("\rOperation at 0x%lx -- %3d%% complete.",
					offset, percent);
		}
	} while (0);
}

static int spi_flash_erase_op(struct spi_flash *flash, unsigned long offset,
		unsigned long len)
{
	int ret;
	struct mtd_info_ex *spiflash_info = get_spiflash_info();
	unsigned long erase_start, erase_len, erase_step;

	erase_start = offset;
	erase_len   = len;
	erase_step  = spiflash_info->erasesize;

	while (len > 0) {
		if (len < erase_step)
			erase_step = len;

		ret = flash->erase(flash, (u32)offset, erase_step);
		if (ret)
			return 1;

		len -= erase_step;
		offset += erase_step;
		/* notify real time schedule */
		schedule_notify(offset, erase_len, erase_start);
	}
	printf("\n");
	return ret;
}

static int spi_flash_write_op(struct spi_flash *flash, unsigned long offset,
		unsigned long len, void *buf)
{
	int ret = 0;
	unsigned long write_start, write_len, write_step;
	char *pbuf = buf;
	struct mtd_info_ex *spiflash_info = get_spiflash_info();

	write_start = offset;
	write_len   = len;
	write_step  = spiflash_info->erasesize;

	while (len > 0) {
		if (len < write_step)
			write_step = len;

		ret = flash->write(flash, offset, write_step, pbuf);
		if (ret)
			break;

		offset += write_step;
		pbuf   += write_step;
		len    -= write_step;
		/* notify real time schedule */
		schedule_notify(offset, write_len, write_start);
	}
	printf("\n");
	return ret;
}

static int au_do_update(int idx, long sz)
{
	image_header_t *hdr;
	unsigned long start, len;
	unsigned long write_len;
	int rc;
	void *buf;
	char *pbuf;

	hdr = (image_header_t *)LOAD_ADDR;

	start = aufl_layout[idx].start;
	len = aufl_layout[idx].end - aufl_layout[idx].start + 1;

	/*
	 * erase the address range.
	 */
	printf("flash erase...\n");
	rc = spi_flash_erase_op(flash, start, len);
	if (rc) {
		printf("SPI flash sector erase failed\n");
		return 1;
	}

	buf = map_physmem((unsigned long)LOAD_ADDR, len, MAP_WRBACK);
	if (!buf) {
		puts("Failed to map physical memory\n");
		return 1;
	}

	/* strip the header - except for the kernel and ramdisk */
	if (hdr->ih_type == IH_TYPE_RAMDISK) {
		pbuf = buf;
		write_len = sizeof(*hdr) + ntohl(hdr->ih_size);
	} else {
		pbuf = (buf + sizeof(*hdr));
		write_len = ntohl(hdr->ih_size);
	}

	/* copy the data from RAM to FLASH */
	printf("\nflash write...\n");
	rc = spi_flash_write_op(flash, start, write_len, pbuf);
	if (rc) {
		printf("SPI flash write failed, return %d\n", rc);
		return 1;
	}

	/* check the dcrc of the copy */
	if (crc32(0, (unsigned char const *)(buf + sizeof(*hdr)),
		ntohl(hdr->ih_size)) != ntohl(hdr->ih_dcrc)) {
		printf("Image %s Bad Data Checksum After COPY\n", aufile[idx]);
		return -1;
	}

	unmap_physmem(buf, len);

	return 0;
}

static void get_update_env(char *img_start, char *img_end)
{
	long start = -1, end = 0;
	char *env;

	/*
	 * check whether start and end are defined in environment
	 * variables.
	 */
	env = getenv(img_start);
	if (env != NULL)
		start = simple_strtoul(env, NULL, 16);

	env = getenv(img_end);
	if (env != NULL)
		end = simple_strtoul(env, NULL, 16);

	if (start >= 0 && end && end > start) {
		ausize[IDX_FIRMWARE] = (end + 1) - start;
		aufl_layout[0].start = start;
		aufl_layout[0].end = end;
	}
}

/*
 * If none of the update file(u-boot, kernel or rootfs) was found
 * in the medium, return -1;
 * If u-boot has been updated, return 1;
 * Others, return 0;
 */

/*
 * OpenIPC guarded full-NOR installer.
 *
 * Files on FAT partition:
 *
 *   openipc-full-nor.img
 *   openipc-full-nor.crc32
 *   openipc-full-nor.check   dry-run / validation only
 *   openipc-full-nor.flash   perform actual update
 *
 * Supported in this first implementation:
 *   8 MiB SPI NOR
 *   16 MiB SPI NOR
 *
 * The existing boot region (first 256 KiB) is deliberately written LAST.
 */
#define FULLNOR_IMAGE       "openipc-full-nor.img"
#define FULLNOR_CRC         "openipc-full-nor.crc32"
#define FULLNOR_CHECK       "openipc-full-nor.check"
#define FULLNOR_FLASH       "openipc-full-nor.flash"

#define FULLNOR_BOOT_SIZE   0x00040000UL
#define FULLNOR_8M_SIZE     0x00800000UL
#define FULLNOR_16M_SIZE    0x01000000UL

#define FULLNOR_CRC_ADDR    ((unsigned char *)0x83100000)
#define FULLNOR_VERIFY_CHUNK 0x00010000UL

#define FULLNOR_NOT_REQUESTED  0
#define FULLNOR_HANDLED        1
#define FULLNOR_FAILED        -1

static int guarded_nor_file_exists(const char *name)
{
    long n;

    n = file_fat_read(name, LOAD_ADDR, 1);
    return n > 0;
}

static int guarded_nor_parse_crc(const char *name, u32 *wanted)
{
    long n;
    char *p;
    char *endp;
    unsigned long v;

    memset(FULLNOR_CRC_ADDR, 0, 64);

    n = file_fat_read(name, FULLNOR_CRC_ADDR, 63);
    if (n <= 0) {
            printf("FULLNOR: missing %s\n", name);
            return -1;
    }

    FULLNOR_CRC_ADDR[n] = '\0';

    p = (char *)FULLNOR_CRC_ADDR;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;

    v = simple_strtoul(p, &endp, 16);
    if (endp == p) {
            printf("FULLNOR: invalid CRC file\n");
            return -1;
    }

    *wanted = (u32)v;
    return 0;
}

static int guarded_nor_crc_flash(struct spi_flash *f, u32 size, u32 *out)
{
    unsigned char *buf;
    u32 off;
    u32 crc = 0;
    u32 len;
    int ret;

    buf = malloc(FULLNOR_VERIFY_CHUNK);
    if (!buf) {
            printf("FULLNOR: verify buffer allocation failed\n");
            return -1;
    }

    for (off = 0; off < size; off += len) {
            len = size - off;
            if (len > FULLNOR_VERIFY_CHUNK)
                    len = FULLNOR_VERIFY_CHUNK;

            ret = f->read(f, off, len, buf);
            if (ret) {
                    printf("FULLNOR: flash read failed at 0x%08x\n", off);
                    free(buf);
                    return -1;
            }

            crc = crc32(crc, buf, len);
    }

    free(buf);
    *out = crc;
    return 0;
}

static int guarded_nor_verify_range(struct spi_flash *f, u32 flash_off,
            const unsigned char *image, u32 len)
{
    unsigned char *buf;
    u32 done = 0;
    u32 step;
    int ret;

    buf = malloc(FULLNOR_VERIFY_CHUNK);
    if (!buf) {
            printf("FULLNOR: verify buffer allocation failed\n");
            return -1;
    }

    while (done < len) {
            step = len - done;
            if (step > FULLNOR_VERIFY_CHUNK)
                    step = FULLNOR_VERIFY_CHUNK;

            ret = f->read(f, flash_off + done, step, buf);
            if (ret) {
                    printf("FULLNOR: read-back failed at 0x%08x\n",
                            flash_off + done);
                    free(buf);
                    return -1;
            }

            if (memcmp(buf, image + done, step) != 0) {
                    printf("FULLNOR: verify mismatch at 0x%08x\n",
                            flash_off + done);
                    free(buf);
                    return -1;
            }

            done += step;
    }

    free(buf);
    return 0;
}

static int guarded_nor_program_range(struct spi_flash *f, u32 flash_off,
            const unsigned char *image, u32 len)
{
    int ret;

    printf("FULLNOR: erase 0x%08x .. 0x%08x\n",
            flash_off, flash_off + len - 1);

    ret = spi_flash_erase_op(f, flash_off, len);
    if (ret) {
            printf("FULLNOR: erase failed\n");
            return -1;
    }

    printf("FULLNOR: write 0x%08x .. 0x%08x\n",
            flash_off, flash_off + len - 1);

    ret = spi_flash_write_op(f, flash_off, len, (void *)image);
    if (ret) {
            printf("FULLNOR: write failed\n");
            return -1;
    }

    printf("FULLNOR: verify 0x%08x .. 0x%08x\n",
            flash_off, flash_off + len - 1);

    return guarded_nor_verify_range(f, flash_off, image, len);
}

/*
 * Generic guarded whole-NOR operation.
 *
 * The caller supplies the SD/FAT filenames and display title.  The safety
 * policy deliberately remains identical to the original OpenIPC FULLNOR
 * implementation:
 *
 *   - supported NOR capacity only;
 *   - expected CRC required;
 *   - exact image/NOR size match;
 *   - image CRC checked before erase;
 *   - CHECK ONLY performs no writes;
 *   - everything after the boot region is programmed and verified first;
 *   - boot region is programmed last;
 *   - final whole-NOR CRC must match.
 *
 * This function intentionally does not save the environment or reset.
 */
static int guarded_nor_handle(struct spi_flash *f,
            const char *image_name,
            const char *crc_name,
            const char *check_name,
            const char *flash_name,
            const char *title)
{
    int check_requested;
    int flash_requested;
    int do_flash;
    long image_len;
    u32 expected_crc;
    u32 image_crc;
    u32 flash_crc;
    u32 flash_size;
    unsigned char *image = LOAD_ADDR;

    check_requested = guarded_nor_file_exists(check_name);
    flash_requested = guarded_nor_file_exists(flash_name);

    if (!check_requested && !flash_requested)
            return FULLNOR_NOT_REQUESTED;

    if (check_requested && flash_requested) {
            printf("FULLNOR: both .check and .flash markers exist - refusing\n");
            return FULLNOR_FAILED;
    }

    do_flash = flash_requested;
    flash_size = f->size;

    printf("\n");
    printf("========================================\n");
    printf(" %s\n", title);
    printf(" Mode: %s\n", do_flash ? "FLASH" : "CHECK ONLY");
    printf(" SPI NOR size: 0x%08x (%u MiB)\n",
            flash_size, flash_size >> 20);
    printf("========================================\n");

    if (flash_size != FULLNOR_8M_SIZE &&
        flash_size != FULLNOR_16M_SIZE) {
            printf("FULLNOR: unsupported flash size - refusing\n");
            return FULLNOR_FAILED;
    }

    if (guarded_nor_parse_crc(crc_name, &expected_crc) != 0)
            return FULLNOR_FAILED;

    printf("FULLNOR: expected CRC32 0x%08x\n", expected_crc);

    if (guarded_nor_crc_flash(f, flash_size, &flash_crc) == 0) {
            printf("FULLNOR: current NOR CRC32 0x%08x\n", flash_crc);

            if (flash_crc == expected_crc) {
                    printf("FULLNOR: requested image is already installed - skipping\n");
                    return FULLNOR_HANDLED;
            }
    }

    printf("FULLNOR: loading %s\n", image_name);

    /*
     * Read at most flash_size + 1 so an oversized image cannot silently
     * appear valid. 8/16 MiB are safe at LOAD_ADDR on this 64 MiB platform.
     */
    image_len = file_fat_read(image_name, image, flash_size + 1);

    if (image_len <= 0) {
            printf("FULLNOR: image not found/readable\n");
            return FULLNOR_FAILED;
    }

    printf("FULLNOR: image size 0x%08lx\n", image_len);

    if ((u32)image_len != flash_size) {
            printf("FULLNOR: image size does not exactly match NOR - refusing\n");
            return FULLNOR_FAILED;
    }

    image_crc = crc32(0, image, flash_size);
    printf("FULLNOR: image CRC32 0x%08x\n", image_crc);

    if (image_crc != expected_crc) {
            printf("FULLNOR: image CRC mismatch - refusing\n");
            return FULLNOR_FAILED;
    }

    printf("FULLNOR: image size and CRC valid\n");

    if (!do_flash) {
            printf("FULLNOR: CHECK ONLY complete - no flash was modified\n");
            return FULLNOR_HANDLED;
    }

    printf("\nFULLNOR: *** FLASH MODE ***\n");
    printf("FULLNOR: boot region will be programmed LAST\n");

    /*
     * Preserve the currently installed/recoverable U-Boot until all bytes
     * after the boot region have been successfully written and verified.
     */
    if (guarded_nor_program_range(f,
                    FULLNOR_BOOT_SIZE,
                    image + FULLNOR_BOOT_SIZE,
                    flash_size - FULLNOR_BOOT_SIZE) != 0) {
            printf("FULLNOR: non-boot region failed; original boot remains intact\n");
            return FULLNOR_FAILED;
    }

    printf("FULLNOR: non-boot region verified\n");
    printf("FULLNOR: programming boot region LAST\n");

    if (guarded_nor_program_range(f, 0, image, FULLNOR_BOOT_SIZE) != 0) {
            printf("FULLNOR: BOOT REGION UPDATE FAILED\n");
            return FULLNOR_FAILED;
    }

    printf("FULLNOR: boot region verified\n");

    if (guarded_nor_crc_flash(f, flash_size, &flash_crc) != 0)
            return FULLNOR_FAILED;

    printf("FULLNOR: final NOR CRC32 0x%08x\n", flash_crc);

    if (flash_crc != expected_crc) {
            printf("FULLNOR: FINAL CRC MISMATCH\n");
            return FULLNOR_FAILED;
    }

    printf("FULLNOR: SUCCESS - complete NOR matches requested image\n");
    printf("FULLNOR: reset required\n");

    return FULLNOR_HANDLED;
}

/*
 * OpenIPC migration wrapper.
 *
 * Keep the historical filenames and externally visible behaviour unchanged.
 * Factory restoration can later become a second caller of guarded_nor_handle()
 * without duplicating the erase/write/verify implementation.
 */
static int fullnor_handle(struct spi_flash *f)
{
    return guarded_nor_handle(f,
                    FULLNOR_IMAGE,
                    FULLNOR_CRC,
                    FULLNOR_CHECK,
                    FULLNOR_FLASH,
                    "OpenIPC guarded full-NOR installer");
}


static int update_to_flash(void)
{
	int i = 0;
	long sz;
	int res, cnt;
	int uboot_updated = 0;
	int image_found = 0;

	/* just loop thru all the possible files */
	for (i = 0; i < AU_MAXFILES; i++) {
		/* just read the header */
		sz = file_fat_read(aufile[i], LOAD_ADDR,
			sizeof(image_header_t));
		debug("read %s sz %ld hdr %d\n",
			aufile[i], sz, sizeof(image_header_t));
		if (sz <= 0 || sz < sizeof(image_header_t)) {
			debug("%s not found\n", aufile[i]);
			continue;
		}

		image_found = 1;

		if (au_check_header_valid(i, sz) < 0) {
			debug("%s header not valid\n", aufile[i]);
			continue;
		}

		sz = file_fat_read(aufile[i], LOAD_ADDR, MAX_LOADSZ);
		debug("read %s sz %ld hdr %d\n",
			aufile[i], sz, sizeof(image_header_t));
		if (sz <= 0 || sz <= sizeof(image_header_t)) {
			debug("%s not found\n", aufile[i]);
			continue;
		}

		if (au_check_cksum_valid(i, sz) < 0) {
			debug("%s checksum not valid\n", aufile[i]);
			continue;
		}

		/* If u-boot had been updated, we need to
		 * save current env to flash */
		if (0 == strcmp((char *)AU_FIRMWARE, aufile[i]))
			uboot_updated = 1;

		/* this is really not a good idea, but it's what the */
		/* customer wants. */
		cnt = 0;
		do {
			res = au_do_update(i, sz);
			/* let the user break out of the loop */
			if (ctrlc() || had_ctrlc()) {
				clear_ctrlc();

				break;
			}
			cnt++;
#ifdef AU_TEST_ONLY
		} while (res < 0 && cnt < 3);
		if (cnt < 3)
#else
		} while (res < 0);
#endif
	}

	if (1 == uboot_updated)
		return 1;
	if (1 == image_found)
		return 0;

	return -1;
}

/*
 * This is called by board_init() after the hardware has been set up
 * and is usable. Only if SPI flash initialization failed will this function
 * return -1, otherwise it will return 0;
 */
int do_auto_update(void)
{
	block_dev_desc_t *stor_dev;
	int old_ctrlc;
	int j;
	int state = -1;
	int dev;

	au_stor_curr_dev = -1;
	for (j = 0; j < MAX_UPDATE_INTF; j++) {
		if (0 != (unsigned int)s_intf[j].name[0]) {
			au_stor_curr_dev = s_intf[j].init();
			if (-1 == au_stor_curr_dev) {
				debug("No %s storage device found!\n",
						s_intf[j].name);
				continue;
			}

			dev = 0;

#if (defined CONFIG_HI3516CV300 || defined CONFIG_ARCH_HI3519 || \
		defined CONFIG_ARCH_HI3519V101 || defined CONFIG_ARCH_HI3516AV200)
			if (strncmp("mmc", s_intf[j].name, sizeof("mmc")) == 0)
				dev = 2;
#endif
			debug("device name %s!\n", s_intf[j].name);
			stor_dev = get_dev(s_intf[j].name, dev);
			if (NULL == stor_dev) {
				debug("Unknow device type!\n");
				continue;
			}

			if (fat_register_device(stor_dev, 1) != 0) {
				debug("Unable to use %s %d:%d for fatls\n",
						s_intf[j].name,
						au_stor_curr_dev,
						1);
				continue;
			}

			if (file_fat_detectfs() != 0) {
				debug("file_fat_detectfs failed\n");
				continue;
			}

			/*
			 * Get image layout from environment.
			 * If the start address and the end address
			 * were not definedin environment virables,
			 * use the default value
			 */
			get_update_env("firmware_st", "firmware_nd");
			get_update_env("kernel_st", "kernel_nd");
			get_update_env("rootfs_st", "rootfs_nd");

			/*
			 * make sure that we see CTRL-C
			 * and save the old state
			 */
			old_ctrlc = disable_ctrlc(0);

			/*
			 * CONFIG_SF_DEFAULT_SPEED=1000000,
			 * CONFIG_SF_DEFAULT_MODE=0x3
			 */
			flash = spi_flash_probe(0, 0, 1000000, 0x3);
			if (!flash) {
				printf("Failed to initialize SPI flash\n");
				return -1;
			}

			/*
			 * Explicit non-destructive migration test mode.
			 * MIGTEST.CFG takes precedence over both the guarded
			 * FULLNOR path and the legacy autoupdate path.
			 */
			if (openipc_migrate_test(flash)) {
			        state = 0;
			        disable_ctrlc(old_ctrlc);
			        s_intf[j].exit();
			        break;
			}

			/*
			 * Guarded real migration path. MIGARM.CFG is an explicit
			 * destructive acknowledgement, but the staged engine itself
			 * remains compile-time disabled until separately audited.
			 */
			if (openipc_migrate_handle(flash)) {
			        state = 0;
			        disable_ctrlc(old_ctrlc);
			        s_intf[j].exit();
			        break;
			}

			/*
			 * OpenIPC guarded whole-NOR migration path.
			 * If no full-NOR marker is present, retain the normal
			 * legacy autoupdate behaviour unchanged.
			 */
			{
				int fullnor_state = fullnor_handle(flash);

				if (fullnor_state != FULLNOR_NOT_REQUESTED) {
					disable_ctrlc(old_ctrlc);
					s_intf[j].exit();

					if (fullnor_state == FULLNOR_HANDLED) {
						printf("FULLNOR: request handled; skipping legacy updater\n");
						return 0;
					}

					printf("FULLNOR: request FAILED; skipping legacy updater\n");
					return -1;
				}
			}

			state = update_to_flash();

			/* restore the old state */
			disable_ctrlc(old_ctrlc);

			s_intf[j].exit();

			/*
			 * no update file found
			 */
			if (-1 == state)
				continue;
			/*
			 * update files have been found on current medium,
			 * so just break here
			 */
			break;
		}
	}

	/*
	 * If u-boot has been updated, it's better to save environment to flash
	 */
	if (1 == state) {
		env_crc_update();
		saveenv();
	}

	return 0;
}
#endif /* CONFIG_AUTO_UPDATE */
