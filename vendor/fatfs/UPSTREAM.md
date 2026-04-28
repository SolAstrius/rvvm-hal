# FatFs upstream

- Project:   FatFs - Generic FAT Filesystem Module
- Author:    ChaN <http://elm-chan.org/fsw/ff/>
- Version:   R0.15a (2024)
- License:   FatFs license (BSD-1-clause: attribution only)
- Source:    http://elm-chan.org/fsw/ff/arc/ff15a.zip
- SHA256:    74737b1cafa1a67a3f722dd0d1a44767c5b54d37b6300ad3825a904cbe88fc3c

## Files vendored

| file              | source                | purpose |
|-------------------|-----------------------|---------|
| ff.c              | upstream verbatim     | FAT/exFAT core (read/write/dir/mkfs) |
| ff.h              | upstream verbatim     | public API |
| ffunicode.c       | upstream verbatim     | LFN code-page translations |
| diskio.h          | upstream verbatim     | block-device API the glue implements |
| ffconf.h          | **HAL-customised**    | exFAT-only-relevant config (US codepage, no CJK, no RTC) |

## Files NOT vendored

| file        | reason |
|-------------|--------|
| diskio.c    | replaced by `src/fatfs_disk.c` — wires to rvvm-hal NVMe |
| ffsystem.c  | replaced by `src/fatfs_disk.c` — `ff_memalloc/free` route to picolibc, `ff_mutex_*` are no-ops (single-hart access) |

## Update procedure

When ChaN releases a new version:

1. Download `http://elm-chan.org/fsw/ff/arc/ff<NN>.zip`, verify SHA256
2. Diff `source/ff.c source/ff.h source/diskio.h source/ffunicode.c` against vendored copies
3. Re-vendor verbatim if the diff is mechanical
4. Re-check `ffconf.h` against the upstream `ffconf.h` for any new options that need our defaults (we customise heavily here)
5. Bump `Version:` and `SHA256:` lines above
