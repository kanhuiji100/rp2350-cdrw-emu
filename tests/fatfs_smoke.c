/* Host-side FatFs smoke test against a real MBR/FAT32 image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"

static FILE *g_img;

PARTITION VolToPart[FF_VOLUMES] = {{0, 0}};

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 && g_img ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    return g_img ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !g_img) return RES_ERROR;
    if (fseek(g_img, (long)sector * 512, SEEK_SET) != 0) return RES_ERROR;
    if (fread(buff, 512, count, g_img) != count) return RES_ERROR;
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !g_img) return RES_ERROR;
    if (fseek(g_img, (long)sector * 512, SEEK_SET) != 0) return RES_ERROR;
    if (fwrite(buff, 512, count, g_img) != count) return RES_ERROR;
    fflush(g_img);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC: fflush(g_img); return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = 64 * 1024 * 1024 / 512;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s image\n", argv[0]);
        return 2;
    }
    g_img = fopen(argv[1], "r+b");
    if (!g_img) {
        perror("open");
        return 2;
    }
    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        fprintf(stderr, "f_mount failed: %d\n", fr);
        return 1;
    }
    printf("mounted, fat type %d, %lu free clusters\n", fs.fs_type,
           (unsigned long)fs.free_clst);

    DIR d;
    FILINFO fi;
    fr = f_opendir(&d, "/");
    if (fr != FR_OK) {
        fprintf(stderr, "opendir failed: %d\n", fr);
        return 1;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        printf("  %c %8lu %s\n", (fi.fattrib & AM_DIR) ? 'd' : '-',
               (unsigned long)fi.fsize, fi.fname);
    }
    f_closedir(&d);

    FIL f;
    UINT br = 0;
    char buf[64] = {0};
    fr = f_open(&f, "/HELLO.TXT", FA_READ);
    if (fr == FR_OK) {
        f_read(&f, buf, sizeof(buf) - 1, &br);
        f_close(&f);
        printf("hello.txt (%u): %s", br, buf);
    } else {
        printf("hello.txt not found (%d)\n", fr);
    }

    fr = f_open(&f, "/PREALLOC.BIN", FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
    if (fr == FR_OK) {
        fr = f_expand(&f, 4 * 1024 * 1024, 0);
        f_close(&f);
        printf("f_expand 4MiB: %d size=%lu\n", fr,
               (unsigned long)fi.fsize);
    } else {
        printf("create prealloc failed: %d\n", fr);
    }
    f_unmount("");
    fclose(g_img);
    return 0;
}
