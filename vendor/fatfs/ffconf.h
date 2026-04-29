/* ffconf.h — rvvm-hal's FatFs configuration.
 *
 * Customised from upstream's `source/ffconf.h` for our use case:
 * single-hart M-mode firmware on RVVM with NVMe-backed disks. We
 * enable exFAT (universal host mount), strip CJK code pages
 * (~15 KiB of rodata gone), drop RTC dependency, drop reentrancy
 * locks (no concurrent FS access from secondary harts today).
 *
 * Bumping FatFs upstream means re-merging this file against the new
 * upstream ffconf.h — see vendor/fatfs/UPSTREAM.md. */

#define FFCONF_DEF	5380

/* ----------------- Function configurations ----------------- */

/* 0=read+write, 1=read-only. We want write. */
#define FF_FS_READONLY	0

/* Strip optional API: f_stat/access/getfree/truncate/utime/chmod/...
 * 0=full, 1=drop optional, 2=drop f_mkdir/f_rename, 3=tiny (read+write only). */
#define FF_FS_MINIMIZE	0

/* f_findfirst / f_findnext (filename pattern matching). Off — adds
 * code, no consumer needs it yet. */
#define FF_USE_FIND		0

/* f_mkfs — format a volume. On so consumers can format from firmware
 * (e.g. game-boy could `f_mkfs("0:", 0, work, 4096)` to wipe a save
 * volume). With gc-sections this is free if unused. */
#define FF_USE_MKFS		1

/* Fast-seek table (caches cluster chain). Off; our reads are sequential. */
#define FF_USE_FASTSEEK	0

/* f_expand / preallocate file. Off; not needed. */
#define FF_USE_EXPAND	0

/* f_chmod / chown (FAT attribute bits). Off. */
#define FF_USE_CHMOD	0

/* f_getlabel / setlabel (volume label). Off. */
#define FF_USE_LABEL	0

/* f_forward (callback-driven streaming reads). Off. */
#define FF_USE_FORWARD	0

/* String-based wrappers: f_gets/f_puts/f_printf. Off — picolibc has
 * fprintf, consumers can use that on top of f_open's FIL pointer. */
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	1
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3

/* ----------------- Locale & filenames ----------------- */

/* Code page. 437 = US English (no DBCS), strips ffunicode.c's CJK
 * tables. The exFAT path uses the on-disk Unicode upcase table for
 * case folding — code page only matters for FAT12/16/32 short
 * filenames. */
#define FF_CODE_PAGE	437

/* Long filename: 1 = enabled with stack buffer, 2 = heap, 3 = static.
 * Required for exFAT (which uses 16-bit Unicode names natively). */
#define FF_USE_LFN		1
#define FF_MAX_LFN		255

/* LFN string encoding the API uses. 0=ANSI/OEM, 1=UTF-16, 2=UTF-8,
 * 3=UTF-32. UTF-8 makes the API behave like POSIX paths. */
#define FF_LFN_UNICODE	2

/* Internal LFN buffer width — match FF_MAX_LFN. */
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12

/* Relative paths via f_chdir/getcwd. Off; firmware uses absolute. */
#define FF_FS_RPATH		0

/* ----------------- Volume / partition ----------------- */

/* Number of mountable volumes. 1 = single drive. Per consumer:
 * NVMe controller 0 (the cart, save image, BASIC programs, etc).
 * Future: bump to 2 to mount BIOS + ROM separately. */
#define FF_VOLUMES		1

/* String volume IDs (`f_open("data:/foo.bin", ...)`). Off — use 0:
 * numeric prefix or none. */
#define FF_STR_VOLUME_ID	0

/* MBR / GPT multi-partition. Off; whole device is one volume. */
#define FF_MULTI_PARTITION	0

/* ----------------- Sector / device ----------------- */

/* Sector size range. NVMe is 512 B fixed in our setup; allow 4 KiB
 * for future SSDs that report 4 KiB logical blocks. */
#define FF_MIN_SS	512
#define FF_MAX_SS	4096

/* 64-bit LBA. Off — our NVMe is small (chip-8 saves to game-boy carts,
 * all <64 GiB which fits in 32-bit LBAs at 512 B sectors). */
#define FF_LBA64		0
#define FF_MIN_GPT	0x10000000

/* TRIM / sector-erase. Off; RVVM's NVMe ignores it. */
#define FF_USE_TRIM	0

/* ----------------- Filesystem features ----------------- */

/* Tiny mode: shared sector buffer per-FS instead of per-FIL. Saves
 * ~512 B per open file. Off; we don't open many files concurrently. */
#define FF_FS_TINY	0

/* exFAT: required for >4 GiB files, Unicode names, and host-platform
 * universal mount (Linux/macOS/Windows all native). The whole reason
 * we vendored FatFs over hand-rolling ext2. */
#define FF_FS_EXFAT		1

/* Real-time clock for file timestamps. 0 = call get_fattime() (our
 * glue routes that to goldfish-rtc → real wallclock). 1 = use the
 * NORTC defaults below — kept as a fallback for firmwares that
 * don't initialise the RTC (rtc_init() is idempotent and cheap, so
 * there's rarely a reason). */
#define FF_FS_NORTC		0
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2024

/* Skip FSINFO sector caching of free cluster count. Off — we want it. */
#define FF_FS_NOFSINFO	0

/* File-locking via per-file lock table. 0 = no internal locking. */
#define FF_FS_LOCK		0

/* Reentrancy (mutex around volume-level state). 0 = single-threaded.
 * Today our consumers only use FatFs from the primary hart; if a
 * secondary hart needs FS access, switch this on and add real mutex
 * impls in `ff_mutex_*`. */
#define FF_FS_REENTRANT	0
