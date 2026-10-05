/* Device layer: opening, safety checks and raw I/O. */
#include "flashfmt.h"

#include <ctype.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>
#ifndef DKIOCGETBASE
#define DKIOCGETBASE _IOR('d', 73, uint64_t)
#endif
#elif defined(__linux__)
#include <linux/fs.h>
#include <mntent.h>
#include <sys/sysmacros.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#endif

device dev = {.fd = -1, .lock_fd = -1, .verify_fd = -1};

void dev_pwrite(uint64_t off, const void* __sized_by(len) buf,
                size_t len) {
    const uint8_t* p = buf;
    size_t remaining = len;
    off += dev.window;
    while (remaining) {
        ssize_t w = pwrite(dev.fd, p, remaining, (off_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            die("write of %zu bytes at offset %" PRIu64 " failed: %s",
                remaining, off, strerror(errno));
        }
        if (w == 0) die("short write at offset %" PRIu64, off);
        p += w;
        off += w;
        remaining -= w;
    }
}

/* Returns 0, or -1 with the unread rest of buf zeroed. */
int dev_pread_abs(int fd, uint64_t off, void* __sized_by(len) buf,
                  size_t len) {
    uint8_t* p = buf;
    size_t remaining = len;
    while (remaining) {
        ssize_t r = pread(fd, p, remaining, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            if (r == 0) errno = EIO;
            memset(p, 0, remaining);
            return -1;
        }
        p += r;
        off += r;
        remaining -= r;
    }
    return 0;
}

void dev_sync(void) {
#ifdef F_FULLFSYNC
    if (fcntl(dev.fd, F_FULLFSYNC, 0) == 0) return;
#endif
    if (fsync(dev.fd) != 0 && errno != EINVAL && errno != ENOTSUP)
        die("sync failed: %s", strerror(errno));
}

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Write `data` at `pos` followed by
 * `pad` bytes up to `padded`. Whole MiB of data are written in one call; the
 * remaining data tail and the padding go out in 1 MiB chunks; optional
 * periodic sync during long fills. */
void write_with_padding(uint64_t pos,
                        const void* __sized_by(dlen) data, size_t dlen,
                        uint64_t padded, uint8_t pad,
                        int sync_interval) {
    static uint8_t* __counted_by_or_null(MIB) chunk;
    if (!chunk) chunk = xcalloc(MIB);
    if (dlen > padded) die("internal: data larger than padded region");
    progress(0);
    size_t tail = padded > dlen ? dlen % MIB : 0;
    size_t direct = dlen - tail;
    if (direct) {
        if (!opt.dry_run) dev_pwrite(pos, data, direct);
        progress(direct);
    }
    if (padded == dlen) return;
    if (tail) memcpy(chunk, (const uint8_t*)data + direct, tail);
    memset(chunk + tail, pad, MIB - tail);
    double next_sync = sync_interval ? now_s() + sync_interval : 0;
    uint64_t done = 0, rest = padded - direct;
    while (rest > done) {
        size_t n = rest - done > MIB ? MIB : (size_t)(rest - done);
        if (!opt.dry_run) dev_pwrite(pos + direct + done, chunk, n);
        if (tail) {
            memset(chunk, pad, tail);
            tail = 0;
        }
        done += n;
        if (sync_interval && !opt.dry_run && now_s() >= next_sync) {
            dev_sync();
            next_sync = now_s() + sync_interval;
        }
        progress(n);
    }
}

/* Whole-device discard (DKIOCUNMAP on macOS, BLKDISCARD on Linux). */
int dev_discard(uint64_t off, uint64_t len, int secure) {
    if (dev.is_file) return ENOTSUP;
#if defined(__APPLE__)
    if (secure) return ENOTSUP; /* not supported on macOS */
    dk_extent_t ext = {.offset = off, .length = len};
    dk_unmap_t um = {0};
    um.extents = &ext;
    um.extentsCount = 1;
    dbg("Issuing DKIOCUNMAP for 1 extent: offset=%" PRIu64 " length=%" PRIu64,
        off, len);
    return ioctl(dev.fd, DKIOCUNMAP, &um) == -1 ? errno : 0;
#elif defined(__linux__)
    uint64_t range[2] = {off, len};
    return ioctl(dev.fd, secure ? BLKSECDISCARD : BLKDISCARD, range) == -1
               ? errno
               : 0;
#else
    (void)off;
    (void)len;
    (void)secure;
    return ENOTSUP;
#endif
}

void dev_refresh_partitions(void) {
#if defined(__linux__)
    if (!dev.is_file) ioctl(dev.fd, BLKRRPART);
#endif
}

/* ------------------------------------------------------------------------ */
/* Device opening and safety checks                                          */

#if defined(__APPLE__)
/* 1 if the disk is internal with non-removable media (a built-in SSD/HDD),
 * 0 if it's external or has removable media (e.g. the built-in SD reader),
 * -1 if DiskArbitration can't tell. */
static int internal_fixed_disk(const char* bsd) {
    int r = -1;
    __auto_type s = DASessionCreate(kCFAllocatorDefault);
    if (!s) return -1;
    __auto_type d = DADiskCreateFromBSDName(kCFAllocatorDefault, s, bsd);
    if (d) {
        __auto_type desc = DADiskCopyDescription(d);
        if (desc) {
            __auto_type internal =
                CFDictionaryGetValue(desc, kDADiskDescriptionDeviceInternalKey);
            __auto_type removable =
                CFDictionaryGetValue(desc, kDADiskDescriptionMediaRemovableKey);
            if (internal && removable &&
                CFGetTypeID(internal) == CFBooleanGetTypeID() &&
                CFGetTypeID(removable) == CFBooleanGetTypeID())
                r = CFBooleanGetValue(internal) &&
                    !CFBooleanGetValue(removable);
            CFRelease(desc);
        }
        CFRelease(d);
    }
    CFRelease(s);
    return r;
}

struct unmount_result {
    int done;
    DAReturn status;
    char reason[256];
};

static void unmount_done(DADiskRef d, DADissenterRef dis, void* ctx) {
    (void)d;
    struct unmount_result* __single r = ctx;
    r->done = 1;
    if (!dis) return;
    r->status = DADissenterGetStatus(dis);
    CFStringRef __single s = __unsafe_forge_single(CFStringRef,
                                                   DADissenterGetStatusString(dis));
    if (!s || !CFStringGetCString(s, r->reason, sizeof r->reason,
                                  kCFStringEncodingUTF8))
        r->reason[0] = 0;
}

/* Unmounts every volume on the whole disk through DiskArbitration, so the
 * read-write open isn't refused with EBUSY. A no-op if nothing is mounted. */
static void unmount_disk(const char* bsd) {
    __auto_type s = DASessionCreate(kCFAllocatorDefault);
    if (!s) die("cannot create a DiskArbitration session");
    __auto_type d = DADiskCreateFromBSDName(kCFAllocatorDefault, s, bsd);
    if (!d) die("cannot find /dev/%s in DiskArbitration", bsd);
    struct unmount_result r = {0};
    DASessionScheduleWithRunLoop(s, CFRunLoopGetCurrent(),
                                 kCFRunLoopDefaultMode);
    DADiskUnmount(d, kDADiskUnmountOptionWhole, unmount_done, &r);
    for (int i = 0; !r.done && i < 60; i++)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1, true);
    DASessionUnscheduleFromRunLoop(s, CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);
    CFRelease(d);
    CFRelease(s);
    if (!r.done) die("timed out unmounting /dev/%s", bsd);
    if (r.status)
        die("cannot unmount /dev/%s: %s (DiskArbitration status 0x%x)", bsd,
            r.reason[0] ? r.reason : "volume in use", (unsigned)r.status);
    vinfo("Unmounted /dev/%s", bsd);
}
#endif

