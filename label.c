/* Volume label handling: validation, code pages, display. */
#include "flashfmt.h"

#include <iconv.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

label_info lbl;

/* Up-case table: FAT labels are upper-cased with the same (compressed)
 * table it writes to exFAT volumes. */
static uint16_t* __counted_by(65536) upcase_map(void) {
    static uint16_t* __counted_by_or_null(65536) m;
    if (m) return m;
    m = xcalloc(65536 * sizeof *m);
    for (uint32_t i = 0; i < 65536; i++) m[i] = (uint16_t)i;
    size_t nw = sizeof upcase_table / 2;
    uint32_t c = 0;
    for (size_t i = 0; i < nw && c < 65536;) {
        uint16_t w = upcase_table[2 * i] | upcase_table[2 * i + 1] << 8;
        if (w == 0xFFFF && i + 1 < nw) {
            c += upcase_table[2 * i + 2] | upcase_table[2 * i + 3] << 8;
            i += 2;
        } else {
            m[c++] = w;
            i++;
        }
    }
    return m;
}

static const sb_codepage* find_sb_codepage(unsigned long cp) {
    for (size_t i = 0; i < sizeof sb_codepages / sizeof sb_codepages[0]; i++)
        if (sb_codepages[i].cp == cp) return &sb_codepages[i];
    return NULL;
}
static int is_dbcs(unsigned long cp) {
    return cp == 932 || cp == 936 || cp == 949 || cp == 950;
}
static int dbcs_lead(unsigned long cp, uint8_t b) {
    if (cp == 932) return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
    return b >= 0x81 && b <= 0xFE;
}

/* Encode one UTF-16 unit; returns bytes written (1-2) or 0 if unmappable. */
static int cp_encode_char(unsigned long cp, uint16_t c,
                          uint8_t* __counted_by(2) out) {
    if (c < 0x20 || c == 0x7f || (c >= 0xD800 && c < 0xE000)) return 0;
    const sb_codepage* sb = find_sb_codepage(cp);
    if (sb) {
        size_t lo = 0, hi = sb->n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (sb->enc[mid].u == c) {
                out[0] = sb->enc[mid].b;
                return 1;
            }
            if (sb->enc[mid].u < c)
                lo = mid + 1;
            else
                hi = mid;
        }
        return 0;
    }
    if (c < 0x80) {
        out[0] = (uint8_t)c;
        return 1;
    }
    char name[16];
    snprintf(name, sizeof name, "CP%lu", cp);
    __auto_type cd = iconv_open(name, "UTF-16LE");
    if (cd == (iconv_t)-1) return 0;
    char in[2] = {(char)(c & 0xff), (char)(c >> 8)}, ob[8];
    char* __unsafe_indexable ip = (char* __unsafe_indexable)in;
    char* __unsafe_indexable op = (char* __unsafe_indexable)ob;
    size_t il = 2, ol = sizeof ob;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    size_t n = sizeof ob - ol;
    if (r == (size_t)-1 || r > 0 || n == 0 || n > 2)
        return 0; /* r > 0: lossy substitution */
    memcpy(out, ob, n);
    return (int)n;
}

/* append ch to out[k] as UTF-8, returns new k; stops when out is nearly full */
static size_t put_utf8(char* __counted_by(n) out, size_t n, size_t k,
                       uint32_t ch) {
    if (k + 4 >= n) return k;
    if (ch < 0x80) {
        out[k++] = (char)ch;
    } else if (ch < 0x800) {
        out[k++] = (char)(0xc0 | (ch >> 6));
        out[k++] = 0x80 | (ch & 0x3f);
    } else if (ch < 0x10000) {
        out[k++] = (char)(0xe0 | (ch >> 12));
        out[k++] = 0x80 | ((ch >> 6) & 0x3f);
        out[k++] = 0x80 | (ch & 0x3f);
    } else {
        out[k++] = (char)(0xf0 | (ch >> 18));
        out[k++] = 0x80 | ((ch >> 12) & 0x3f);
        out[k++] = 0x80 | ((ch >> 6) & 0x3f);
        out[k++] = 0x80 | (ch & 0x3f);
    }
    return k;
}

/* code-page bytes -> UTF-8 (for display) */
static void cp_decode(unsigned long cp, const uint8_t* __counted_by(len) b,
                      int len, char* __counted_by(outsz) out, size_t outsz) {
    size_t k = 0;
    const sb_codepage* sb = find_sb_codepage(cp);
    if (!sb) {
        char name[16];
        snprintf(name, sizeof name, "CP%lu", cp);
        __auto_type cd = iconv_open("UTF-8", name);
        if (cd != (iconv_t)-1) {
            char* __unsafe_indexable ip = (char* __unsafe_indexable)b;
            char* __unsafe_indexable op = (char* __unsafe_indexable)out;
            size_t il = (size_t)len, ol = outsz - 1;
            iconv(cd, &ip, &il, &op, &ol);
            iconv_close(cd);
            out[outsz - 1 - ol] = 0;
            return;
        }
    }
    for (int i = 0; i < len; i++) {
        uint32_t ch = sb ? sb->dec[b[i]] : b[i];
        k = put_utf8(out, outsz, k, ch ? ch : '?');
    }
    out[k] = 0;
}

/* UTF-8 -> UTF-16 code units. Returns the full length (only the first max are
 * stored), -1 on bad UTF-8. */
