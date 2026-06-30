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
#include "fat32_core.c"
#include "fat32_dirent.c"
#include "fat32_path.c"
#include "fat32_create.c"

fat_status_t fat32_mount(fat32_volume_t *vol, const fat_blockdev_t *dev, bool read_only) {
    if (!vol || !dev || !dev->read) return FAT_ERR_INVALID_ARG;
    if (dev->sector_size == 0 || dev->sector_size > FAT_MAX_SECTOR_SIZE) return FAT_ERR_INVALID_ARG;

    fat_memset(vol, 0, sizeof(*vol));
    vol->dev = *dev;
    vol->read_only = read_only || (dev->write == NULL);

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    fat_status_t st = disk_read(vol, 0, 1, buf);
    if (st != FAT_OK) return st;

    fat_memcpy(&vol->bpb, buf, sizeof(fat32_bpb_t));

    if (vol->bpb.boot_sector_signature != 0xAA55) return FAT_ERR_BAD_BPB;
    if (vol->bpb.bytes_per_sector == 0 || vol->bpb.sectors_per_cluster == 0) return FAT_ERR_BAD_BPB;
    if (vol->bpb.fat_size_32 == 0) return FAT_ERR_BAD_BPB;
    if (vol->bpb.num_fats == 0) return FAT_ERR_BAD_BPB;

    if (vol->bpb.bytes_per_sector != dev->sector_size) {
        return FAT_ERR_BAD_BPB;
    }

    vol->fat_start_lba = vol->bpb.reserved_sector_count;
    vol->data_start_lba = vol->fat_start_lba + (vol->bpb.num_fats * vol->bpb.fat_size_32);
    vol->sectors_per_cluster = vol->bpb.sectors_per_cluster;
    vol->bytes_per_cluster = vol->bpb.bytes_per_sector * vol->bpb.sectors_per_cluster;
    vol->root_cluster = vol->bpb.root_cluster;

    uint32_t total_sectors = vol->bpb.total_sectors_32 ? vol->bpb.total_sectors_32 : vol->bpb.total_sectors_16;
    if (total_sectors <= vol->data_start_lba) return FAT_ERR_BAD_BPB;
    uint32_t data_sectors = total_sectors - vol->data_start_lba;
    vol->total_clusters = data_sectors / vol->sectors_per_cluster;

    if (vol->root_cluster < 2) return FAT_ERR_BAD_BPB;

    vol->free_cluster_count = 0xFFFFFFFF;
    vol->next_free_cluster = 2;
    if (vol->bpb.fs_info_sector != 0 && vol->bpb.fs_info_sector != 0xFFFF) {
        uint8_t fsbuf[FAT_MAX_SECTOR_SIZE];
        if (disk_read(vol, vol->bpb.fs_info_sector, 1, fsbuf) == FAT_OK) {
            fat32_fsinfo_t fsinfo;
            fat_memcpy(&fsinfo, fsbuf, sizeof(fsinfo));
            if (fsinfo.lead_signature == 0x41615252 && fsinfo.trail_signature == 0xAA550000) {
                vol->free_cluster_count = fsinfo.free_cluster_count;
                if (fsinfo.next_free_cluster >= 2) vol->next_free_cluster = fsinfo.next_free_cluster;
            }
        }
        vol->fsinfo_sector = vol->bpb.fs_info_sector;
    }

    vol->mounted = true;
    return FAT_OK;
}

fat_status_t fat32_flush(fat32_volume_t *vol) {
    if (!vol || !vol->mounted) return FAT_ERR_NOT_MOUNTED;
    if (vol->read_only) return FAT_OK;
    if (!vol->fsinfo_sector) return FAT_OK;

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    fat_status_t st = disk_read(vol, vol->fsinfo_sector, 1, buf);
    if (st != FAT_OK) return st;

    fat32_fsinfo_t fsinfo;
    fat_memcpy(&fsinfo, buf, sizeof(fsinfo));
    if (fsinfo.lead_signature == 0x41615252 && fsinfo.trail_signature == 0xAA550000) {
        fsinfo.free_cluster_count = vol->free_cluster_count;
        fsinfo.next_free_cluster = vol->next_free_cluster;
        fat_memcpy(buf, &fsinfo, sizeof(fsinfo));
        st = disk_write(vol, vol->fsinfo_sector, 1, buf);
        if (st != FAT_OK) return st;
    }
    return FAT_OK;
}

