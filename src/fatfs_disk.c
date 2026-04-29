/* fatfs_disk.c — FatFs ↔ rvvm-hal disk glue.
 *
 * Replaces upstream's `diskio.c` and `ffsystem.c` with implementations
 * that route FatFs's block-device API to our nvme_t driver. Single
 * volume bound at a time (FF_VOLUMES=1 in ffconf.h).
 *
 * Layered functions:
 *
 *   §1  fs_mount / fs_unmount         the public surface in include/fs.h
 *
 *   §2  disk_initialize / status / read / write / ioctl
 *                                     FatFs's diskio.h API; wires to
 *                                     the bound nvme_t
 *
 *   §3  ff_memalloc / ff_memfree
 *                                     FatFs heap hooks. Routes to
 *                                     picolibc's malloc/free (which
 *                                     consumer firmwares supply via
 *                                     bump allocator overrides, OR
 *                                     picolibc's nano-malloc).
 *
 *   §4  ff_mutex_create / delete / take / give
 *                                     no-ops at FF_FS_REENTRANT=0.
 *
 *   §5  get_fattime
 *                                     stub at FF_FS_NORTC=1 — never
 *                                     called, but the symbol must
 *                                     resolve.  When goldfish-rtc
 *                                     gets wired into the HAL,
 *                                     replace with the wallclock
 *                                     read.
 *
 * Compiled only when HAL_FATFS=1; gated below to avoid emitting
 * stubs in firmwares that don't include FatFs. */

#ifdef HAL_FATFS

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>     /* malloc/free for ff_memalloc */

#include "ff.h"
#include "diskio.h"
#include "fs.h"
#include "uart.h"
#include "nvme.h"

/* ====================================================================
 * §1. Public mount API
 *
 * Stash the nvme_t pointer per-drive. With FF_VOLUMES=1 this is just
 * a single global; if FF_VOLUMES grows, change to an array indexed
 * by FatFs's pdrv (BYTE 0..FF_VOLUMES-1).
 * ==================================================================== */

static nvme_t *fs_disk[FF_VOLUMES];   /* one per FatFs drive */
static FATFS   fs_state[FF_VOLUMES];  /* FatFs's per-drive state */

bool fs_mount(nvme_t *disk, const char *path) {
    if (!disk || !disk->present) {
        uart_puts("fs_mount: nvme_t isn't initialised or has no namespace\n");
        return false;
    }
    /* FatFs uses a numeric drive prefix at the start of the path
     * ("0:", "1:", ...). Decode it to pick the slot. */
    int drv = 0;
    if (path && path[0] >= '0' && path[0] <= '9' && path[1] == ':') {
        drv = path[0] - '0';
    }
    if (drv < 0 || drv >= FF_VOLUMES) {
        uart_printf("fs_mount: drive %d out of range (FF_VOLUMES=%u)\n",
                    (int64_t)drv, (uint64_t)FF_VOLUMES);
        return false;
    }
    fs_disk[drv] = disk;

    /* Eager mount (second argument == 1) so we surface format errors
     * up front rather than on first f_open. */
    FRESULT r = f_mount(&fs_state[drv], path, 1);
    if (r != FR_OK) {
        uart_printf("fs_mount: f_mount(\"%s\") failed (FRESULT=%d)\n",
                    path, (int64_t)r);
        fs_disk[drv] = NULL;
        return false;
    }
    return true;
}

void fs_unmount(const char *path) {
    int drv = 0;
    if (path && path[0] >= '0' && path[0] <= '9' && path[1] == ':') {
        drv = path[0] - '0';
    }
    if (drv < 0 || drv >= FF_VOLUMES) return;
    f_unmount(path);
    fs_disk[drv] = NULL;
}

/* ====================================================================
 * §2. diskio.h backend
 *
 * pdrv is the FatFs drive number (0..FF_VOLUMES-1). Sector size is
 * fixed at 512 B (NVME_LBA_SIZE in nvme.h), and we report it via
 * GET_SECTOR_SIZE in disk_ioctl.
 * ==================================================================== */

static nvme_t *resolve(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES) return NULL;
    return fs_disk[pdrv];
}

