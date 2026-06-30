/*
 * ========================================================================
 * FAT32 Driver Library (Bare-Metal / Kernel Level)
 * ========================================================================
 *
 * Copyright © 2026 Heartless. All rights reserved.
 *
 * This Software is designed for use in bare-metal environments,
 * operating system kernels, bootloaders, and low-level systems.
 *
 * OWNERSHIP
 * This Software and all associated source code are the exclusive property
 * of the author.
 *
 * PERMITTED USE (PRIVATE USE ONLY)
 * - You may use, compile, and modify this Software for personal or
 * private non-commercial purposes, including bare-metal and kernel
 * development.
 *
 * RESTRICTIONS
 * - Redistribution, publication, sublicensing, or sharing of this Software,
 * in whole or in part, is strictly prohibited without explicit written
 * permission from the author.
 *
 * COMMERCIAL / DISTRIBUTION USE
 * - Any use in commercial products, paid software, embedded systems,
 * operating systems, firmware, or hardware distributions requires
 * prior written permission from the author.
 *
 * - Selling, licensing, or monetizing this Software or derived works is
 * not allowed without explicit approval.
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

#include "astrafs.h"

void astra_memset(void *dst, int val, uint64_t n) {
    if (!dst) return;
    uint8_t *d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

void astra_memcpy(void *dst, const void *src, uint64_t n) {
    if (!dst || !src) return;
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

int astra_memcmp(const void *a, const void *b, uint64_t n) {
    const uint8_t *pa = (const uint8_t*)a;
    const uint8_t *pb = (const uint8_t*)b;
    for (uint64_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

uint64_t astra_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

astra_status_t astra_disk_read_blocks(astrafs_volume_t *vol, uint64_t block, uint64_t count, void *buf) {
    if (!vol->dev.read) return ASTRA_ERR_IO;
    uint64_t lba = block * vol->sectors_per_block;
    uint64_t sectors = count * vol->sectors_per_block;
    int r = vol->dev.read(vol->dev.ctx, lba, sectors, buf);
    return (r == 0) ? ASTRA_OK : ASTRA_ERR_IO;
}

astra_status_t astra_disk_write_blocks(astrafs_volume_t *vol, uint64_t block, uint64_t count, const void *buf) {
    if (vol->read_only) return ASTRA_ERR_READ_ONLY;
    if (!vol->dev.write) return ASTRA_ERR_IO;
    uint64_t lba = block * vol->sectors_per_block;
    uint64_t sectors = count * vol->sectors_per_block;
    int r = vol->dev.write(vol->dev.ctx, lba, sectors, buf);
    return (r == 0) ? ASTRA_OK : ASTRA_ERR_IO;
}

uint64_t astra_superblock_checksum(const astrafs_superblock_t *sb) {
    const uint8_t *bytes = (const uint8_t*)sb;
    uint64_t sum = 0;
    uint64_t checksum_offset = (uint64_t)((const uint8_t*)&sb->checksum - bytes);
    for (uint64_t i = 0; i < checksum_offset; i++) sum += bytes[i];
    uint64_t after = checksum_offset + sizeof(sb->checksum);
    for (uint64_t i = after; i < sizeof(*sb); i++) sum += bytes[i];
    return sum;
}
