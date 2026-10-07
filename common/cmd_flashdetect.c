#include <common.h>
#include <command.h>
#include <image.h>
#include <spi_flash.h>

#define FD_SPI_SPEED            1000000
#define FD_SPI_MODE             0x3

#define FD_8M_SIZE              0x00800000UL
#define FD_16M_SIZE             0x01000000UL

#define FD_OPENIPC_KERNEL_OFF   0x00050000UL
#define FD_OPENIPC8_KERNEL_MAX  0x00200000UL
#define FD_OPENIPC8_ROOTFS_OFF  0x00250000UL

#define FD_OPENIPC16_KERNEL_MAX 0x00300000UL
#define FD_OPENIPC16_ROOTFS_OFF 0x00350000UL

#define FD_FACTORY8_KERNEL_OFF    0x00080000UL
#define FD_FACTORY8_KERNEL_MAX    0x00280000UL
#define FD_FACTORY8_ROOTFS_OFF    0x00300000UL

#define FD_FACTORY16_KERNEL_OFF   0x00080000UL
#define FD_FACTORY16_KERNEL_MAX   0x00380000UL
#define FD_FACTORY16_ROOTFS_OFF   0x00400000UL

#define FD_FACTORY8_BOOTCMD \
        "setenv bootargs mem=32M console=ttyAMA0,115200 " \
        "root=/dev/mtdblock4 rootfstype=squashfs " \
        "mtdparts=hi_sfc:256k(boot),128k(env),128k(conf)," \
        "2560k(os),5120k(rootfs);" \
        "sf probe 0;" \
        "sf read 0x82000000 0x80000 0x280000;" \
        "bootm 0x82000000"

#define FD_FACTORY16_BOOTCMD \
        "setenv bootargs mem=32M console=ttyAMA0,115200 " \
        "root=/dev/mtdblock4 rootfstype=squashfs " \
        "mtdparts=hi_sfc:256k(boot),128k(env),128k(conf)," \
        "3584k(os),4096k(rootfs),4608k(userfs),3584k(extfs);" \
        "sf probe 0;" \
        "sf read 0x82000000 0x80000 0x380000;" \
        "bootm 0x82000000"

#define FD_OPENIPC8_BOOTCMD \
        "setenv bootargs mem=32M console=ttyAMA0,115200 panic=20 " \
        "root=/dev/mtdblock3 rootfstype=squashfs init=/init " \
        "mtdparts=hi_sfc:256k(boot),64k(env),2048k(kernel)," \
        "5120k(rootfs),-(rootfs_data);" \
        "sf probe 0;" \
        "sf read 0x82000000 0x50000 0x300000;" \
        "bootm 0x82000000"

enum flash_layout {
        FLASH_LAYOUT_UNKNOWN = 0,
        FLASH_LAYOUT_FACTORY_EV200_8M,
        FLASH_LAYOUT_FACTORY_EV200_16M,
        FLASH_LAYOUT_OPENIPC_8M,
        FLASH_LAYOUT_OPENIPC_16M,
        FLASH_LAYOUT_AMBIGUOUS
};

static int fd_read(struct spi_flash *flash, u32 off, u32 len, void *buf)
{
        if ((u64)off + len > flash->size)
                return -1;

        return flash->read(flash, off, len, buf);
}

static int fd_check_squashfs(struct spi_flash *flash, u32 off)
{
        unsigned char magic[4];

        if (fd_read(flash, off, sizeof(magic), magic))
                return 0;

        return magic[0] == 'h' &&
               magic[1] == 's' &&
               magic[2] == 'q' &&
               magic[3] == 's';
}

static int fd_check_kernel(struct spi_flash *flash, u32 off, u32 max_len,
                image_header_t *out)
{
        image_header_t hdr;
        u32 image_len;

        if (fd_read(flash, off, sizeof(hdr), &hdr))
                return 0;

        if (!image_check_magic(&hdr))
                return 0;

        if (!image_check_hcrc(&hdr))
                return 0;

        if (!image_check_arch(&hdr, IH_ARCH_ARM))
                return 0;

        if (!image_check_type(&hdr, IH_TYPE_KERNEL))
                return 0;

        image_len = image_get_image_size(&hdr);

        if (image_len < sizeof(image_header_t))
                return 0;

        if (image_len > max_len)
                return 0;

        if ((u64)off + image_len > flash->size)
                return 0;

        if (out)
                memcpy(out, &hdr, sizeof(hdr));

        return 1;
}