#if defined(__linux__)
static int is_prefix_device(const char* mnt_from, const char* devnode) {
    size_t l = strlen(devnode);
    if (strncmp(mnt_from, devnode, l)) return 0;
    char c = mnt_from[l];
    return c == 0 || c == 's' || c == 'p' || isdigit((unsigned char)c);
}
#endif

/* Copies a path, refusing to truncate it (a truncated path names a different
 * file). */
static void set_path(char* __counted_by(n) dst, size_t n, const char* src) {
    if ((size_t)snprintf(dst, n, "%s", src) >= n)
        die("path too long: '%s'", src);
}

/* The target as it was checked: every later open by name must resolve to the
 * same object, so swapping the path (e.g. for a symlink) after the checks
 * can't redirect the format. */
static struct {
    mode_t type;
    dev_t dev, rdev;
    ino_t ino;
    off_t size;
} target;

static void record_target(const struct stat* st) {
    target.type = st->st_mode & S_IFMT;
    target.dev = st->st_dev;
    target.rdev = st->st_rdev;
    target.ino = st->st_ino;
    target.size = st->st_size;
}

/* Size of a block device in 512-byte sectors; 0 if it can't be read or the
 * sector size isn't 512. */
static uint64_t device_sectors(int fd) {
#if defined(__APPLE__)
    uint32_t bs = 0;
    uint64_t bc = 0;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) != 0 ||
        ioctl(fd, DKIOCGETBLOCKCOUNT, &bc) != 0)
        return 0;
    return bs == SECTOR ? bc : 0;
