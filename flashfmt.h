/* flashfmt - shared declarations */
#ifndef FLASHFMT_H
#define FLASHFMT_H

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Keep buffer contracts visible to Clang without requiring ptrcheck.h on
 * Linux. */
#if defined(__clang__) && __has_include(<ptrcheck.h>)
#include <ptrcheck.h>
#else
#define __counted_by(n)
#define __counted_by_or_null(n)
#define __sized_by(n)
#define __null_terminated
#define __unsafe_indexable
#define __unsafe_forge_null_terminated(t, p) ((t)(p))
#define __unsafe_null_terminated_from_indexable(p) (p)
#endif

#include "flashfmt_tables.h"

#define SECTOR 512u
#define MIB (1024u * 1024u)

/* Devices larger than this (128 PiB) are rejected as implausible; it keeps
 * every byte offset far from 64-bit overflow. */
#define MAX_SECTORS (1ull << 48)

#define EXFAT_THRESHOLD 0x3FD8000ull /* sectors; above this -> exFAT */

/* ------------------------------------------------------------------------ */
/* Options (main.c)                                                          */

typedef struct {
    const char* device;
    const char* label;
    unsigned long codepage;
    int discard; /* 0 none, 1 discard, 2 secure discard */
    int relax;
    int align_cylinder;
    int overwrite;
    int force;
    int verbose;
    int debug;
    int progress_bar; /* -1 unset (auto), 0/1 */
    int machine_progress;
    int yes;
    int dry_run;
    int quiet;
    int allow_internal;
    uint64_t create_size;
    int have_serial, have_mbr_sig, have_disk_guid, have_part_guid;
    uint32_t serial, mbr_sig;
    uint8_t disk_guid[16], part_guid[16];
} options;

extern options opt;

/* ------------------------------------------------------------------------ */
/* Logging and helpers (util.c)                                              */

extern int formatting; /* set once the format itself has started */

#define PRINTF_LIKE __attribute__((format(printf, 1, 2)))
PRINTF_LIKE void dbg(const char* fmt, ...);
PRINTF_LIKE void vinfo(const char* fmt, ...);
PRINTF_LIKE void info(const char* fmt, ...);
PRINTF_LIKE void warn(const char* fmt, ...);
PRINTF_LIKE __attribute__((noreturn)) void die(const char* fmt, ...);

void* __sized_by(n) xcalloc(size_t n);
uint32_t random_u32(void);
void random_guid(uint8_t* __counted_by(16) g);
uint32_t crc32_buf(const uint8_t* __counted_by(n) p, size_t n);

static inline void put16(uint8_t* __counted_by(2) p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
static inline void put32(uint8_t* __counted_by(4) p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
static inline void put64(uint8_t* __counted_by(8) p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = v >> (8 * i);
}
static inline uint64_t div_up(uint64_t a, uint64_t b) {
    return (a + b - 1) / b;
}

/* ------------------------------------------------------------------------ */
/* Layout parameter tables, selected by device size (fat.c, exfat.c)        */

typedef struct {
    uint64_t max_sectors, min_sectors;
    uint32_t boundary_unit; /* sectors */
    uint32_t sectors_per_cluster;
    uint8_t heads, sectors_per_track, allowed, fat32;
} layout_row;

/* ------------------------------------------------------------------------ */
/* Device layer (device.c) and progress reporting (progress.c)              */

typedef struct {
    int fd;
    int lock_fd;   /* macOS: /dev/rdiskN, locked alongside /dev/diskN */
    int verify_fd; /* uncached read-back fd (-1: read back through fd) */
    char path[1024];
    char rnode[128]; /* macOS raw node */
    int is_file;
    int create;       /* image file doesn't exist yet; created by open_rw() */
    uint64_t sectors; /* in 512-byte units */
    uint64_t window;  /* byte offset added to every partition-relative I/O */
    uint64_t done, total; /* progress */
} device;

extern device dev;

void dev_pwrite(uint64_t off, const void* __sized_by(len) buf, size_t len);
int dev_pread_abs(int fd, uint64_t off, void* __sized_by(len) buf, size_t len);
void dev_sync(void);
void write_with_padding(uint64_t pos, const void* __sized_by(dlen) data,
                        size_t dlen, uint64_t padded, uint8_t pad,
                        int sync_interval);
int dev_discard(uint64_t off, uint64_t len, int secure);
void dev_refresh_partitions(void);
void open_device(void);
void open_rw(void);

extern const char* prog_msg;
void on_winch(int sig);
void refresh_term_cols(void);
void progress(uint64_t n);

/* ------------------------------------------------------------------------ */
/* Volume label (label.c) and output about the volume (report.c)            */

typedef struct {
    int set;
    uint8_t fat[11];
    int fat_len; /* code-page bytes, unpadded */
    uint16_t ex[11];
    int ex_len; /* UTF-16 */
} label_info;

extern label_info lbl;

void prepare_fat_label(void);
void prepare_exfat_label(void);
char* __null_terminated label_display(int exfat);
void verbose_header(int exfat);
void report_fs_info(const char* fstype, uint64_t cluster, uint64_t dev_total,
                    uint64_t fs_total, uint64_t fs_free, int exfat);
void report_result(const char* fstype, uint64_t capacity, uint64_t free_bytes,
                   uint64_t cluster, int exfat);

/* ------------------------------------------------------------------------ */
/* Write plan: the list of regions written during the format (plan.c)       */

typedef struct {
    const char* phase; /* printed when the phase starts (NULL = same phase) */
    int sync_before;
    uint64_t offset; /* partition-relative */
    const uint8_t* __sized_by(data_len) data;
    size_t data_len;
    uint64_t padded;
} extent;

typedef struct {
    uint64_t dev_sectors;
    uint64_t part_start, part_sectors; /* sectors */
    const layout_row* row;
    int use_gpt;
    uint8_t mbr_type;
    extent ext[16];
    int next;
} plan;

void add_extent(plan* p, const char* phase, int sync, uint64_t off,
                const void* __sized_by(len) data, size_t len, uint64_t padded);
void execute(plan* p);
void verify_region(uint64_t off, const uint8_t* __counted_by(dlen) data,
                   size_t dlen, uint64_t len);

/* ------------------------------------------------------------------------ */
/* File systems (fat.c, exfat.c)                                             */

/* FAT layout, computed and checked before anything is opened for writing */
typedef struct {
    const layout_row* row;
    uint8_t type;
    uint64_t pstart, psize, rsv;
    uint32_t root_entries, root_sec, clusters, fatsz;
} fat_geom;

void plan_fat(fat_geom* g);
void format_fat(const fat_geom* g);

/* exFAT layout, computed and checked before anything is opened for writing */
typedef struct {
    const layout_row* row;
    uint64_t pstart, psize, fat_off, fat_len, heap_off, cc;
} exfat_geom;

void plan_exfat(exfat_geom* g);
void format_exfat(const exfat_geom* g);

#endif