static void fd_print_kernel(const char *label, u32 off, int valid,
                const image_header_t *hdr)
{
        if (!valid) {
                printf("FLASHDETECT: %-15s @ 0x%08x: absent/invalid\n",
                        label, off);
                return;
        }

        printf("FLASHDETECT: %-15s @ 0x%08x: valid uImage\n",
                label, off);
        printf("FLASHDETECT:   name : %.32s\n", image_get_name(hdr));
        printf("FLASHDETECT:   size : 0x%08x\n",
                image_get_image_size(hdr));
        printf("FLASHDETECT:   load : 0x%08x\n",
                image_get_load(hdr));
        printf("FLASHDETECT:   entry: 0x%08x\n",
                image_get_ep(hdr));
}

static enum flash_layout fd_detect(struct spi_flash *flash)
{
        image_header_t factory_hdr;
        image_header_t openipc_hdr;

        int factory_kernel = 0;
        int factory_rootfs = 0;
        int openipc_kernel = 0;
        int openipc_rootfs = 0;

        int factory8_match = 0;
        int factory16_match = 0;
        int openipc8_match = 0;
        int openipc16_match = 0;
        int matches = 0;

        printf("\n");
        printf("========================================\n");
        printf(" SPI firmware layout detector\n");
        printf(" SPI NOR size: 0x%08x (%u MiB)\n",
                flash->size, flash->size >> 20);
        printf("========================================\n");

        if (flash->size == FD_8M_SIZE) {
                factory_kernel = fd_check_kernel(flash,
                                FD_FACTORY8_KERNEL_OFF,
                                FD_FACTORY8_KERNEL_MAX,
                                &factory_hdr);

                factory_rootfs = fd_check_squashfs(flash,
                                FD_FACTORY8_ROOTFS_OFF);

                fd_print_kernel("factory8 kernel",
                                FD_FACTORY8_KERNEL_OFF,
                                factory_kernel,
                                &factory_hdr);

                printf("FLASHDETECT: factory8 rootfs @ 0x%08x: %s\n",
                        FD_FACTORY8_ROOTFS_OFF,
                        factory_rootfs ? "SquashFS" : "absent/invalid");

                factory8_match = factory_kernel && factory_rootfs;

                if (factory8_match)
                        matches++;
        }

        if (flash->size == FD_16M_SIZE) {
                factory_kernel = fd_check_kernel(flash,
                                FD_FACTORY16_KERNEL_OFF,
                                FD_FACTORY16_KERNEL_MAX,
                                &factory_hdr);

                factory_rootfs = fd_check_squashfs(flash,
                                FD_FACTORY16_ROOTFS_OFF);

                fd_print_kernel("factory16 kernel",
                                FD_FACTORY16_KERNEL_OFF,
                                factory_kernel,
                                &factory_hdr);

                printf("FLASHDETECT: factory16 rootfs @ 0x%08x: %s\n",
                        FD_FACTORY16_ROOTFS_OFF,
                        factory_rootfs ? "SquashFS" : "absent/invalid");

                factory16_match = factory_kernel && factory_rootfs;

                if (factory16_match)
                        matches++;
        }

        if (flash->size == FD_8M_SIZE ||
            flash->size == FD_16M_SIZE) {
                u32 kernel_max;
                u32 rootfs_off;

                if (flash->size == FD_8M_SIZE) {
                        kernel_max = FD_OPENIPC8_KERNEL_MAX;
                        rootfs_off = FD_OPENIPC8_ROOTFS_OFF;
                } else {
                        kernel_max = FD_OPENIPC16_KERNEL_MAX;
                        rootfs_off = FD_OPENIPC16_ROOTFS_OFF;
                }

                openipc_kernel = fd_check_kernel(flash,
                                FD_OPENIPC_KERNEL_OFF,
                                kernel_max,
                                &openipc_hdr);

                openipc_rootfs = fd_check_squashfs(flash,
                                rootfs_off);

                fd_print_kernel("OpenIPC kernel",
                                FD_OPENIPC_KERNEL_OFF,
                                openipc_kernel,
                                &openipc_hdr);

                printf("FLASHDETECT: OpenIPC rootfs   @ 0x%08x: %s\n",
                        rootfs_off,
                        openipc_rootfs ? "SquashFS" : "absent/invalid");

                if (flash->size == FD_8M_SIZE)
                        openipc8_match =
                                openipc_kernel && openipc_rootfs;
                else
                        openipc16_match =
                                openipc_kernel && openipc_rootfs;

                if (openipc8_match)
                        matches++;

                if (openipc16_match)
                        matches++;
        }

