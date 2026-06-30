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

#include "astrafs_internal.h"

#define ASTRA_MAX_BLOCK_SIZE 65536

static uint64_t inodes_per_block(astrafs_volume_t *vol) {
    return vol->block_size / sizeof(astrafs_inode_t);
}

astra_status_t astra_inode_read(astrafs_volume_t *vol, uint64_t inode_id, astrafs_inode_t *out) {
    if (!vol || !out) return ASTRA_ERR_INVALID_ARG;
    if (inode_id >= vol->sb.inode_count) return ASTRA_ERR_INVALID_ARG;

    uint64_t per_block = inodes_per_block(vol);
    uint64_t block_idx = vol->sb.inode_table_start + (inode_id / per_block);
    uint64_t index_in_block = inode_id % per_block;

    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) return ASTRA_ERR_NO_MEMORY;

    astra_status_t st = astra_disk_read_blocks(vol, block_idx, 1, buf);
    if (st != ASTRA_OK) {
        vol->free(buf);
        return st;
    }

    const astrafs_inode_t *raw_inode = (const astrafs_inode_t *)(buf + index_in_block * sizeof(astrafs_inode_t));

    out->inode_id = le64_to_cpu(raw_inode->inode_id);
    out->size = le64_to_cpu(raw_inode->size);
    out->flags = le32_to_cpu(raw_inode->flags);
    out->extent_count = le32_to_cpu(raw_inode->extent_count);
    out->created_time = le64_to_cpu(raw_inode->created_time);
    out->modified_time = le64_to_cpu(raw_inode->modified_time);
    out->extent_overflow_block = le64_to_cpu(raw_inode->extent_overflow_block);

    for (int i = 0; i < ASTRA_INODE_INLINE_EXTENTS; i++) {
        out->extents[i].start_block = le64_to_cpu(raw_inode->extents[i].start_block);
        out->extents[i].block_count = le64_to_cpu(raw_inode->extents[i].block_count);
    }

    astra_memcpy(out->reserved, raw_inode->reserved, sizeof(out->reserved));

    vol->free(buf);
    return ASTRA_OK;
}

astra_status_t astra_inode_write(astrafs_volume_t *vol, const astrafs_inode_t *inode) {
    if (!vol || !inode) return ASTRA_ERR_INVALID_ARG;
    if (vol->read_only) return ASTRA_ERR_READ_ONLY;
    if (inode->inode_id >= vol->sb.inode_count) return ASTRA_ERR_INVALID_ARG;

    if (vol->lock) vol->lock(vol->lock_ctx);

    uint64_t per_block = inodes_per_block(vol);
    uint64_t block_idx = vol->sb.inode_table_start + (inode->inode_id / per_block);
    uint64_t index_in_block = inode->inode_id % per_block;

    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) {
        if (vol->unlock) vol->unlock(vol->lock_ctx);
        return ASTRA_ERR_NO_MEMORY;
    }

    astra_status_t st = astra_disk_read_blocks(vol, block_idx, 1, buf);
    if (st != ASTRA_OK) {
        vol->free(buf);
        if (vol->unlock) vol->unlock(vol->lock_ctx);
        return st;
    }

    astrafs_inode_t *raw_inode = (astrafs_inode_t *)(buf + index_in_block * sizeof(astrafs_inode_t));

    raw_inode->inode_id = cpu_to_le64(inode->inode_id);
    raw_inode->size = cpu_to_le64(inode->size);
    raw_inode->flags = cpu_to_le32(inode->flags);
    raw_inode->extent_count = cpu_to_le32(inode->extent_count);
    raw_inode->created_time = cpu_to_le64(inode->created_time);
    raw_inode->modified_time = cpu_to_le64(inode->modified_time);
    raw_inode->extent_overflow_block = cpu_to_le64(inode->extent_overflow_block);

    for (int i = 0; i < ASTRA_INODE_INLINE_EXTENTS; i++) {
        raw_inode->extents[i].start_block = cpu_to_le64(inode->extents[i].start_block);
        raw_inode->extents[i].block_count = cpu_to_le64(inode->extents[i].block_count);
    }

    astra_memcpy(raw_inode->reserved, inode->reserved, sizeof(inode->reserved));

    st = astra_disk_write_blocks(vol, block_idx, 1, buf);
    vol->free(buf);

    if (vol->unlock) vol->unlock(vol->lock_ctx);
    return st;
}

