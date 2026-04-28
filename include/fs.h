/* fs — FAT / exFAT filesystem on NVMe via vendored FatFs.
 *
 * Mounts an NVMe namespace as a FatFs volume. The whole on-disk
 * format is universal — Linux/macOS/Windows all mount the resulting
 * RVVM disk image natively, so consumers can drop save files into
 * an emulator's NVMe-backed drive by editing the host file directly.
 *
 * Typical use:
 *
 *     #include "fs.h"
 *     #include "ff.h"
 *
 *     nvme_t disk;
 *     nvme_init(&disk);              // pick controller 0
 *     fs_mount(&disk, "0:");         // wire FatFs to that controller
 *
 *     FIL fp;
 *     if (f_open(&fp, "0:/save.bin", FA_READ) == FR_OK) {
 *         UINT got;
 *         f_read(&fp, buf, sizeof(buf), &got);
 *         f_close(&fp);
 *     }
 *
 * fs.h itself is intentionally thin — once fs_mount is called, the
 * full FatFs `<ff.h>` API becomes available (f_open, f_read, f_write,
 * f_close, f_lseek, f_stat, f_unlink, f_mkdir, f_rename, f_mkfs, etc).
 *
 * One drive at a time today (FF_VOLUMES=1 in vendor/fatfs/ffconf.h).
 * Bump that and fs_mount's drive parameter to support more.
 *
 * Compiled only when HAL_FATFS=1. The Makefile gate excludes both
 * vendor/fatfs/ff.c and src/fatfs_disk.c from the default libhal.a
 * build, so consumers that don't need a filesystem don't pay for
 * 22 KiB of compiled FatFs code in their firmware. */

#pragma once
#include <stdbool.h>

#ifdef HAL_FATFS

#include "nvme.h"

/* Bind FatFs drive `path` (e.g. "0:") to a previously-`nvme_init`'d
 * controller. Returns true if the volume was recognised (any
 * FatFs-supported FAT or exFAT format). After this, the standard
 * FatFs `<ff.h>` API is usable on `path`. The `nvme_t *` must remain
 * valid for the lifetime of FS access — fs.h does not copy it. */
bool fs_mount(nvme_t *disk, const char *path);

/* Unmount and detach the bound NVMe. After this, FatFs calls on
 * `path` fail until a new fs_mount. Equivalent to `f_unmount(path)`
 * + clearing the disk binding. */
void fs_unmount(const char *path);

#endif /* HAL_FATFS */
