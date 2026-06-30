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

static uint8_t lfn_checksum(const uint8_t name83[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) | (sum >> 1)) + name83[i];
    }
    return sum;
}

static void make_numeric_tail_short_name(const char *name, uint32_t attempt, uint8_t out83[11]) {
    fat_memset(out83, ' ', 11);

    uint32_t len = fat_strlen(name);
    const char *dot = NULL;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '.') dot = &name[i];
    }
    uint32_t base_len = dot ? (uint32_t)(dot - name) : len;
    uint32_t ext_len   = dot ? (len - (uint32_t)(dot - name) - 1) : 0;

    char tail[8];
    uint32_t tail_len = 0;
    uint32_t a = attempt;
    do { tail[tail_len++] = (char)('0' + (a % 10)); a /= 10; } while (a > 0 && tail_len < 7);

    for (uint32_t i = 0; i < tail_len / 2; i++) {
        char t = tail[i]; tail[i] = tail[tail_len - 1 - i]; tail[tail_len - 1 - i] = t;
    }

    uint32_t copy_base = base_len;
    if (copy_base > 8 - (tail_len + 1)) copy_base = 8 - (tail_len + 1);

    uint32_t pos = 0;
    for (uint32_t i = 0; i < copy_base; i++) {
        uint8_t c = fat_to_upper((uint8_t)name[i]);
        out83[pos++] = is_valid_short_char(c) ? c : '_';
    }
    out83[pos++] = '~';
    for (uint32_t i = 0; i < tail_len; i++) out83[pos++] = tail[i];

    for (uint32_t i = 0; i < ext_len && i < 3; i++) {
        uint8_t c = fat_to_upper((uint8_t)dot[1 + i]);
        out83[8 + i] = is_valid_short_char(c) ? c : '_';
    }

    if (out83[0] == 0xE5) out83[0] = 0x05;
}

typedef struct {
    uint32_t needed;
    uint32_t run_start_cluster;
    uint32_t run_start_offset;
    uint32_t run_count;
    bool     found;
} free_slot_ctx_t;

static int free_slot_visitor(dirent_visit_ctx_t *ctx, void *user) {
    free_slot_ctx_t *fc = (free_slot_ctx_t*)user;
    fat32_dirent_t *de = ctx->raw;

    bool is_free = (de->name[0] == FAT_DIRENT_FREE || de->name[0] == FAT_DIRENT_END);

    uint32_t offset = ctx->sector_in_cluster * 512 + ctx->index_in_sector * sizeof(fat32_dirent_t);

    if (is_free) {
        if (fc->run_count == 0) {
            fc->run_start_cluster = ctx->cluster;
            fc->run_start_offset = offset;
        }
        fc->run_count++;
        if (fc->run_count >= fc->needed) {
            fc->found = true;
            return 1;
        }
        if (de->name[0] == FAT_DIRENT_END) {
            while (fc->run_count < fc->needed) {
                fc->run_count++;
            }
            fc->found = true;
            return 1;
        }
    } else {
        fc->run_count = 0;
    }
    return 0;
}

static fat_status_t dir_find_free_run(fat32_volume_t *vol, uint32_t dir_cluster,
                                       uint32_t needed, uint32_t *out_cluster, uint32_t *out_offset) {
    free_slot_ctx_t fc;
    fat_memset(&fc, 0, sizeof(fc));
    fc.needed = needed;

    fat_status_t st = dir_walk(vol, dir_cluster, free_slot_visitor, &fc);
    if (st != FAT_OK && st != FAT_ERR_NOT_FOUND) return st;

    if (fc.found) {
        *out_cluster = fc.run_start_cluster;
        *out_offset = fc.run_start_offset;
        return FAT_OK;
    }

    uint32_t cluster = dir_cluster;
    while (true) {
        uint32_t next;
        st = fat_entry_read(vol, cluster, &next);
        if (st != FAT_OK) return st;
        if (is_eoc(next)) break;
        cluster = next;
    }

    uint32_t new_cluster;
    st = cluster_chain_extend(vol, cluster, &new_cluster);
    if (st != FAT_OK) return st;

    *out_cluster = new_cluster;
    *out_offset = 0;
    return FAT_OK;
}

static fat_status_t dir_write_entry_at(fat32_volume_t *vol, uint32_t cluster, uint32_t offset,
                                        const void *entry32) {
    uint32_t sector_in_cluster = offset / vol->dev.sector_size;
    uint32_t offset_in_sector = offset % vol->dev.sector_size;
    uint32_t lba = cluster_to_lba(vol, cluster) + sector_in_cluster;

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    fat_status_t st = disk_read(vol, lba, 1, buf);
    if (st != FAT_OK) return st;

    fat_memcpy(buf + offset_in_sector, entry32, sizeof(fat32_dirent_t));

    return disk_write(vol, lba, 1, buf);
}

