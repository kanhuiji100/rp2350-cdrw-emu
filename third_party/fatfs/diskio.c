/* FatFs block-device glue for the SPI microSD card. */
#include "ff.h"
#include "diskio.h"
#include "sd_spi.h"

/* Map logical volume 0 to physical drive 0, auto-detecting the first valid
 * FAT partition (also supports superfloppy-formatted cards). */
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}};

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT | STA_NODISK;
    }
    if (!sd_is_ready()) {
        return STA_NOINIT;
    }
    if (!sd_card_present()) {
        return STA_NOINIT | STA_NODISK;
    }
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT | STA_NODISK;
    }
    if (!sd_init()) {
        return STA_NOINIT | STA_NODISK;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !buff || !count) {
        return RES_PARERR;
    }
    return sd_read_blocks((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !buff || !count) {
        return RES_PARERR;
    }
    return sd_write_blocks((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) {
        return RES_PARERR;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return sd_sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        if (!buff) return RES_PARERR;
        *(LBA_t *)buff = sd_block_count();
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (!buff) return RES_PARERR;
        *(WORD *)buff = SD_BLOCK_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buff) return RES_PARERR;
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
