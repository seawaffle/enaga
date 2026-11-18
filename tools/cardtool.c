/* Host card tool (tools/card.py builds and runs it): FAT and exFAT card images through Enaga's own FatFs, with
 * long names, so tests see the card exactly as the firmware does.
 *
 *   cardtool IMG make SIZE_MB fat|exfat     a new image: one partition, as cards come
 *   cardtool IMG put CARD_PATH LOCAL        copy a file in (folders created)
 *   cardtool IMG get CARD_PATH LOCAL        copy a file out (exit 3 if missing)
 *   cardtool IMG ls CARD_PATH               one line per entry: "D name" or "F size name"
 *   cardtool IMG free                       free bytes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

static unsigned char *disk;
static LBA_t sectors;

DSTATUS disk_status(BYTE d) { (void)d; return 0; }
DSTATUS disk_initialize(BYTE d) { (void)d; return 0; }
DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) { (void)d; memcpy(b, disk + (size_t)s * 512, n * 512u); return RES_OK; }
DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) { (void)d; memcpy(disk + (size_t)s * 512, b, n * 512u); return RES_OK; }
DRESULT disk_ioctl(BYTE d, BYTE cmd, void *buff)
{
    (void)d;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    }
    return RES_PARERR;
}

static int fail(const char *what, int code) { fprintf(stderr, "cardtool: %s (FatFs %d)\n", what, code); return 1; }

static void mkdirs(const char *path)
{
    char tmp[1024];
    size_t i;
    strncpy(tmp, path, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;
    for (i = 1; tmp[i]; i++)
        if (tmp[i] == '/') { tmp[i] = 0; f_mkdir(tmp); tmp[i] = '/'; }
}

static int load(const char *image)
{
    FILE *f = fopen(image, "rb");
    long size;
    if (!f) return fail("cannot open the image", 0);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sectors = (LBA_t)(size / 512);
    disk = malloc((size_t)size);
    if (!disk || fread(disk, 1, (size_t)size, f) != (size_t)size) return fail("cannot read the image", 0);
    fclose(f);
    return 0;
}

static int save(const char *image)
{
    FILE *f = fopen(image, "wb");
    if (!f || fwrite(disk, 512, (size_t)sectors, f) != (size_t)sectors) return fail("cannot write the image", 0);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static FATFS fs;
    static BYTE work[FF_MAX_SS * 8];
    static unsigned char buf[65536];
    FRESULT r;
    if (argc < 3) { fprintf(stderr, "usage: see tools/cardtool.c\n"); return 2; }
    if (!strcmp(argv[2], "make")) {
        MKFS_PARM opt = { 0, 0, 0, 0, 0 };
        if (argc != 5) return 2;
        opt.fmt = !strcmp(argv[4], "exfat") ? FM_EXFAT : FM_FAT | FM_FAT32;
        sectors = (LBA_t)atoi(argv[3]) * 2048;
        disk = calloc((size_t)sectors, 512);
        if ((r = f_mkfs("", &opt, work, sizeof work)) != FR_OK) return fail("format failed", r);
        return save(argv[1]);
    }
    if (load(argv[1])) return 1;
    if ((r = f_mount(&fs, "", 1)) != FR_OK) return fail("mount failed", r);
    if (!strcmp(argv[2], "put") && argc == 5) {
        FIL f;
        FILE *in = fopen(argv[4], "rb");
        size_t n;
        UINT put;
        if (!in) return fail("cannot read the local file", 0);
        mkdirs(argv[3]);
        if ((r = f_open(&f, argv[3], FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) return fail("cannot create the file", r);
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            if ((r = f_write(&f, buf, (UINT)n, &put)) != FR_OK || put != n) return fail("write failed (card full?)", r);
        fclose(in);
        f_close(&f);
    } else if (!strcmp(argv[2], "get") && argc == 5) {
        FIL f;
        FILE *out;
        UINT got;
        if (f_open(&f, argv[3], FA_READ) != FR_OK) return 3;
        out = fopen(argv[4], "wb");
        if (!out) return fail("cannot write the local file", 0);
        while ((r = f_read(&f, buf, sizeof buf, &got)) == FR_OK && got) fwrite(buf, 1, got, out);
        fclose(out);
        f_close(&f);
        return r == FR_OK ? 0 : fail("read failed", r);
    } else if (!strcmp(argv[2], "ls") && argc == 4) {
        DIR dir;
        FILINFO info;
        if ((r = f_opendir(&dir, argv[3])) != FR_OK) return 3;
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if (info.fattrib & AM_DIR) printf("D %s\n", info.fname);
            else printf("F %llu %s\n", (unsigned long long)info.fsize, info.fname);
        }
        f_closedir(&dir);
        return 0;
    } else if (!strcmp(argv[2], "free") && argc == 3) {
        DWORD clusters;
        FATFS *p;
        if ((r = f_getfree("", &clusters, &p)) != FR_OK) return fail("getfree failed", r);
        printf("%llu\n", (unsigned long long)clusters * p->csize * 512u);
        return 0;
    } else {
        return 2;
    }
    f_unmount("");
    return save(argv[1]);
}