#elif defined(__linux__)
    uint64_t bytes = 0;
    int lbs = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0 || ioctl(fd, BLKSSZGET, &lbs) != 0)
        return 0;
    return lbs == (int)SECTOR ? bytes / SECTOR : 0;
#else
    (void)fd;
    return 0;
#endif
}

static int is_target(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0 || (st.st_mode & S_IFMT) != target.type) return 0;
    if (target.type == S_IFREG)
        return st.st_dev == target.dev && st.st_ino == target.ino &&
               st.st_size == target.size;
    return st.st_rdev == target.rdev && device_sectors(fd) == dev.sectors;
}


void open_device(void) {
    const char* __null_terminated path = opt.device;
    char buf[1024];
    struct stat st;

    /* All checks below run on this descriptor (O_NONBLOCK: don't hang on a
     * FIFO). */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT && opt.create_size) {
        /* created by open_rw() */
        dev.is_file = dev.create = 1;
        set_path(dev.path, sizeof dev.path, path);
        dev.sectors = opt.create_size / SECTOR;
        return;
    }
    if (fd < 0 && errno == ENOENT && strncmp(path, "/dev/", 5)) {
        set_path(buf, sizeof buf, "/dev/");
        set_path(buf + 5, sizeof buf - 5, path);
        path = __unsafe_null_terminated_from_indexable(buf);
        fd = open(path, O_RDONLY | O_NONBLOCK);
    }
    if (fd < 0) die("cannot open '%s': %s", opt.device, strerror(errno));
    if (fstat(fd, &st) != 0) die("cannot stat '%s': %s", path, strerror(errno));
    record_target(&st);
    dev.is_file = S_ISREG(st.st_mode);
    set_path(dev.path, sizeof dev.path, path);

    if (dev.is_file) {
        close(fd);
        if (st.st_size % SECTOR)
            die("image size is not a multiple of 512 bytes");
        dev.sectors = (uint64_t)st.st_size / SECTOR;
        return;
    }
    if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode))
        die("'%s' is neither a block device nor a regular file", path);

#if defined(__APPLE__)
    /* The disk is identified by the device number of what was opened, not by
     * the spelling of the path (a symlink named "disk9" may point anywhere). */
    char bsd[64], node[128], rnode[128];
    if (!devname_r(st.st_rdev, st.st_mode & S_IFMT, bsd, sizeof bsd))
        die("cannot find the device node for '%s'", path);
    close(fd);
    size_t bsd_len = strnlen(bsd, sizeof bsd);
    if (bsd_len == sizeof bsd) die("device name is too long");
    if (bsd[0] == 'r') {
        memmove(bsd, bsd + 1, bsd_len);
        bsd_len--;
    }
    if (strncmp(bsd, "disk", 4) || !isdigit((unsigned char)bsd[4]) ||
        memchr(bsd + 4, 's', bsd_len > 4 ? bsd_len - 4 : 0))
        die("Device '%s' seems to be a partition. This tool should only be run "
            "on top-level block devices.",
            path);
    snprintf(node, sizeof node, "/dev/%s", bsd);
    snprintf(rnode, sizeof rnode, "/dev/r%s", bsd);

    /* No mount check here: open_rw() unmounts the disk through
     * DiskArbitration once the format has been confirmed. */
    if (!opt.allow_internal) {
        int r = internal_fixed_disk(
            __unsafe_null_terminated_from_indexable(bsd));
        if (r < 0)
            die("cannot determine whether %s is an internal disk; refusing "
                "(use --allow-internal to override)",
                node);
        if (r > 0)
            die("%s is an internal, non-removable disk; refusing (use "
                "--allow-internal to override)",
                node);
    }

    /* The block node is what gets written; it's the target from here on. */
    uint64_t base = 0;
    int pb = open(node, O_RDONLY);
    if (pb < 0) die("cannot open %s: %s", node, strerror(errno));
    if (fstat(pb, &st) != 0 || !S_ISBLK(st.st_mode))
        die("%s is not a block device", node);
    record_target(&st);
    if (ioctl(pb, DKIOCGETBASE, &base) == 0 && base != 0)
        die("Device '%s' seems to be a partition. This tool should only be run "
            "on top-level block devices.",
            node);
    dev.sectors = device_sectors(pb);
    if (!dev.sectors)
        die("cannot query size of %s (only 512-byte sector devices are "
            "supported)",
            node);
    close(pb);

    snprintf(dev.path, sizeof dev.path, "%s", node);
    snprintf(dev.rnode, sizeof dev.rnode, "%s", rnode);
