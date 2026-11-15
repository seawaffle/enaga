#include "hal.h"
#include "ff.h"
#include "diskio.h"

/* FatFs disk I/O over the MMCSD0 driver (drive 0 only). */
DSTATUS disk_status(BYTE drv) { return (drv == 0 && sd_present()) ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE drv) { return (drv == 0 && sd_init() == 0) ? 0 : STA_NOINIT; }
DRESULT disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count)
{
    return (drv == 0 && sd_read(buff, (u32)sector, count) == 0) ? RES_OK : RES_ERROR;
}
DRESULT disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count)
{
    return (drv == 0 && sd_write(buff, (u32)sector, count) == 0) ? RES_OK : RES_ERROR;
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