        if (matches > 1)
                return FLASH_LAYOUT_AMBIGUOUS;

        if (factory8_match)
                return FLASH_LAYOUT_FACTORY_EV200_8M;

        if (factory16_match)
                return FLASH_LAYOUT_FACTORY_EV200_16M;

        if (openipc8_match)
                return FLASH_LAYOUT_OPENIPC_8M;

        if (openipc16_match)
                return FLASH_LAYOUT_OPENIPC_16M;

        return FLASH_LAYOUT_UNKNOWN;
}

static const char *fd_layout_name(enum flash_layout layout)
{
        switch (layout) {
        case FLASH_LAYOUT_FACTORY_EV200_8M:
                return "FACTORY_EV200_8M";

        case FLASH_LAYOUT_FACTORY_EV200_16M:
                return "FACTORY_EV200_16M";

        case FLASH_LAYOUT_OPENIPC_8M:
                return "OPENIPC_8M";

        case FLASH_LAYOUT_OPENIPC_16M:
                return "OPENIPC_16M";

        case FLASH_LAYOUT_AMBIGUOUS:
                return "AMBIGUOUS";

        default:
                return "UNKNOWN";
        }
}

static enum flash_layout fd_probe_and_detect(void)
{
        struct spi_flash *flash;
        enum flash_layout layout;

        flash = spi_flash_probe(0, 0, FD_SPI_SPEED, FD_SPI_MODE);

        if (!flash) {
                printf("FLASHDETECT: failed to probe SPI NOR\n");
                return FLASH_LAYOUT_UNKNOWN;
        }

        layout = fd_detect(flash);

        printf("\nFLASHDETECT: result: %s\n",
                fd_layout_name(layout));

        spi_flash_free(flash);

        return layout;
}

/*
 * Called from main_loop before the ordinary autoboot countdown.
 *
 * Only modifies the in-RAM environment.  It never calls saveenv.
 */
int flashdetect_configure_autoboot(void)
{
        enum flash_layout layout;

        layout = fd_probe_and_detect();

        switch (layout) {
        case FLASH_LAYOUT_FACTORY_EV200_8M:
                printf("AUTOBOOT: selecting factory EV200 8M boot path\n");
                setenv("bootcmd", FD_FACTORY8_BOOTCMD);
                return 0;

        case FLASH_LAYOUT_FACTORY_EV200_16M:
                printf("AUTOBOOT: selecting factory EV200 16M boot path\n");
                setenv("bootcmd", FD_FACTORY16_BOOTCMD);
                return 0;

        case FLASH_LAYOUT_OPENIPC_8M:
                printf("AUTOBOOT: selecting OpenIPC 8M boot path\n");
                setenv("bootcmd", FD_OPENIPC8_BOOTCMD);
                return 0;

        case FLASH_LAYOUT_OPENIPC_16M:
                printf("AUTOBOOT: OpenIPC 16M detected but not yet "
                       "enabled for automatic boot\n");
                setenv("bootcmd", NULL);
                return 1;

        case FLASH_LAYOUT_AMBIGUOUS:
                printf("AUTOBOOT: ambiguous flash layout; "
                       "automatic boot disabled\n");
                setenv("bootcmd", NULL);
                return 1;

        default:
                printf("AUTOBOOT: unknown flash layout; "
                       "automatic boot disabled\n");
                setenv("bootcmd", NULL);
                return 1;
        }
}

static int do_flashdetect(cmd_tbl_t *cmdtp, int flag, int argc, char *argv[])
{
        enum flash_layout layout;

        layout = fd_probe_and_detect();

        if (layout == FLASH_LAYOUT_UNKNOWN ||
            layout == FLASH_LAYOUT_AMBIGUOUS)
                return 1;

        return 0;
}

U_BOOT_CMD(
        flashdetect, 1, 1, do_flashdetect,
        "detect known firmware layout in SPI NOR",
        "- read-only detection of factory/OpenIPC layouts"
);