fat_status_t fat32_unmount(fat32_volume_t *vol) {
    if (!vol || !vol->mounted) return FAT_ERR_NOT_MOUNTED;
    fat_status_t st = fat32_flush(vol);
    vol->mounted = false;
    return st;
}

fat_status_t fat32_open(fat32_volume_t *vol, const char *path, bool create, bool write, fat_file_t *out) {
    if (!vol || !vol->mounted || !path || !out) return FAT_ERR_INVALID_ARG;
    if ((create || write) && vol->read_only) return FAT_ERR_READ_ONLY;

    fat32_dirent_t entry;
    uint32_t parent_cluster, entry_cluster, entry_offset;
    fat_status_t st = resolve_path(vol, path, &entry, &parent_cluster, &entry_cluster, &entry_offset);

    if (st == FAT_ERR_NOT_FOUND && create) {
        const char *name;
        uint32_t name_len;
        st = resolve_parent_and_name(vol, path, &parent_cluster, &name, &name_len);
        if (st != FAT_OK) return st;

        st = dir_create_entry(vol, parent_cluster, name, name_len, FAT_ATTR_ARCHIVE, 0,
                               &entry_cluster, &entry_offset);
        if (st != FAT_OK) return st;

        fat_memset(&entry, 0, sizeof(entry));
        entry.attr = FAT_ATTR_ARCHIVE;
    } else if (st != FAT_OK) {
        return st;
    }

    if (entry.attr & FAT_ATTR_DIRECTORY) return FAT_ERR_IS_DIR;

    fat_memset(out, 0, sizeof(*out));
    out->vol = vol;
    out->first_cluster = dirent_first_cluster(&entry);
    out->current_cluster = out->first_cluster;
    out->cluster_index = 0;
    out->file_size = entry.file_size;
    out->position = 0;
    out->dirent_cluster = entry_cluster;
    out->dirent_offset = entry_offset;
    out->attr = entry.attr;
    out->is_dir = false;
    out->writable = write && !vol->read_only;

    return FAT_OK;
}

fat_status_t fat32_close(fat_file_t *file) {
    if (!file || !file->vol) return FAT_ERR_INVALID_ARG;
    fat_memset(file, 0, sizeof(*file));
    return FAT_OK;
}

fat_status_t fat32_read(fat_file_t *file, void *buffer, uint32_t size, uint32_t *out_read) {
    if (!file || !file->vol || !buffer) return FAT_ERR_INVALID_ARG;
    fat32_volume_t *vol = file->vol;

    if (out_read) *out_read = 0;
    if (file->position >= file->file_size || size == 0) return FAT_OK;

    if (file->position + size > file->file_size) size = file->file_size - file->position;

    uint8_t *dst = (uint8_t*)buffer;
    uint32_t remaining = size;
    uint32_t total_read = 0;

    uint8_t sector_buf[FAT_MAX_SECTOR_SIZE];

    while (remaining > 0) {
        uint32_t cluster_idx = file->position / vol->bytes_per_cluster;
        uint32_t offset_in_cluster = file->position % vol->bytes_per_cluster;

        if (cluster_idx != file->cluster_index || file->current_cluster < 2) {
            uint32_t c;
            fat_status_t st = cluster_chain_seek(vol, file->first_cluster, cluster_idx, false, &c);
            if (st != FAT_OK) return st;
            file->current_cluster = c;
            file->cluster_index = cluster_idx;
        }

        uint32_t sector_idx = offset_in_cluster / vol->dev.sector_size;
        uint32_t offset_in_sector = offset_in_cluster % vol->dev.sector_size;
        uint32_t lba = cluster_to_lba(vol, file->current_cluster) + sector_idx;

        fat_status_t st = disk_read(vol, lba, 1, sector_buf);
        if (st != FAT_OK) return st;

        uint32_t chunk = vol->dev.sector_size - offset_in_sector;
        if (chunk > remaining) chunk = remaining;

        fat_memcpy(dst, sector_buf + offset_in_sector, chunk);

        dst += chunk;
        file->position += chunk;
        remaining -= chunk;
        total_read += chunk;
    }

    if (out_read) *out_read = total_read;
    return FAT_OK;
}

