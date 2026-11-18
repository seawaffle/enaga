#include "hal.h"
#include "ff.h"
#include "diskio.h"

/* FatFs disk I/O over the MMCSD0 driver (drive 0 only). */
DSTATUS disk_status(BYTE drv) { return (drv == 0 && sd_present()) ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE drv) { return (drv == 0 && sd_init() == 0) ? 0 : STA_NOINIT; }

/* The driver moves words, so it takes only word-aligned buffers. FatFs reads and writes whole sectors straight in
 * the caller's buffer, which is anywhere (an app reading from offset 314 has its whole sectors at buffer + 198):
 * those go through an aligned sector, one at a time. */
static u32 bounce[128];

DRESULT disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count)
{
    UINT i, j;
    if (drv != 0) return RES_PARERR;
    if (!((u32)buff & 3)) return sd_read(buff, (u32)sector, count) == 0 ? RES_OK : RES_ERROR;
    for (i = 0; i < count; i++) {
        if (sd_read((u8 *)bounce, (u32)sector + i, 1) != 0) return RES_ERROR;
        for (j = 0; j < 512; j++) buff[512 * i + j] = ((const u8 *)bounce)[j];
    }
    return RES_OK;
}
DRESULT disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count)
{
    UINT i, j;
    if (drv != 0) return RES_PARERR;
    if (!((u32)buff & 3)) return sd_write(buff, (u32)sector, count) == 0 ? RES_OK : RES_ERROR;
    for (i = 0; i < count; i++) {
        for (j = 0; j < 512; j++) ((u8 *)bounce)[j] = buff[512 * i + j];
        if (sd_write((const u8 *)bounce, (u32)sector + i, 1) != 0) return RES_ERROR;
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drv, BYTE cmd, void *buff)
{
    (void)buff;
    if (drv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