astra_status_t astra_inode_alloc(astrafs_volume_t *vol, uint32_t flags, uint64_t *out_inode_id) {
    if (!vol || !out_inode_id) return ASTRA_ERR_INVALID_ARG;
    if (vol->read_only) return ASTRA_ERR_READ_ONLY;

    if (vol->lock) vol->lock(vol->lock_ctx);

    astra_status_t final_status = ASTRA_ERR_NO_INODES;
    
    if (vol->sb.free_inodes == 0) {
        if (vol->unlock) vol->unlock(vol->lock_ctx);
        return ASTRA_ERR_NO_INODES;
    }

    for (uint64_t id = 0; id < vol->sb.inode_count; id++) {
        astrafs_inode_t ino;
        astra_status_t st = astra_inode_read(vol, id, &ino);
        if (st != ASTRA_OK) {
            final_status = st;
            break;
        }

        if (!(ino.flags & ASTRA_INODE_FLAG_INUSE)) {
            astra_memset(&ino, 0, sizeof(ino));
            ino.inode_id = id;
            ino.flags = flags | ASTRA_INODE_FLAG_INUSE;
            ino.extent_count = 0;
            ino.extent_overflow_block = 0;
            
            st = astra_inode_write(vol, &ino);
            if (st != ASTRA_OK) {
                final_status = st;
                break;
            }

            if (vol->sb.free_inodes > 0) vol->sb.free_inodes--;

            *out_inode_id = id;
            final_status = ASTRA_OK;
            break;
        }
    }

    if (vol->unlock) vol->unlock(vol->lock_ctx);
    return final_status;
}

astra_status_t astra_inode_free(astrafs_volume_t *vol, uint64_t inode_id) {
    if (!vol) return ASTRA_ERR_INVALID_ARG;
    
    astrafs_inode_t ino;
    astra_status_t st = astra_inode_read(vol, inode_id, &ino);
    if (st != ASTRA_OK) return st;

    if (!(ino.flags & ASTRA_INODE_FLAG_INUSE)) {
        return ASTRA_OK; 
    }

    st = astra_inode_free_all_extents(vol, &ino);
    if (st != ASTRA_OK) return st;

    astra_memset(&ino, 0, sizeof(ino));
    ino.inode_id = inode_id;
    ino.flags = 0;

    st = astra_inode_write(vol, &ino);
    if (st != ASTRA_OK) return st;

    vol->sb.free_inodes++;
    return ASTRA_OK;
}

static uint64_t overflow_extents_per_block(astrafs_volume_t *vol) {
    return (vol->block_size - sizeof(astrafs_extent_overflow_hdr_t)) / sizeof(astrafs_extent_t);
}

astra_status_t astra_inode_get_extents(astrafs_volume_t *vol, const astrafs_inode_t *inode,
                                       astrafs_extent_t *out_array, uint32_t max_extents,
                                       uint32_t *out_count) {
    if (!vol || !inode || !out_array || !out_count) return ASTRA_ERR_INVALID_ARG;

    uint32_t n = 0;
    uint32_t inline_n = (inode->extent_count < ASTRA_INODE_INLINE_EXTENTS)
                        ? inode->extent_count : ASTRA_INODE_INLINE_EXTENTS;

    for (uint32_t i = 0; i < inline_n && n < max_extents; i++, n++) {
        out_array[n] = inode->extents[i];
    }

    uint64_t next = inode->extent_overflow_block;
    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) return ASTRA_ERR_NO_MEMORY;

    astra_status_t final_status = ASTRA_OK;

    while (next != 0 && n < max_extents) {
        astra_status_t st = astra_disk_read_blocks(vol, next, 1, buf);
        if (st != ASTRA_OK) {
            final_status = st;
            break;
        }

        const astrafs_extent_overflow_hdr_t *hdr = (const astrafs_extent_overflow_hdr_t *)buf;
        uint64_t hdr_next = le64_to_cpu(hdr->next_block);
        uint64_t hdr_count = le64_to_cpu(hdr->extent_count);

        const astrafs_extent_t *raw_exts = (const astrafs_extent_t*)(buf + sizeof(astrafs_extent_overflow_hdr_t));
        
        uint64_t max_possible = overflow_extents_per_block(vol);
        if (hdr_count > max_possible) {
            final_status = ASTRA_ERR_CORRUPT;
            break;
        }

        for (uint64_t i = 0; i < hdr_count && n < max_extents; i++, n++) {
            out_array[n].start_block = le64_to_cpu(raw_exts[i].start_block);
            out_array[n].block_count = le64_to_cpu(raw_exts[i].block_count);
        }
        next = hdr_next;
    }

    vol->free(buf);
    *out_count = n;
    return final_status;
}

