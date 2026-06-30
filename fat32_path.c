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

typedef struct {
    const char      *target;
    uint32_t         target_len;
    fat32_dirent_t   found;
    uint32_t         found_cluster;
    uint32_t         found_offset;
    bool             found_flag;

    uint16_t         lfn_buf[256];
    uint32_t         lfn_len;
    bool             lfn_pending;
} lookup_ctx_t;

static void lfn_extract_chars(const fat32_lfn_t *lfn, uint16_t *out) {
    for (int i = 0; i < 5; i++) out[i] = lfn->name1[i];
    for (int i = 0; i < 6; i++) out[5 + i] = lfn->name2[i];
    for (int i = 0; i < 2; i++) out[11 + i] = lfn->name3[i];
}

static int lookup_visitor(dirent_visit_ctx_t *ctx, void *user) {
    lookup_ctx_t *lk = (lookup_ctx_t*)user;
    fat32_dirent_t *de = ctx->raw;

    if (de->name[0] == FAT_DIRENT_END) return 0;
    if (de->name[0] == FAT_DIRENT_FREE) { lk->lfn_pending = false; return 0; }

    if (de->attr == FAT_ATTR_LFN) {
        fat32_lfn_t *lfn = (fat32_lfn_t*)de;
        uint16_t chunk[13];
        lfn_extract_chars(lfn, chunk);

        uint8_t seq = lfn->order & 0x1F;
        if (seq == 0) return 0;
        uint32_t base = (seq - 1) * 13;

        if (lfn->order & 0x40) {
            lk->lfn_len = base + 13;
            if (lk->lfn_len > 255) lk->lfn_len = 255;
            lk->lfn_pending = true;
        }
        if (lk->lfn_pending) {
            for (int i = 0; i < 13 && (base + i) < 255; i++) {
                lk->lfn_buf[base + i] = chunk[i];
            }
        }
        return 0;
    }

    bool matched = false;

    if (lk->lfn_pending) {
        uint32_t real_len = 0;
        for (uint32_t i = 0; i < lk->lfn_len; i++) {
            if (lk->lfn_buf[i] == 0x0000) break;
            real_len++;
        }
        if (lfn_chars_match_ascii(lk->lfn_buf, real_len, lk->target, lk->target_len, 0) &&
            real_len == lk->target_len) {
            matched = true;
        }
    }
    lk->lfn_pending = false;

    if (!matched && !(de->attr & FAT_ATTR_VOLUME_ID)) {
        matched = short_name_matches(de->name, lk->target);
    }

    if (matched) {
        lk->found = *de;
        lk->found_cluster = ctx->cluster;
        lk->found_offset = ctx->sector_in_cluster * 512 + ctx->index_in_sector * sizeof(fat32_dirent_t);
        lk->found_flag = true;
        return 1;
    }

    return 0;
}

static fat_status_t dir_lookup(fat32_volume_t *vol, uint32_t dir_cluster,
                                const char *component, uint32_t component_len,
                                fat32_dirent_t *out, uint32_t *out_cluster, uint32_t *out_offset) {
    lookup_ctx_t lk;
    fat_memset(&lk, 0, sizeof(lk));
    lk.target = component;
    lk.target_len = component_len;

    fat_status_t st = dir_walk(vol, dir_cluster, lookup_visitor, &lk);
    if (st != FAT_OK && st != FAT_ERR_NOT_FOUND) return st;

    if (!lk.found_flag) return FAT_ERR_NOT_FOUND;

    *out = lk.found;
    if (out_cluster) *out_cluster = lk.found_cluster;
    if (out_offset) *out_offset = lk.found_offset;
    return FAT_OK;
}

static uint32_t dirent_first_cluster(const fat32_dirent_t *de) {
    return ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
}

static void dirent_set_first_cluster(fat32_dirent_t *de, uint32_t cluster) {
    de->first_cluster_hi = (uint16_t)(cluster >> 16);
    de->first_cluster_lo = (uint16_t)(cluster & 0xFFFF);
}