fat_status_t fat32_write(fat_file_t *file, const void *buffer, uint32_t size, uint32_t *out_written) {
    if (!file || !file->vol || !buffer) return FAT_ERR_INVALID_ARG;
    if (!file->writable) return FAT_ERR_READ_ONLY;
    fat32_volume_t *vol = file->vol;

    if (out_written) *out_written = 0;
    if (size == 0) return FAT_OK;

    if (file->first_cluster < 2) {
        uint32_t c;
        fat_status_t st = cluster_alloc(vol, &c);
        if (st != FAT_OK) return st;
        file->first_cluster = c;
        file->current_cluster = c;
        file->cluster_index = 0;

        uint8_t buf[FAT_MAX_SECTOR_SIZE];
        uint32_t sec = file->dirent_offset / vol->dev.sector_size;
        uint32_t off = file->dirent_offset % vol->dev.sector_size;
        uint32_t lba = cluster_to_lba(vol, file->dirent_cluster) + sec;
        st = disk_read(vol, lba, 1, buf);
        if (st != FAT_OK) return st;
        fat32_dirent_t *de = (fat32_dirent_t*)(buf + off);
        dirent_set_first_cluster(de, c);
        st = disk_write(vol, lba, 1, buf);
        if (st != FAT_OK) return st;
    }

    const uint8_t *src = (const uint8_t*)buffer;
    uint32_t remaining = size;
    uint32_t total_written = 0;

    uint8_t sector_buf[FAT_MAX_SECTOR_SIZE];

    while (remaining > 0) {
        uint32_t cluster_idx = file->position / vol->bytes_per_cluster;
        uint32_t offset_in_cluster = file->position % vol->bytes_per_cluster;

        if (cluster_idx != file->cluster_index || file->current_cluster < 2) {
            uint32_t c;
            fat_status_t st = cluster_chain_seek(vol, file->first_cluster, cluster_idx, true, &c);
            if (st != FAT_OK) return st;
            file->current_cluster = c;
            file->cluster_index = cluster_idx;
        }

        uint32_t sector_idx = offset_in_cluster / vol->dev.sector_size;
        uint32_t offset_in_sector = offset_in_cluster % vol->dev.sector_size;
        uint32_t lba = cluster_to_lba(vol, file->current_cluster) + sector_idx;

        uint32_t chunk = vol->dev.sector_size - offset_in_sector;
        if (chunk > remaining) chunk = remaining;

        fat_status_t st;
        if (chunk < vol->dev.sector_size) {
            st = disk_read(vol, lba, 1, sector_buf);
            if (st != FAT_OK) return st;
        }
        fat_memcpy(sector_buf + offset_in_sector, src, chunk);
        st = disk_write(vol, lba, 1, sector_buf);
        if (st != FAT_OK) return st;

        src += chunk;
        file->position += chunk;
        remaining -= chunk;
        total_written += chunk;

        if (file->position > file->file_size) file->file_size = file->position;
    }

    {
        uint8_t buf[FAT_MAX_SECTOR_SIZE];
        uint32_t sec = file->dirent_offset / vol->dev.sector_size;
        uint32_t off = file->dirent_offset % vol->dev.sector_size;
        uint32_t lba = cluster_to_lba(vol, file->dirent_cluster) + sec;
        fat_status_t st = disk_read(vol, lba, 1, buf);
        if (st != FAT_OK) return st;
        fat32_dirent_t *de = (fat32_dirent_t*)(buf + off);
        de->file_size = file->file_size;
        st = disk_write(vol, lba, 1, buf);
        if (st != FAT_OK) return st;
    }

    if (out_written) *out_written = total_written;
    return FAT_OK;
}

fat_status_t fat32_seek(fat_file_t *file, int32_t offset, fat_seek_whence_t whence, uint32_t *out_pos) {
    if (!file || !file->vol) return FAT_ERR_INVALID_ARG;

    int64_t base;
    switch (whence) {
        case FAT_SEEK_SET: base = 0; break;
        case FAT_SEEK_CUR: base = file->position; break;
        case FAT_SEEK_END: base = file->file_size; break;
        default: return FAT_ERR_INVALID_ARG;
    }

    int64_t new_pos = base + offset;
    if (new_pos < 0) return FAT_ERR_INVALID_ARG;

    file->position = (uint32_t)new_pos;
    file->cluster_index = 0xFFFFFFFF;

    if (out_pos) *out_pos = file->position;
    return FAT_OK;
}