astra_status_t astra_inode_append_extent(astrafs_volume_t *vol, astrafs_inode_t *inode,
                                         const astrafs_extent_t *extent) {
    if (!vol || !inode || !extent) return ASTRA_ERR_INVALID_ARG;
    if (vol->read_only) return ASTRA_ERR_READ_ONLY;

    if (inode->extent_count < ASTRA_INODE_INLINE_EXTENTS) {
        inode->extents[inode->extent_count] = *extent;
        inode->extent_count++;
        return ASTRA_OK;
    }

    if (vol->lock) vol->lock(vol->lock_ctx);

    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) {
        if (vol->unlock) vol->unlock(vol->lock_ctx);
        return ASTRA_ERR_NO_MEMORY;
    }

    uint64_t per_block = overflow_extents_per_block(vol);
    uint64_t cur = inode->extent_overflow_block;
    uint64_t prev = 0;
    astra_status_t final_status = ASTRA_OK;

    while (cur != 0) {
        astra_status_t st = astra_disk_read_blocks(vol, cur, 1, buf);
        if (st != ASTRA_OK) {
            final_status = st;
            goto cleanup;
        }

        astrafs_extent_overflow_hdr_t *hdr = (astrafs_extent_overflow_hdr_t *)buf;
        uint64_t hdr_next = le64_to_cpu(hdr->next_block);
        uint64_t hdr_count = le64_to_cpu(hdr->extent_count);

        if (hdr_count < per_block) {
            astrafs_extent_t *raw_exts = (astrafs_extent_t *)(buf + sizeof(astrafs_extent_overflow_hdr_t));

            raw_exts[hdr_count].start_block = cpu_to_le64(extent->start_block);
            raw_exts[hdr_count].block_count = cpu_to_le64(extent->block_count);

            hdr->extent_count = cpu_to_le64(hdr_count + 1);

            st = astra_disk_write_blocks(vol, cur, 1, buf);
            if (st != ASTRA_OK) {
                final_status = st;
                goto cleanup;
            }

            inode->extent_count++;
            goto cleanup;
        }

        prev = cur;
        cur = hdr_next;
    }

    uint64_t new_block;
    astra_status_t st = astrafs_alloc_block(vol, &new_block);
    if (st != ASTRA_OK) {
        final_status = st;
        goto cleanup;
    }

    astra_memset(buf, 0, vol->block_size);
    astrafs_extent_overflow_hdr_t *new_hdr = (astrafs_extent_overflow_hdr_t *)buf;
    new_hdr->next_block = cpu_to_le64(0);
    new_hdr->extent_count = cpu_to_le64(1);

    astrafs_extent_t *raw_ext = (astrafs_extent_t *)(buf + sizeof(astrafs_extent_overflow_hdr_t));
    raw_ext->start_block = cpu_to_le64(extent->start_block);
    raw_ext->block_count = cpu_to_le64(extent->block_count);

    st = astra_disk_write_blocks(vol, new_block, 1, buf);
    if (st != ASTRA_OK) {
        astrafs_free_block(vol, new_block);
        final_status = st;
        goto cleanup;
    }

    if (inode->extent_overflow_block == 0) {
        inode->extent_overflow_block = new_block;
    } else if (prev != 0) {
        st = astra_disk_read_blocks(vol, prev, 1, buf);
        if (st != ASTRA_OK) {
            final_status = st;
            goto cleanup;
        }

        astrafs_extent_overflow_hdr_t *prev_hdr = (astrafs_extent_overflow_hdr_t *)buf;
        prev_hdr->next_block = cpu_to_le64(new_block);

        st = astra_disk_write_blocks(vol, prev, 1, buf);
        if (st != ASTRA_OK) {
            final_status = st;
            goto cleanup;
        }
    }

    inode->extent_count++;

