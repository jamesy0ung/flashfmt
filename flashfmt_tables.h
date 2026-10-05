/* Data tables (tables.c): boot sector, FSInfo and GPT templates, the exFAT
 * up-case table and OEM code page maps. */
#ifndef FLASHFMT_TABLES_H
#define FLASHFMT_TABLES_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint16_t u; uint8_t b; } cp_pair;
typedef struct {
  unsigned cp;
  const cp_pair *__counted_by(n) enc;
  size_t n;
  const uint16_t *__counted_by(256) dec;
} sb_codepage;

extern const uint8_t upcase_table[5836];
extern const uint8_t exfat_boot_template[512];
extern const uint8_t fat1216_boot_template[512];
extern const uint8_t fat32_boot_template[512];
extern const uint8_t fsinfo_template[512];
extern const uint8_t gpt_header_template[512];
extern const uint8_t gpt_entry_template[128];
extern const sb_codepage sb_codepages[16];

#endif