fat_status_t fat32_truncate(fat_file_t *file, uint32_t new_size) {
    if (!file || !file->vol) return FAT_ERR_INVALID_ARG;
    if (!file->writable) return FAT_ERR_READ_ONLY;
    fat32_volume_t *vol = file->vol;

    if (new_size >= file->file_size) {
        file->file_size = new_size;
    } else {
        if (new_size == 0) {
            fat_status_t st = cluster_chain_free(vol, file->first_cluster);
            if (st != FAT_OK) return st;
            file->first_cluster = 0;
            file->current_cluster = 0;
        } else {
            uint32_t last_needed_idx = (new_size - 1) / vol->bytes_per_cluster;
            uint32_t last_cluster;
            fat_status_t st = cluster_chain_seek(vol, file->first_cluster, last_needed_idx, false, &last_cluster);
            if (st != FAT_OK) return st;

            uint32_t next;
            st = fat_entry_read(vol, last_cluster, &next);
            if (st != FAT_OK) return st;

            if (!is_eoc(next)) {
                st = fat_entry_write(vol, last_cluster, FAT32_EOC);
                if (st != FAT_OK) return st;
                st = cluster_chain_free(vol, next);
                if (st != FAT_OK) return st;
            }
        }
        file->file_size = new_size;
        if (file->position > new_size) file->position = new_size;
        file->cluster_index = 0xFFFFFFFF;
    }

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    uint32_t sec = file->dirent_offset / vol->dev.sector_size;
    uint32_t off = file->dirent_offset % vol->dev.sector_size;
    uint32_t lba = cluster_to_lba(vol, file->dirent_cluster) + sec;
    fat_status_t st = disk_read(vol, lba, 1, buf);
    if (st != FAT_OK) return st;
    fat32_dirent_t *de = (fat32_dirent_t*)(buf + off);
    de->file_size = file->file_size;
    if (file->first_cluster == 0) dirent_set_first_cluster(de, 0);
    return disk_write(vol, lba, 1, buf);
}

fat_status_t fat32_mkdir(fat32_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted || !path) return FAT_ERR_INVALID_ARG;
    if (vol->read_only) return FAT_ERR_READ_ONLY;

    uint32_t parent_cluster;
    const char *name;
    uint32_t name_len;
    fat_status_t st = resolve_parent_and_name(vol, path, &parent_cluster, &name, &name_len);
    if (st != FAT_OK) return st;

    uint32_t new_cluster;
    st = cluster_alloc(vol, &new_cluster);
    if (st != FAT_OK) return st;

    uint32_t entry_cluster, entry_offset;
    st = dir_create_entry(vol, parent_cluster, name, name_len, FAT_ATTR_DIRECTORY, new_cluster,
                           &entry_cluster, &entry_offset);
    if (st != FAT_OK) {
        cluster_chain_free(vol, new_cluster);
        return st;
    }

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    fat_memset(buf, 0, vol->dev.sector_size);

    fat32_dirent_t *dot = (fat32_dirent_t*)buf;
    fat_memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dirent_set_first_cluster(dot, new_cluster);

    fat32_dirent_t *dotdot = (fat32_dirent_t*)(buf + sizeof(fat32_dirent_t));
    fat_memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dirent_set_first_cluster(dotdot, (parent_cluster == vol->root_cluster) ? 0 : parent_cluster);

    uint32_t lba = cluster_to_lba(vol, new_cluster);
    return disk_write(vol, lba, 1, buf);
}

static fat_status_t dir_is_empty(fat32_volume_t *vol, uint32_t dir_cluster, bool *out_empty) {
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
                if (de->name[0] == FAT_DIRENT_END) { *out_empty = true; return FAT_OK; }
                if (de->name[0] == FAT_DIRENT_FREE) continue;
                if (de->attr == FAT_ATTR_LFN) continue;
                if (de->name[0] == '.' &&
                    (de->name[1] == ' ' || de->name[1] == '.')) continue;
                *out_empty = false;
                return FAT_OK;
            }
        }
        uint32_t next;
        fat_status_t st = fat_entry_read(vol, cluster, &next);
        if (st != FAT_OK) return st;
        if (is_eoc(next)) { *out_empty = true; return FAT_OK; }
        cluster = next;
    }
}