cleanup:
    vol->free(buf);
    if (vol->unlock) vol->unlock(vol->lock_ctx);
    return final_status;
}

astra_status_t astra_inode_free_all_extents(astrafs_volume_t *vol, astrafs_inode_t *inode) {
    if (!vol || !inode) return ASTRA_ERR_INVALID_ARG;

    uint32_t inline_n = (inode->extent_count < ASTRA_INODE_INLINE_EXTENTS)
                        ? inode->extent_count : ASTRA_INODE_INLINE_EXTENTS;

    for (uint32_t i = 0; i < inline_n; i++) {
        astra_status_t st = astrafs_free_extent(vol, &inode->extents[i]);
        if (st != ASTRA_OK) return st;
    }

    uint64_t cur = inode->extent_overflow_block;
    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) return ASTRA_ERR_NO_MEMORY;

    astra_status_t final_status = ASTRA_OK;

    while (cur != 0) {
        astra_status_t st = astra_disk_read_blocks(vol, cur, 1, buf);
        if (st != ASTRA_OK) {
            final_status = st;
            break;
        }

        const astrafs_extent_overflow_hdr_t *hdr = (const astrafs_extent_overflow_hdr_t *)buf;
        uint64_t hdr_next = le64_to_cpu(hdr->next_block);
        uint64_t hdr_count = le64_to_cpu(hdr->extent_count);

        const astrafs_extent_t *raw_exts = (const astrafs_extent_t*)(buf + sizeof(astrafs_extent_overflow_hdr_t));
        
        uint64_t max_possible = overflow_extents_per_block(vol);
        if (hdr_count > max_possible) {
            final_status = ASTRA_ERR_CORRUPT;
            break;
        }

        for (uint64_t i = 0; i < hdr_count; i++) {
            astrafs_extent_t ext;
            ext.start_block = le64_to_cpu(raw_exts[i].start_block);
            ext.block_count = le64_to_cpu(raw_exts[i].block_count);

            st = astrafs_free_extent(vol, &ext);
            if (st != ASTRA_OK) {
                final_status = st;
                break;
            }
        }

        if (final_status != ASTRA_OK) break;

        uint64_t next = hdr_next;

        st = astrafs_free_block(vol, cur);
        if (st != ASTRA_OK) {
            final_status = st;
            break;
        }

        cur = next;
    }

    vol->free(buf);

    if (final_status == ASTRA_OK) {
        inode->extent_count = 0;
        inode->extent_overflow_block = 0;
        inode->size = 0;
    }

    return final_status;
}

