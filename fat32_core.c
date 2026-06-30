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
 * - Email: heartlessalt01@gmail.com
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

static void fat_memset(void *dst, int val, uint32_t n) {
    uint8_t *d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void fat_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int fat_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t*)a;
    const uint8_t *pb = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static uint32_t fat_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static uint8_t fat_to_upper(uint8_t c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

#define FAT_MAX_SECTOR_SIZE 4096

static fat_status_t disk_read(fat32_volume_t *vol, uint32_t lba, uint32_t count, void *buf) {
    if (!vol->dev.read) return FAT_ERR_IO;
    int r = vol->dev.read(vol->dev.ctx, lba, count, buf);
    return (r == 0) ? FAT_OK : FAT_ERR_IO;
}

static fat_status_t disk_write(fat32_volume_t *vol, uint32_t lba, uint32_t count, const void *buf) {
    if (vol->read_only) return FAT_ERR_READ_ONLY;
    if (!vol->dev.write) return FAT_ERR_IO;
    int r = vol->dev.write(vol->dev.ctx, lba, count, buf);
    return (r == 0) ? FAT_OK : FAT_ERR_IO;
}

static uint32_t cluster_to_lba(fat32_volume_t *vol, uint32_t cluster) {
    return vol->data_start_lba +
           (cluster - FAT32_FIRST_DATA_CLUSTER) * vol->sectors_per_cluster;
}

static fat_status_t fat_entry_read(fat32_volume_t *vol, uint32_t cluster, uint32_t *out_value) {
    if (cluster < 2 || cluster >= vol->total_clusters + 2) return FAT_ERR_INVALID_ARG;

    uint32_t fat_offset = cluster * 4;
    uint32_t sector = vol->fat_start_lba + (fat_offset / vol->dev.sector_size);
    uint32_t offset_in_sector = fat_offset % vol->dev.sector_size;

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    fat_status_t st = disk_read(vol, sector, 1, buf);
    if (st != FAT_OK) return st;

    uint32_t raw = ((uint32_t)buf[offset_in_sector])       |
                   ((uint32_t)buf[offset_in_sector + 1] << 8)  |
                   ((uint32_t)buf[offset_in_sector + 2] << 16) |
                   ((uint32_t)buf[offset_in_sector + 3] << 24);

    *out_value = raw & FAT32_CLUSTER_MASK;
    return FAT_OK;
}

static fat_status_t fat_entry_write(fat32_volume_t *vol, uint32_t cluster, uint32_t value) {
    if (vol->read_only) return FAT_ERR_READ_ONLY;
    if (cluster < 2 || cluster >= vol->total_clusters + 2) return FAT_ERR_INVALID_ARG;

    uint32_t fat_offset = cluster * 4;
    uint32_t sector_off = fat_offset / vol->dev.sector_size;
    uint32_t offset_in_sector = fat_offset % vol->dev.sector_size;

    uint8_t buf[FAT_MAX_SECTOR_SIZE];

    for (uint32_t fat_i = 0; fat_i < vol->bpb.num_fats; fat_i++) {
        uint32_t sector = vol->fat_start_lba + fat_i * vol->bpb.fat_size_32 + sector_off;

        fat_status_t st = disk_read(vol, sector, 1, buf);
        if (st != FAT_OK) return st;

        uint32_t existing = ((uint32_t)buf[offset_in_sector])       |
                             ((uint32_t)buf[offset_in_sector + 1] << 8)  |
                             ((uint32_t)buf[offset_in_sector + 2] << 16) |
                             ((uint32_t)buf[offset_in_sector + 3] << 24);
        uint32_t new_val = (existing & ~FAT32_CLUSTER_MASK) | (value & FAT32_CLUSTER_MASK);

        buf[offset_in_sector]     = (uint8_t)(new_val);
        buf[offset_in_sector + 1] = (uint8_t)(new_val >> 8);
        buf[offset_in_sector + 2] = (uint8_t)(new_val >> 16);
        buf[offset_in_sector + 3] = (uint8_t)(new_val >> 24);

        st = disk_write(vol, sector, 1, buf);
        if (st != FAT_OK) return st;
    }
    return FAT_OK;
}

static bool is_eoc(uint32_t cluster_val) {
    return cluster_val >= FAT32_EOC_MIN;
}

static fat_status_t cluster_alloc(fat32_volume_t *vol, uint32_t *out_cluster) {
    if (vol->read_only) return FAT_ERR_READ_ONLY;

    uint32_t start = (vol->next_free_cluster >= 2) ? vol->next_free_cluster : 2;
    uint32_t total = vol->total_clusters + 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t begin = (pass == 0) ? start : 2;
        uint32_t end   = (pass == 0) ? total : start;
        for (uint32_t c = begin; c < end; c++) {
            uint32_t val;
            fat_status_t st = fat_entry_read(vol, c, &val);
            if (st != FAT_OK) return st;
            if (val == FAT32_FREE_CLUSTER) {
                st = fat_entry_write(vol, c, FAT32_EOC);
                if (st != FAT_OK) return st;

                vol->next_free_cluster = c + 1;
                if (vol->free_cluster_count != 0xFFFFFFFF && vol->free_cluster_count > 0) {
                    vol->free_cluster_count--;
                }

                uint8_t zero[FAT_MAX_SECTOR_SIZE];
                fat_memset(zero, 0, vol->dev.sector_size);
                uint32_t lba = cluster_to_lba(vol, c);
                for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
                    st = disk_write(vol, lba + s, 1, zero);
                    if (st != FAT_OK) return st;
                }

                *out_cluster = c;
                return FAT_OK;
            }
        }
    }
    return FAT_ERR_NO_SPACE;
}

static fat_status_t cluster_chain_free(fat32_volume_t *vol, uint32_t start_cluster) {
    uint32_t c = start_cluster;
    while (c >= 2 && !is_eoc(c) && c != FAT32_FREE_CLUSTER) {
        uint32_t next;
        fat_status_t st = fat_entry_read(vol, c, &next);
        if (st != FAT_OK) return st;

        st = fat_entry_write(vol, c, FAT32_FREE_CLUSTER);
        if (st != FAT_OK) return st;

        if (vol->free_cluster_count != 0xFFFFFFFF) vol->free_cluster_count++;

        c = next;
    }
    return FAT_OK;
}

static fat_status_t cluster_chain_extend(fat32_volume_t *vol, uint32_t last_cluster, uint32_t *out_new) {
    uint32_t new_c;
    fat_status_t st = cluster_alloc(vol, &new_c);
    if (st != FAT_OK) return st;

    st = fat_entry_write(vol, last_cluster, new_c);
    if (st != FAT_OK) return st;

    *out_new = new_c;
    return FAT_OK;
}

static fat_status_t cluster_chain_seek(fat32_volume_t *vol, uint32_t first_cluster,
                                       uint32_t index, bool extend, uint32_t *out_cluster) {
    if (first_cluster < 2) return FAT_ERR_INVALID_ARG;

    uint32_t cur = first_cluster;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t next;
        fat_status_t st = fat_entry_read(vol, cur, &next);
        if (st != FAT_OK) return st;

        if (is_eoc(next)) {
            if (!extend) return FAT_ERR_EOF;
            st = cluster_chain_extend(vol, cur, &next);
            if (st != FAT_OK) return st;
        }
        cur = next;
    }
    *out_cluster = cur;
    return FAT_OK;
}