static fat_status_t dirent_slot_advance(fat32_volume_t *vol, uint32_t *cluster, uint32_t *offset) {
    *offset += sizeof(fat32_dirent_t);
    if (*offset < vol->bytes_per_cluster) return FAT_OK;

    *offset = 0;
    uint32_t next;
    fat_status_t st = fat_entry_read(vol, *cluster, &next);
    if (st != FAT_OK) return st;
    if (is_eoc(next)) {
        st = cluster_chain_extend(vol, *cluster, &next);
        if (st != FAT_OK) return st;
    }
    *cluster = next;
    return FAT_OK;
}

static fat_status_t dir_create_entry(fat32_volume_t *vol, uint32_t parent_cluster,
                                      const char *name, uint32_t name_len,
                                      uint8_t attr, uint32_t first_cluster,
                                      uint32_t *out_cluster, uint32_t *out_offset) {
    if (name_len == 0 || name_len > 255) return FAT_ERR_INVALID_NAME;

    char name_buf[256];
    fat_memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';

    fat32_dirent_t existing;
    fat_status_t st = dir_lookup(vol, parent_cluster, name_buf, name_len, &existing, NULL, NULL);
    if (st == FAT_OK) return FAT_ERR_ALREADY_EXISTS;
    if (st != FAT_ERR_NOT_FOUND) return st;

    uint8_t short_name[11];
    bool fits_short = try_make_short_name(name_buf, short_name);

    if (!fits_short) {
        for (uint32_t attempt = 1; attempt < 1000000; attempt++) {
            make_numeric_tail_short_name(name_buf, attempt, short_name);
            fat32_dirent_t probe;

            lookup_ctx_t lk;
            fat_memset(&lk, 0, sizeof(lk));
            char shortname_str[13];
            (void)shortname_str;

            bool collision = false;
            {
                struct collide_ctx { const uint8_t *target; bool hit; } cc = { short_name, false };
                uint32_t cluster = parent_cluster;
                uint8_t buf[FAT_MAX_SECTOR_SIZE];
                bool done = false;
                while (!done) {
                    uint32_t lba = cluster_to_lba(vol, cluster);
                    for (uint32_t s = 0; s < vol->sectors_per_cluster && !done; s++) {
                        st = disk_read(vol, lba + s, 1, buf);
                        if (st != FAT_OK) return st;
                        uint32_t epc = vol->dev.sector_size / sizeof(fat32_dirent_t);
                        for (uint32_t e = 0; e < epc; e++) {
                            fat32_dirent_t *de = (fat32_dirent_t*)(buf + e * sizeof(fat32_dirent_t));
                            if (de->name[0] == FAT_DIRENT_END) { done = true; break; }
                            if (de->name[0] == FAT_DIRENT_FREE) continue;
                            if (de->attr == FAT_ATTR_LFN) continue;
                            if (fat_memcmp(de->name, cc.target, 11) == 0) {
                                cc.hit = true; done = true; break;
                            }
                        }
                    }
                    if (done) break;
                    uint32_t next;
                    st = fat_entry_read(vol, cluster, &next);
                    if (st != FAT_OK) return st;
                    if (is_eoc(next)) break;
                    cluster = next;
                }
                collision = cc.hit;
            }
            (void)probe;
            if (!collision) break;
        }
    }

    uint32_t lfn_count = 0;
    {
        uint8_t roundtrip[11];
        bool exact = fits_short && try_make_short_name(name_buf, roundtrip) &&
                     fat_memcmp(roundtrip, short_name, 11) == 0;
        bool all_upper = true;
        for (uint32_t i = 0; i < name_len; i++) {
            if (name_buf[i] >= 'a' && name_buf[i] <= 'z') { all_upper = false; break; }
        }
        if (!exact || !all_upper) {
            lfn_count = (name_len + 12) / 13;
        }
    }

    uint32_t total_slots = lfn_count + 1;
    uint32_t entries_per_cluster = vol->bytes_per_cluster / sizeof(fat32_dirent_t);
    if (total_slots > entries_per_cluster) return FAT_ERR_INVALID_NAME;

    uint32_t run_cluster, run_offset;
    st = dir_find_free_run(vol, parent_cluster, total_slots, &run_cluster, &run_offset);
    if (st != FAT_OK) return st;

    uint8_t checksum = lfn_checksum(short_name);

    uint32_t cur_cluster = run_cluster;
    uint32_t cur_offset = run_offset;

    for (uint32_t i = lfn_count; i >= 1; i--) {
        fat32_lfn_t lfn;
        fat_memset(&lfn, 0, sizeof(lfn));
        lfn.order = (uint8_t)i;
        if (i == lfn_count) lfn.order |= 0x40;
        lfn.attr = FAT_ATTR_LFN;
        lfn.type = 0;
        lfn.checksum = checksum;
        lfn.first_cluster_lo = 0;

        uint32_t base = (i - 1) * 13;
        uint16_t chunk[13];
        for (uint32_t k = 0; k < 13; k++) {
            uint32_t ni = base + k;
            if (ni < name_len) chunk[k] = (uint16_t)(uint8_t)name_buf[ni];
            else if (ni == name_len) chunk[k] = 0x0000;
            else chunk[k] = 0xFFFF;
        }
        for (int k = 0; k < 5; k++) lfn.name1[k] = chunk[k];
        for (int k = 0; k < 6; k++) lfn.name2[k] = chunk[5 + k];
        for (int k = 0; k < 2; k++) lfn.name3[k] = chunk[11 + k];

        st = dir_write_entry_at(vol, cur_cluster, cur_offset, &lfn);
        if (st != FAT_OK) return st;

        if (i > 1) {
            st = dirent_slot_advance(vol, &cur_cluster, &cur_offset);
            if (st != FAT_OK) return st;
        }
    }

    if (lfn_count > 0) {
        st = dirent_slot_advance(vol, &cur_cluster, &cur_offset);
        if (st != FAT_OK) return st;
    }

    fat32_dirent_t de;
    fat_memset(&de, 0, sizeof(de));
    fat_memcpy(de.name, short_name, 11);
    de.attr = attr;
    de.file_size = 0;
    dirent_set_first_cluster(&de, first_cluster);

    st = dir_write_entry_at(vol, cur_cluster, cur_offset, &de);
    if (st != FAT_OK) return st;

    if (out_cluster) *out_cluster = cur_cluster;
    if (out_offset) *out_offset = cur_offset;
    return FAT_OK;
}