fat_status_t fat32_remove(fat32_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted || !path) return FAT_ERR_INVALID_ARG;
    if (vol->read_only) return FAT_ERR_READ_ONLY;

    fat32_dirent_t entry;
    uint32_t parent_cluster, entry_cluster, entry_offset;
    fat_status_t st = resolve_path(vol, path, &entry, &parent_cluster, &entry_cluster, &entry_offset);
    if (st != FAT_OK) return st;

    uint32_t target_cluster = dirent_first_cluster(&entry);

    if (entry.attr & FAT_ATTR_DIRECTORY) {
        if (target_cluster < 2) return FAT_ERR_INVALID_ARG;
        bool empty;
        st = dir_is_empty(vol, target_cluster, &empty);
        if (st != FAT_OK) return st;
        if (!empty) return FAT_ERR_DIR_NOT_EMPTY;
    }

    st = dir_delete_entry(vol, parent_cluster, entry_cluster, entry_offset);
    if (st != FAT_OK) return st;

    if (target_cluster >= 2) {
        st = cluster_chain_free(vol, target_cluster);
        if (st != FAT_OK) return st;
    }

    return FAT_OK;
}

fat_status_t fat32_rename(fat32_volume_t *vol, const char *old_path, const char *new_path) {
    if (!vol || !vol->mounted || !old_path || !new_path) return FAT_ERR_INVALID_ARG;
    if (vol->read_only) return FAT_ERR_READ_ONLY;

    fat32_dirent_t entry;
    uint32_t old_parent_cluster, old_entry_cluster, old_entry_offset;
    fat_status_t st = resolve_path(vol, old_path, &entry, &old_parent_cluster, &old_entry_cluster, &old_entry_offset);
    if (st != FAT_OK) return st;

    uint32_t new_parent_cluster;
    const char *new_name;
    uint32_t new_name_len;
    st = resolve_parent_and_name(vol, new_path, &new_parent_cluster, &new_name, &new_name_len);
    if (st != FAT_OK) return st;

    uint32_t target_cluster = dirent_first_cluster(&entry);

    uint32_t out_cluster, out_offset;
    st = dir_create_entry(vol, new_parent_cluster, new_name, new_name_len, entry.attr, target_cluster,
                           &out_cluster, &out_offset);
    if (st != FAT_OK) return st;

    if (entry.file_size != 0) {
        uint8_t buf[FAT_MAX_SECTOR_SIZE];
        uint32_t sec = out_offset / vol->dev.sector_size;
        uint32_t off = out_offset % vol->dev.sector_size;
        uint32_t lba = cluster_to_lba(vol, out_cluster) + sec;
        st = disk_read(vol, lba, 1, buf);
        if (st != FAT_OK) return st;
        fat32_dirent_t *de = (fat32_dirent_t*)(buf + off);
        de->file_size = entry.file_size;
        st = disk_write(vol, lba, 1, buf);
        if (st != FAT_OK) return st;
    }

    return dir_delete_entry(vol, old_parent_cluster, old_entry_cluster, old_entry_offset);
}

fat_status_t fat32_dir_open(fat32_volume_t *vol, const char *path, fat_dir_iter_t *iter) {
    if (!vol || !vol->mounted || !path || !iter) return FAT_ERR_INVALID_ARG;

    fat32_dirent_t entry;
    fat_status_t st = resolve_path(vol, path, &entry, NULL, NULL, NULL);
    if (st != FAT_OK) return st;
    if (!(entry.attr & FAT_ATTR_DIRECTORY)) return FAT_ERR_NOT_DIR;

    uint32_t dir_cluster = dirent_first_cluster(&entry);
    if (dir_cluster == 0) dir_cluster = vol->root_cluster;

    fat_memset(iter, 0, sizeof(*iter));
    iter->vol = vol;
    iter->dir_first_cluster = dir_cluster;
    iter->cur_cluster = dir_cluster;
    iter->cur_index = 0;
    return FAT_OK;
}