DSTATUS disk_status(BYTE pdrv) {
    nvme_t *d = resolve(pdrv);
    if (!d || !d->present) return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    /* Initialised lazily by fs_mount — the nvme_t was prepared by the
     * caller's nvme_init. Return current status. */
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    nvme_t *d = resolve(pdrv);
    if (!d) return RES_PARERR;
    if (!d->present) return RES_NOTRDY;
    /* nvme_read returns the number of LBAs successfully read. */
    uint32_t got = nvme_read(d, sector, buff, count);
    return (got == count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    nvme_t *d = resolve(pdrv);
    if (!d) return RES_PARERR;
    if (!d->present) return RES_NOTRDY;
    uint32_t put = nvme_write(d, sector, buff, count);
    return (put == count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    nvme_t *d = resolve(pdrv);
    if (!d) return RES_PARERR;
    if (!d->present) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return nvme_flush(d) ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)d->num_lbas;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = NVME_LBA_SIZE;       /* 512 */
        return RES_OK;
    case GET_BLOCK_SIZE:
        /* Erase block size in sectors. NVMe doesn't expose this; 1 is
         * always safe (FatFs uses it for allocation hints). */
        *(DWORD *)buff = 1;
        return RES_OK;
    case CTRL_TRIM:
        /* TRIM disabled in ffconf.h (FF_USE_TRIM=0) — never called. */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* ====================================================================
 * §3. Heap hooks
 *
 * FatFs uses ff_memalloc / ff_memfree for LFN buffers and exFAT's
 * Unicode upcase table when those need to live longer than a stack
 * frame (~4 KiB). Routes to picolibc's malloc/free, which consumer
 * firmwares either back with picolibc's nano-malloc or override with
 * a bump allocator.
 * ==================================================================== */

void *ff_memalloc(UINT msize) {
    return malloc((size_t)msize);
}

void ff_memfree(void *mblock) {
    free(mblock);
}

/* ====================================================================
 * §4. Mutex hooks
 *
 * FF_FS_REENTRANT=0 means FatFs never calls these — but their symbols
 * still need to resolve. Provide no-ops; if a future consumer needs
 * concurrent FS access from multiple harts, bump FF_FS_REENTRANT and
 * back these with real atomics + wfi/MSIP wakeups.
 * ==================================================================== */

#if FF_FS_REENTRANT
int  ff_mutex_create(int vol)   { (void)vol; return 1; }
void ff_mutex_delete(int vol)   { (void)vol; }
int  ff_mutex_take  (int vol)   { (void)vol; return 1; }
void ff_mutex_give  (int vol)   { (void)vol; }
#endif

/* ====================================================================
 * §5. Wallclock for file timestamps
 *
 * FatFs encodes mtime/ctime as a 32-bit DOS-style date-time. With
 * FF_FS_NORTC=1, FatFs uses the FF_NORTC_* defaults from ffconf.h
 * and never calls get_fattime — but its symbol still needs to
 * resolve, so provide a stub returning the same constant. Once the
 * HAL exposes goldfish-rtc, flip FF_FS_NORTC to 0 and return real
 * wallclock here.
 * ==================================================================== */

/* FatFs's date-time format (FAT timestamp):
 *   bits 31:25  year - 1980  (0..127)
 *   bits 24:21  month        (1..12)
 *   bits 20:16  mday         (1..31)
 *   bits 15:11  hour         (0..23)
 *   bits 10:5   minute       (0..59)
 *   bits  4:0   sec / 2      (0..29 covers 0..58 even sec)
 *
 * With FF_FS_NORTC=1 in ffconf.h, FatFs uses its hardcoded defaults
 * and never calls this function — but the symbol still has to
 * resolve, and a future flip to FF_FS_NORTC=0 should land somewhere
 * useful, so we wire it to the goldfish RTC. */
#include "rtc.h"

DWORD get_fattime(void) {
    uint64_t secs = rtc_now_seconds();
    /* Days-from-epoch → Y/M/D using the standard civil-from-days
     * algorithm (Howard Hinnant, public domain).  Faster than a
     * mktime/gmtime call and dependency-free. */
    int64_t days = (int64_t)(secs / 86400);
    int sec_of_day = (int)(secs % 86400);
    int hour = sec_of_day / 3600;
    int min  = (sec_of_day / 60) % 60;
    int sec  = sec_of_day % 60;

    days += 719468;                  /* shift epoch to 0000-03-01 */
    int era = (int)((days >= 0 ? days : days - 146096) / 146097);
    unsigned doe = (unsigned)(days - (int64_t)era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int year = (int)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp  = (5*doy + 2) / 153;
    unsigned mday  = doy - (153*mp + 2)/5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2) year++;

    if (year < 1980) { year = 1980; month = 1; mday = 1; hour = min = sec = 0; }
    if (year > 2107) { year = 2107; }

    return ((DWORD)(year - 1980) << 25)
         | ((DWORD)month        << 21)
         | ((DWORD)mday         << 16)
         | ((DWORD)hour         << 11)
         | ((DWORD)min          <<  5)
         | ((DWORD)(sec / 2));
}

#endif /* HAL_FATFS */