astra_status_t astra_inode_offset_to_block(astrafs_volume_t *vol, const astrafs_inode_t *inode,
                                           uint64_t byte_offset, uint64_t *out_block,
                                           uint32_t *out_offset_in_block, uint64_t *out_run_remaining) {
    if (!vol || !inode || !out_block || !out_offset_in_block || !out_run_remaining) 
        return ASTRA_ERR_INVALID_ARG;

    uint64_t block_target = byte_offset / vol->block_size;
    uint32_t offset_in_block = (uint32_t)(byte_offset % vol->block_size);

    uint64_t cumulative = 0;

    uint32_t inline_n = (inode->extent_count < ASTRA_INODE_INLINE_EXTENTS)
                        ? inode->extent_count : ASTRA_INODE_INLINE_EXTENTS;

    for (uint32_t i = 0; i < inline_n; i++) {
        const astrafs_extent_t *e = &inode->extents[i];

        if (block_target < cumulative + e->block_count) {
            uint64_t within = block_target - cumulative;

            *out_block = e->start_block + within;
            *out_offset_in_block = offset_in_block;
            *out_run_remaining = e->block_count - within;

            return ASTRA_OK;
        }

        cumulative += e->block_count;
    }

    uint64_t cur = inode->extent_overflow_block;
    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) return ASTRA_ERR_NO_MEMORY;

    astra_status_t final_status = ASTRA_ERR_EOF;

    while (cur != 0) {
        astra_status_t st = astra_disk_read_blocks(vol, cur, 1, buf);
        if (st != ASTRA_OK) {
            final_status = st;
            break;
        }

        const astrafs_extent_overflow_hdr_t *hdr = (const astrafs_extent_overflow_hdr_t *)buf;
        uint64_t hdr_next = le64_to_cpu(hdr->next_block);
        uint64_t hdr_count = le64_to_cpu(hdr->extent_count);

        const astrafs_extent_t *raw_exts = (const astrafs_extent_t*)(buf + sizeof(astrafs_extent_overflow_hdr_t));
        
        uint64_t max_possible = overflow_extents_per_block(vol);
        if (hdr_count > max_possible) {
            final_status = ASTRA_ERR_CORRUPT;
            break;
        }

        for (uint64_t i = 0; i < hdr_count; i++) {
            astrafs_extent_t e;
            e.start_block = le64_to_cpu(raw_exts[i].start_block);
            e.block_count = le64_to_cpu(raw_exts[i].block_count);

            if (block_target < cumulative + e.block_count) {
                uint64_t within = block_target - cumulative;

                *out_block = e.start_block + within;
                *out_offset_in_block = offset_in_block;
                *out_run_remaining = e.block_count - within;

                final_status = ASTRA_OK;
                break;
            }

            cumulative += e.block_count;
        }

        if (final_status == ASTRA_OK) break;
        cur = hdr_next;
    }

    vol->free(buf);
    return final_status;
}

astra_status_t astra_inode_ensure_blocks(astrafs_volume_t *vol, astrafs_inode_t *inode, uint64_t needed_blocks) {
    if (!vol || !inode) return ASTRA_ERR_INVALID_ARG;
    
    uint64_t have = 0;

    uint32_t inline_n = (inode->extent_count < ASTRA_INODE_INLINE_EXTENTS)
                        ? inode->extent_count : ASTRA_INODE_INLINE_EXTENTS;

    for (uint32_t i = 0; i < inline_n; i++) {
        have += inode->extents[i].block_count;
    }

    uint64_t cur = inode->extent_overflow_block;
    uint8_t *buf = (uint8_t *)vol->malloc(vol->block_size);
    if (!buf) return ASTRA_ERR_NO_MEMORY;

    while (cur != 0) {
        astra_status_t st = astra_disk_read_blocks(vol, cur, 1, buf);
        if (st != ASTRA_OK) {
            vol->free(buf);
            return st;
        }

        const astrafs_extent_overflow_hdr_t *hdr = (const astrafs_extent_overflow_hdr_t *)buf;
        uint64_t hdr_count = le64_to_cpu(hdr->extent_count);
        const astrafs_extent_t *raw_exts = (const astrafs_extent_t*)(buf + sizeof(astrafs_extent_overflow_hdr_t));

        uint64_t max_possible = overflow_extents_per_block(vol);
        if (hdr_count > max_possible) {
            vol->free(buf);
            return ASTRA_ERR_CORRUPT;
        }

        for (uint64_t i = 0; i < hdr_count; i++) {
            have += le64_to_cpu(raw_exts[i].block_count);
        }

        cur = le64_to_cpu(hdr->next_block);
    }

    vol->free(buf);

    while (have < needed_blocks) {
        uint64_t want = needed_blocks - have;

        astrafs_extent_t ext;
        astra_status_t st = astrafs_alloc_extent(vol, want, &ext);
        if (st != ASTRA_OK) return st;

        st = astra_inode_append_extent(vol, inode, &ext);
        if (st != ASTRA_OK) {
            astrafs_free_extent(vol, &ext);
            return st;
        }

        have += ext.block_count;
    }

    return ASTRA_OK;
}