fat_status_t fat32_dir_read(fat_dir_iter_t *iter, fat_dirent_info_t *out) {
    if (!iter || !iter->vol || !out) return FAT_ERR_INVALID_ARG;
    fat32_volume_t *vol = iter->vol;

    uint16_t lfn_buf[256];
    uint32_t lfn_len = 0;
    bool lfn_pending = false;

    uint8_t buf[FAT_MAX_SECTOR_SIZE];
    uint32_t entries_per_cluster = vol->bytes_per_cluster / sizeof(fat32_dirent_t);

    while (true) {
        if (iter->cur_index >= entries_per_cluster) {
            uint32_t next;
            fat_status_t st = fat_entry_read(vol, iter->cur_cluster, &next);
            if (st != FAT_OK) return st;
            if (is_eoc(next)) return FAT_ERR_EOF;
            iter->cur_cluster = next;
            iter->cur_index = 0;
        }

        uint32_t sector_idx = (iter->cur_index * sizeof(fat32_dirent_t)) / vol->dev.sector_size;
        uint32_t entries_per_sector = vol->dev.sector_size / sizeof(fat32_dirent_t);
        uint32_t idx_in_sector = iter->cur_index % entries_per_sector;

        uint32_t lba = cluster_to_lba(vol, iter->cur_cluster) + sector_idx;
        fat_status_t st = disk_read(vol, lba, 1, buf);
        if (st != FAT_OK) return st;

        fat32_dirent_t *de = (fat32_dirent_t*)(buf + idx_in_sector * sizeof(fat32_dirent_t));
        iter->cur_index++;

        if (de->name[0] == FAT_DIRENT_END) return FAT_ERR_EOF;
        if (de->name[0] == FAT_DIRENT_FREE) { lfn_pending = false; continue; }

        if (de->attr == FAT_ATTR_LFN) {
            fat32_lfn_t *lfn = (fat32_lfn_t*)de;
            uint16_t chunk[13];
            lfn_extract_chars(lfn, chunk);
            uint8_t seq = lfn->order & 0x1F;
            if (seq == 0) continue;
            uint32_t base = (seq - 1) * 13;
            if (lfn->order & 0x40) {
                lfn_len = base + 13;
                if (lfn_len > 255) lfn_len = 255;
                lfn_pending = true;
            }
            if (lfn_pending) {
                for (int i = 0; i < 13 && (base + i) < 255; i++) lfn_buf[base + i] = chunk[i];
            }
            continue;
        }

        if (de->attr & FAT_ATTR_VOLUME_ID) { lfn_pending = false; continue; }
        if (de->name[0] == '.' && (de->name[1] == ' ' || de->name[1] == '.')) {
            lfn_pending = false; continue;
        }

        if (lfn_pending) {
            uint32_t real_len = 0;
            for (uint32_t i = 0; i < lfn_len && i < 255; i++) {
                if (lfn_buf[i] == 0x0000) break;
                out->name[i] = (lfn_buf[i] <= 0xFF) ? (char)lfn_buf[i] : '?';
                real_len++;
            }
            out->name[real_len] = '\0';
        } else {
            uint32_t pos = 0;
            for (int i = 0; i < 8 && de->name[i] != ' '; i++) out->name[pos++] = (char)de->name[i];
            if (de->name[8] != ' ') {
                out->name[pos++] = '.';
                for (int i = 8; i < 11 && de->name[i] != ' '; i++) out->name[pos++] = (char)de->name[i];
            }
            out->name[pos] = '\0';
        }

        out->attr = de->attr;
        out->size = de->file_size;
        out->first_cluster = dirent_first_cluster(de);
        return FAT_OK;
    }
}

fat_status_t fat32_dir_close(fat_dir_iter_t *iter) {
    if (!iter) return FAT_ERR_INVALID_ARG;
    fat_memset(iter, 0, sizeof(*iter));
    return FAT_OK;
}

fat_status_t fat32_stat(fat32_volume_t *vol, const char *path, fat_dirent_info_t *out) {
    if (!vol || !vol->mounted || !path || !out) return FAT_ERR_INVALID_ARG;

    fat32_dirent_t entry;
    fat_status_t st = resolve_path(vol, path, &entry, NULL, NULL, NULL);
    if (st != FAT_OK) return st;

    uint32_t len = fat_strlen(path);
    int32_t last_slash = -1;
    for (int32_t i = (int32_t)len - 1; i >= 0; i--) if (path[i] == '/') { last_slash = i; break; }
    const char *name = path + last_slash + 1;
    uint32_t name_len = len - (last_slash + 1);
    if (name_len > 255) name_len = 255;
    fat_memcpy(out->name, name, name_len);
    out->name[name_len] = '\0';

    out->attr = entry.attr;
    out->size = entry.file_size;
    out->first_cluster = dirent_first_cluster(&entry);
    return FAT_OK;
}
