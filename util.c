/* Logging, errors and small helpers. */
#include "flashfmt.h"

/* --debug: layout/diagnostic details */
void dbg(const char* fmt, ...) {
    if (!opt.debug) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("  ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
/* -v: informational messages */
void vinfo(const char* fmt, ...) {
    if (!opt.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}
void info(const char* fmt, ...) {
    if (opt.quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
int formatting; /* set once the format itself has started */

/* move off the progress bar line */
static void clear_bar(void) {
    if (opt.progress_bar == 1 && formatting) {
        fflush(stdout);
        fputs("\r\033[K", stderr);
    }
}
void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    clear_bar();
    fputs("Warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    clear_bar();
    fputs("Error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (formatting) fputs("Formatting failed!\n", stderr);
    exit(1);
}

void* __sized_by(n) xcalloc(size_t n) {
    void* p = calloc(1, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void random_bytes(void* __sized_by(n) buf, size_t n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, n, f) != n) die("cannot read /dev/urandom");
    fclose(f);
}
uint32_t random_u32(void) {
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}
void random_guid(uint8_t* __counted_by(16) g) {
    random_bytes(g, 16);
    g[7] = (g[7] & 0x0f) | 0x40; /* version 4 (stored little-endian field) */
    g[8] = (g[8] & 0x3f) | 0x80; /* RFC 4122 variant */
}

uint32_t crc32_buf(const uint8_t* __counted_by(n) p, size_t n) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
