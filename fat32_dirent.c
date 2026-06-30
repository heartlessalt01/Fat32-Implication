/*
 * ========================================================================
 * FAT32 Driver Library (Bare-Metal / Kernel Level)
 * ========================================================================
 *
 * Copyright © 2026 Heartless. All rights reserved.
 *
 * This Software is intended for use in bare-metal systems, operating
 * system kernels, bootloaders, and other low-level environments.
 *
 * OWNERSHIP
 * This Software and all associated source code are the exclusive property
 * of the author.
 *
 * PERMITTED USE (PRIVATE USE ONLY)
 * - You may use, copy, and modify this Software for personal or private
 * non-commercial purposes only, including experimental kernel or OS work.
 *
 * RESTRICTIONS
 * - Redistribution, sublicensing, publishing, or sharing of this Software,
 * in whole or in part, is strictly prohibited without explicit written
 * permission from the author.
 *
 * COMMERCIAL / DISTRIBUTION USE
 * - Any use in commercial products, paid software, embedded systems,
 * operating systems, firmware, or hardware distributions requires prior
 * written permission from the author.
 *
 * - Selling, licensing, or otherwise monetizing this Software or derived
 * works is not allowed without explicit approval.
 *
 * PERMISSION CONTACT
 * Email: heartlessalt01@gmail.com
 * heartlesalt01@gmail.com
 *
 * DISCLAIMER
 * This Software is provided "AS IS", without warranty of any kind,
 * express or implied, including but not limited to fitness for a
 * particular purpose or reliability in production systems.
 *
 * ========================================================================
 */

#include "fat32.h"

static bool is_valid_short_char(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
        case '$': case '%': case '\'': case '-': case '_': case '@':
        case '~': case '`': case '!': case '(': case ')': case '{':
        case '}': case '^': case '#': case '&':
            return true;
        default:
            return false;
    }
}

static bool try_make_short_name(const char *name, uint8_t out83[11]) {
    uint32_t len = fat_strlen(name);
    if (len == 0 || len > 12) return false;

    const char *dot = NULL;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '.') {
            if (dot) return false;
            dot = &name[i];
        }
    }

    uint32_t base_len = dot ? (uint32_t)(dot - name) : len;
    uint32_t ext_len   = dot ? (len - base_len - 1) : 0;

    if (base_len == 0 || base_len > 8 || ext_len > 3) return false;

    fat_memset(out83, ' ', 11);

    for (uint32_t i = 0; i < base_len; i++) {
        uint8_t c = fat_to_upper((uint8_t)name[i]);
        if (!is_valid_short_char(c) && c != ' ') return false;
        out83[i] = c;
    }
    if (dot) {
        for (uint32_t i = 0; i < ext_len; i++) {
            uint8_t c = fat_to_upper((uint8_t)dot[1 + i]);
            if (!is_valid_short_char(c) && c != ' ') return false;
            out83[8 + i] = c;
        }
    }

    if (out83[0] == 0xE5) out83[0] = 0x05;

    return true;
}

static bool short_name_matches(const uint8_t disk_name[11], const char *name) {
    uint8_t candidate[11];
    if (!try_make_short_name(name, candidate)) return false;
    return fat_memcmp(disk_name, candidate, 11) == 0;
}

static bool lfn_chars_match_ascii(const uint16_t *ucs2, uint32_t ucs2_count,
                                   const char *ascii, uint32_t ascii_len, uint32_t ascii_off) {
    for (uint32_t i = 0; i < ucs2_count; i++) {
        uint32_t ai = ascii_off + i;
        if (ai >= ascii_len) {
            return (ucs2[i] == 0xFFFF || ucs2[i] == 0x0000);
        }
        uint8_t ac = fat_to_upper((uint8_t)ascii[ai]);
        uint16_t lc = ucs2[i];
        uint8_t lc8 = (lc <= 0xFF) ? fat_to_upper((uint8_t)lc) : 0xFF;
        if (lc8 != ac) return false;
    }
    return true;
}

typedef struct {
    fat32_dirent_t *raw;
    uint32_t         cluster;
    uint32_t         sector_in_cluster;
    uint32_t         index_in_sector;
    uint8_t         *sector_buf;
} dirent_visit_ctx_t;

typedef int (*dirent_visitor_fn)(dirent_visit_ctx_t *ctx, void *user);

static fat_status_t dir_walk(fat32_volume_t *vol, uint32_t dir_first_cluster,
                              dirent_visitor_fn visitor, void *user) {
    if (dir_first_cluster < 2) return FAT_ERR_INVALID_ARG;

    uint32_t cluster = dir_first_cluster;
    uint8_t buf[FAT_MAX_SECTOR_SIZE];

    while (true) {
        uint32_t lba = cluster_to_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            fat_status_t st = disk_read(vol, lba + s, 1, buf);
            if (st != FAT_OK) return st;

            uint32_t entries_per_sector = vol->dev.sector_size / sizeof(fat32_dirent_t);
            for (uint32_t e = 0; e < entries_per_sector; e++) {
                fat32_dirent_t *de = (fat32_dirent_t*)(buf + e * sizeof(fat32_dirent_t));

                dirent_visit_ctx_t ctx;
                ctx.raw = de;
                ctx.cluster = cluster;
                ctx.sector_in_cluster = s;
                ctx.index_in_sector = e;
                ctx.sector_buf = buf;

                int r = visitor(&ctx, user);
                if (r != 0) {
                    if (r < 0) return (fat_status_t)r;
                    return FAT_OK;
                }

                if (de->name[0] == FAT_DIRENT_END) {
                    return FAT_ERR_NOT_FOUND;
                }
            }
        }

        uint32_t next;
        fat_status_t st = fat_entry_read(vol, cluster, &next);
        if (st != FAT_OK) return st;
        if (is_eoc(next)) return FAT_ERR_NOT_FOUND;
        cluster = next;
    }
}