#elif defined(__linux__)
    if (!S_ISBLK(st.st_mode)) die("'%s' is not a block device", path);
    snprintf(buf, sizeof buf, "/sys/dev/block/%u:%u/partition",
             major(st.st_rdev), minor(st.st_rdev));
    if (access(buf, F_OK) == 0)
        die("Device '%s' seems to be a partition. This tool should only be run "
            "on top-level block devices.",
            path);
    /* From here on, use the kernel's name for what was opened, not the given
     * path. */
    char real[PATH_MAX], link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t rl = readlink(link, real, sizeof real - 1);
    if (rl <= 0 || (size_t)rl >= sizeof real - 1)
        die("cannot resolve the device node for '%s'", path);
    real[rl] = 0;
    set_path(dev.path, sizeof dev.path, real);
    FILE* mf = setmntent("/proc/mounts", "r");
    struct mntent* me;
    while (mf && (me = getmntent(mf)))
        if (is_prefix_device(me->mnt_fsname, real)) {
            if (!strcmp(me->mnt_dir, "/"))
                die("%s holds the root file system", path);
            if (!opt.force)
                die("%s is mounted on %s. Unmount it first.", me->mnt_fsname,
                    me->mnt_dir);
            fprintf(stderr,
                    "Warning: %s is mounted; continuing due to --force.\n",
                    path);
        }
    if (mf) endmntent(mf);
    dev.sectors = device_sectors(fd);
    if (!dev.sectors)
        die("cannot query size of %s (only 512-byte sector devices are "
            "supported)",
            path);
    close(fd);
#else
    die("block devices are not supported on this platform; use an image file");
#endif
}

/* The read-write open, done after the verbose header. A dry run never opens
 * the target for writing. */
void open_rw(void) {
    if (opt.dry_run) return;
    int flags = O_RDWR;
    if (dev.create) {
        /* O_CREAT|O_EXCL never follows a symlink; this descriptor is used
         * directly. */
        dev.fd = open(dev.path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (dev.fd < 0 || ftruncate(dev.fd, (off_t)opt.create_size) != 0)
            die("cannot create image '%s': %s", dev.path, strerror(errno));
    } else {
#if defined(__linux__)
        if (!dev.is_file && !opt.force) flags |= O_EXCL;
#endif
#if defined(__APPLE__)
        if (!dev.is_file)
            unmount_disk(__unsafe_null_terminated_from_indexable(
                dev.path + 5)); /* skip "/dev/" */
#endif
        dev.fd = open(dev.path, flags);
        if (dev.fd < 0)
            die("cannot open '%s' for writing: %s", dev.path, strerror(errno));
        if (!is_target(dev.fd))
            die("'%s' changed after it was checked; refusing to format it.",
                opt.device);
    }
#if defined(__APPLE__)
    if (!dev.is_file) {
        dev.lock_fd = open(dev.rnode, O_RDWR);
        if (flock(dev.fd, LOCK_EX | LOCK_NB) != 0 ||
            (dev.lock_fd >= 0 && flock(dev.lock_fd, LOCK_EX | LOCK_NB) != 0))
            die("cannot lock '%s': %s", dev.path, strerror(errno));
        /* /dev/diskN goes through the buffer cache; read back through the raw
         * node. */
        dev.verify_fd = open(dev.rnode, O_RDONLY);
        if (dev.verify_fd < 0)
            warn("cannot open %s for read-back (%s); the self-check will read "
                 "through the buffer cache and can't detect data that didn't "
                 "reach the device.",
                 dev.rnode, strerror(errno));
        return;
    }
#endif
#if defined(__linux__)
    if (!dev.is_file) {
        /* else: page cache dropped before verifying */
        dev.verify_fd = open(dev.path, O_RDONLY | O_DIRECT);
        if (dev.verify_fd < 0)
            dbg("O_DIRECT open of %s failed: %s", dev.path, strerror(errno));
        else if (!is_target(dev.verify_fd))
            die("'%s' changed after it was checked; refusing to format it.",
                opt.device);
    }
#endif
    if (flock(dev.fd, LOCK_EX | LOCK_NB) != 0)
        die("cannot lock '%s': %s", dev.path, strerror(errno));
}