static int utf8_to_utf16(const char* s, uint16_t* __counted_by(max) out,
                         int max) {
    const char* __null_terminated p = s;
    int n = 0;
    while (*p) {
        uint32_t c;
        int extra;
        static const uint32_t min_cp[4] = {0, 0x80, 0x800, 0x10000};
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x80) {
            c = ch;
            extra = 0;
        } else if ((ch & 0xe0) == 0xc0) {
            c = ch & 0x1f;
            extra = 1;
        } else if ((ch & 0xf0) == 0xe0) {
            c = ch & 0x0f;
            extra = 2;
        } else if ((ch & 0xf8) == 0xf0) {
            c = ch & 0x07;
            extra = 3;
        } else
            return -1;
        int len = extra;
        p++;
        while (extra--) {
            ch = (unsigned char)*p;
            if ((ch & 0xc0) != 0x80) return -1;
            c = (c << 6) | (ch & 0x3f);
            p++;
        }
        /* overlong forms, UTF-16 surrogates and values past U+10FFFF aren't
         * characters */
        if (c < min_cp[len] || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF)
            return -1;
        if (c >= 0x10000) {
            c -= 0x10000;
            if (n + 2 <= max) {
                out[n] = 0xd800 | (c >> 10);
                out[n + 1] = 0xdc00 | (c & 0x3ff);
            }
            n += 2;
        } else {
            if (n + 1 <= max) out[n] = (uint16_t)c;
            n++;
        }
    }
    return n;
}

/* Unicode normalisation of the label (CFStringNormalize on macOS):
 * NFC before encoding, NFD for display. Returns a malloc'd string. */
static char* __null_terminated normalize_label(const char* s, int nfd) {
#if defined(__APPLE__)
    __auto_type cs =
        CFStringCreateWithBytes(NULL, (const UInt8*)s, (CFIndex)strlen(s),
                                kCFStringEncodingUTF8, false);
    if (cs) {
        __auto_type m = CFStringCreateMutableCopy(NULL, 0, cs);
        CFRelease(cs);
        if (m) {
            CFStringNormalize(m, nfd ? kCFStringNormalizationFormD
                                     : kCFStringNormalizationFormC);
            CFIndex max = CFStringGetMaximumSizeForEncoding(
                              CFStringGetLength(m), kCFStringEncodingUTF8) +
                          1;
            char* out = xcalloc((size_t)max);
            Boolean ok = CFStringGetCString(m, out, max, kCFStringEncodingUTF8);
            CFRelease(m);
            if (ok) return __unsafe_null_terminated_from_indexable(out);
            free(out);
        }
    }
#else
    (void)nfd;
#endif
    return strdup(s);
}

/* opt.label -> NFC UTF-16, max 11 units */
static int label_utf16(uint16_t* __counted_by(11) u) {
    char* __null_terminated nfc = normalize_label(opt.label, 0);
    int n = utf8_to_utf16(nfc, u, 11);
    free(nfc);
    if (n < 0) die("Failed to encode volume label into UTF-16.");
    if (n > 11)
        die("Invalid volume label length. (Was: %d Maximum length allowed: "
            "11)",
            n);
    return n;
}

/* FAT: code page check, then label -> UTF-16 -> up-case -> code page ->
 * validity */
void prepare_fat_label(void) {
    if (opt.codepage == 0)
        opt.codepage = 437;
    else if (!find_sb_codepage(opt.codepage) && !is_dbcs(opt.codepage))
        die("Unsupported code page %lu.", opt.codepage);
    if (!opt.label) return;
    uint16_t u[11];
    int n = label_utf16(u);
    uint16_t* up = upcase_map();
    int len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b[2];
        int k = cp_encode_char(opt.codepage, up[u[i]], b);
        if (!k || len + k > 11)
            die("Non-CP%lu characters in volume label.", opt.codepage);
        memcpy(lbl.fat + len, b, (size_t)k);
        len += k;
    }
    int ok = len > 0 && lbl.fat[len - 1] != ' ';
    for (int i = 0; ok && i < len; i++) {
        if (is_dbcs(opt.codepage) && dbcs_lead(opt.codepage, lbl.fat[i]) &&
            i + 1 < len) {
            i++;
            continue;
        }
        if (strchr("\"*+,./:;<=>?[\\]|", lbl.fat[i])) ok = 0;
    }
    if (!ok) die("Invalid characters in volume label.");
    lbl.fat_len = len;
    lbl.set = 1;
}

/* exFAT: label -> UTF-16 (max 11 units) -> validity */
void prepare_exfat_label(void) {
    if (!opt.label) return;
    uint16_t u[11];
    int n = label_utf16(u);
    int ok = n > 0;
    /* C0/C1 controls, DEL, U+FFFE/U+FFFF, reserved punctuation */
    for (int i = 0; ok && i < n; i++)
        if (u[i] < 0x20 || (u[i] >= 0x7f && u[i] < 0xa0) || u[i] >= 0xfffe ||
            (u[i] < 0x80 && strchr("\"*/:<>?\\|", u[i])))
            ok = 0;
    if (!ok) die("Invalid characters in volume label.");
    memcpy(lbl.ex, u, sizeof(uint16_t) * (size_t)n);
    lbl.ex_len = n;
    lbl.set = 1;
}

/* Label for display: decoded from the stored form, NFD. */
char* __null_terminated label_display(int exfat) {
    char u8[128];
    if (!exfat) {
        cp_decode(opt.codepage, lbl.fat, lbl.fat_len, u8, sizeof u8);
    } else {
        size_t k = 0;
        for (int i = 0; i < lbl.ex_len; i++) { /* UTF-16 -> UTF-8 */
            uint32_t ch = lbl.ex[i];
            if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < lbl.ex_len)
                ch = 0x10000 + ((ch - 0xD800) << 10) + (lbl.ex[++i] - 0xDC00);
            k = put_utf8(u8, sizeof u8, k, ch);
        }
        u8[k] = 0;
    }
    return normalize_label(__unsafe_null_terminated_from_indexable(u8), 1);
}