static fat_status_t dir_delete_entry(fat32_volume_t *vol, uint32_t dir_cluster,
                                      uint32_t target_cluster, uint32_t target_offset) {
    uint32_t lfn_clusters[20];
    uint32_t lfn_offsets[20];
    uint32_t lfn_run_len = 0;

    uint32_t cluster = dir_cluster;
    uint8_t buf[FAT_MAX_SECTOR_SIZE];

    while (true) {
        uint32_t lba = cluster_to_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            fat_status_t st = disk_read(vol, lba + s, 1, buf);
            if (st != FAT_OK) return st;

            uint32_t epc = vol->dev.sector_size / sizeof(fat32_dirent_t);
            for (uint32_t e = 0; e < epc; e++) {
                fat32_dirent_t *de = (fat32_dirent_t*)(buf + e * sizeof(fat32_dirent_t));
                uint32_t offset = s * vol->dev.sector_size + e * sizeof(fat32_dirent_t);

                if (de->name[0] == FAT_DIRENT_END) return FAT_ERR_NOT_FOUND;

                if (de->name[0] == FAT_DIRENT_FREE) { lfn_run_len = 0; continue; }

                if (de->attr == FAT_ATTR_LFN) {
                    if (lfn_run_len < 20) {
                        lfn_clusters[lfn_run_len] = cluster;
                        lfn_offsets[lfn_run_len] = offset;
                        lfn_run_len++;
                    }
                    continue;
                }

                if (cluster == target_cluster && offset == target_offset) {
                    de->name[0] = FAT_DIRENT_FREE;
                    uint32_t wlba = cluster_to_lba(vol, cluster) + s;
                    fat_status_t wst = disk_write(vol, wlba, 1, buf);
                    if (wst != FAT_OK) return wst;

                    for (uint32_t i = 0; i < lfn_run_len; i++) {
                        uint32_t lc = lfn_clusters[i], lo = lfn_offsets[i];
                        uint32_t lsec = lo / vol->dev.sector_size;
                        uint32_t loff = lo % vol->dev.sector_size;
                        uint8_t lbuf[FAT_MAX_SECTOR_SIZE];
                        uint32_t llba = cluster_to_lba(vol, lc) + lsec;
                        wst = disk_read(vol, llba, 1, lbuf);
                        if (wst != FAT_OK) return wst;
                        lbuf[loff] = FAT_DIRENT_FREE;
                        wst = disk_write(vol, llba, 1, lbuf);
                        if (wst != FAT_OK) return wst;
                    }
                    return FAT_OK;
                }

                lfn_run_len = 0;
            }
        }

        uint32_t next;
        fat_status_t st = fat_entry_read(vol, cluster, &next);
        if (st != FAT_OK) return st;
        if (is_eoc(next)) return FAT_ERR_NOT_FOUND;
        cluster = next;
    }
}
