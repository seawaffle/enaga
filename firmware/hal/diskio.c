#include "hal.h"
#include "ff.h"
#include "diskio.h"

/* FatFs disk I/O over the MMCSD0 driver (drive 0 only). */
DSTATUS disk_status(BYTE drv) { return (drv == 0 && sd_present()) ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE drv) { return (drv == 0 && sd_init() == 0) ? 0 : STA_NOINIT; }

/* The driver moves words, so it takes only word-aligned buffers. FatFs reads and writes whole sectors straight in
 * the caller's buffer, which is anywhere (an app reading from offset 314 has its whole sectors at buffer + 198):
 * those go through an aligned buffer, BOUNCE_SECTORS at a time, so they still use multi-block commands (one sector
 * a command runs at about 170 KiB/s, below a stereo recording's 187.5). */
#define BOUNCE_SECTORS 16u
static u32 bounce[128 * BOUNCE_SECTORS];

DRESULT disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count)
{
    UINT i, j, n;
    if (drv != 0) return RES_PARERR;
    if (!((u32)buff & 3)) return sd_read(buff, (u32)sector, count) == 0 ? RES_OK : RES_ERROR;
    for (i = 0; i < count; i += n) {
        n = count - i < BOUNCE_SECTORS ? count - i : BOUNCE_SECTORS;
        if (sd_read((u8 *)bounce, (u32)sector + i, n) != 0) return RES_ERROR;
        for (j = 0; j < 512 * n; j++) buff[512 * i + j] = ((const u8 *)bounce)[j];
    }
    return RES_OK;
}
DRESULT disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count)
{
    UINT i, j, n;
    if (drv != 0) return RES_PARERR;
    if (!((u32)buff & 3)) return sd_write(buff, (u32)sector, count) == 0 ? RES_OK : RES_ERROR;
    for (i = 0; i < count; i += n) {
        n = count - i < BOUNCE_SECTORS ? count - i : BOUNCE_SECTORS;
        for (j = 0; j < 512 * n; j++) ((u8 *)bounce)[j] = buff[512 * i + j];
        if (sd_write((const u8 *)bounce, (u32)sector + i, n) != 0) return RES_ERROR;
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