static fat_status_t resolve_path(fat32_volume_t *vol, const char *path,
                                  fat32_dirent_t *out_entry,
                                  uint32_t *out_parent_cluster,
                                  uint32_t *out_entry_cluster,
                                  uint32_t *out_entry_offset) {
    if (!path || path[0] != '/') return FAT_ERR_INVALID_ARG;

    uint32_t cur_dir_cluster = vol->root_cluster;
    uint32_t parent_cluster = vol->root_cluster;

    const char *p = path + 1;

    if (*p == '\0') {
        fat_memset(out_entry, 0, sizeof(*out_entry));
        out_entry->attr = FAT_ATTR_DIRECTORY;
        dirent_set_first_cluster(out_entry, vol->root_cluster);
        if (out_parent_cluster) *out_parent_cluster = vol->root_cluster;
        if (out_entry_cluster) *out_entry_cluster = 0;
        if (out_entry_offset) *out_entry_offset = 0;
        return FAT_OK;
    }

    while (true) {
        const char *seg_start = p;
        while (*p && *p != '/') p++;
        uint32_t seg_len = (uint32_t)(p - seg_start);
        if (seg_len == 0) return FAT_ERR_INVALID_NAME;
        if (seg_len > 255) return FAT_ERR_INVALID_NAME;

        bool is_last = (*p == '\0');

        fat32_dirent_t entry;
        uint32_t entry_cluster, entry_offset;
        fat_status_t st = dir_lookup(vol, cur_dir_cluster, seg_start, seg_len,
                                      &entry, &entry_cluster, &entry_offset);
        if (st != FAT_OK) return st;

        if (is_last) {
            *out_entry = entry;
            if (out_parent_cluster) *out_parent_cluster = cur_dir_cluster;
            if (out_entry_cluster) *out_entry_cluster = entry_cluster;
            if (out_entry_offset) *out_entry_offset = entry_offset;
            return FAT_OK;
        }

        if (!(entry.attr & FAT_ATTR_DIRECTORY)) return FAT_ERR_NOT_DIR;

        parent_cluster = cur_dir_cluster;
        cur_dir_cluster = dirent_first_cluster(&entry);
        if (cur_dir_cluster == 0) cur_dir_cluster = vol->root_cluster;

        p++;
        if (*p == '\0') return FAT_ERR_NOT_FOUND;
    }

    (void)parent_cluster;
}

static fat_status_t resolve_parent_and_name(fat32_volume_t *vol, const char *path,
                                             uint32_t *out_parent_cluster,
                                             const char **out_name, uint32_t *out_name_len) {
    if (!path || path[0] != '/') return FAT_ERR_INVALID_ARG;
    uint32_t len = fat_strlen(path);
    if (len <= 1) return FAT_ERR_INVALID_NAME;

    int32_t last_slash = -1;
    for (int32_t i = (int32_t)len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    *out_name = path + last_slash + 1;
    *out_name_len = len - (last_slash + 1);
    if (*out_name_len == 0) return FAT_ERR_INVALID_NAME;

    if (last_slash == 0) {
        *out_parent_cluster = vol->root_cluster;
        return FAT_OK;
    }

    char parent_path[512];
    if ((uint32_t)last_slash >= sizeof(parent_path)) return FAT_ERR_INVALID_NAME;
    fat_memcpy(parent_path, path, last_slash);
    parent_path[last_slash] = '\0';

    fat32_dirent_t parent_entry;
    fat_status_t st = resolve_path(vol, parent_path, &parent_entry, NULL, NULL, NULL);
    if (st != FAT_OK) return st;
    if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) return FAT_ERR_NOT_DIR;

    *out_parent_cluster = dirent_first_cluster(&parent_entry);
    if (*out_parent_cluster == 0) *out_parent_cluster = vol->root_cluster;
    return FAT_OK;
}