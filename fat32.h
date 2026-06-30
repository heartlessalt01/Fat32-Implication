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

#ifndef FAT32_H
#define FAT32_H

#include "fat32_types.h"

// error codes

typedef enum {
    FAT_OK                  = 0,
    FAT_ERR_IO               = -1, 
    FAT_ERR_NOT_MOUNTED      = -2,
    FAT_ERR_BAD_BPB          = -3,  
    FAT_ERR_NOT_FOUND        = -4,
    FAT_ERR_ALREADY_EXISTS   = -5,
    FAT_ERR_NO_SPACE         = -6,  
    FAT_ERR_NOT_DIR          = -7,
    FAT_ERR_IS_DIR           = -8,
    FAT_ERR_DIR_NOT_EMPTY    = -9,
    FAT_ERR_INVALID_NAME     = -10,
    FAT_ERR_INVALID_ARG      = -11,
    FAT_ERR_NO_MEMORY        = -12,
    FAT_ERR_READ_ONLY        = -13,
    FAT_ERR_EOF              = -14,
    FAT_ERR_CORRUPT          = -15
} fat_status_t;

typedef int (*fat_disk_read_fn)(void *ctx, uint32_t lba, uint32_t sector_count, void *buffer);
typedef int (*fat_disk_write_fn)(void *ctx, uint32_t lba, uint32_t sector_count, const void *buffer);
typedef struct {
    fat_disk_read_fn  read;
    fat_disk_write_fn write;
    void             *ctx;          
    uint32_t          sector_size;  
} fat_blockdev_t;

typedef struct {
    fat_blockdev_t   dev;
    fat32_bpb_t      bpb;
    uint32_t         fat_start_lba;
    uint32_t         data_start_lba;
    uint32_t         total_clusters;
    uint32_t         root_cluster;
    uint32_t         sectors_per_cluster;
    uint32_t         bytes_per_cluster;
    uint32_t         fsinfo_sector;
    uint32_t         free_cluster_count;  
    uint32_t         next_free_cluster;
    bool             mounted;
    bool             read_only;
} fat32_volume_t;

typedef enum {
    FAT_SEEK_SET = 0,
    FAT_SEEK_CUR = 1,
    FAT_SEEK_END = 2
} fat_seek_whence_t;

typedef struct {
    fat32_volume_t  *vol;
    uint32_t         first_cluster;
    uint32_t         current_cluster;
    uint32_t         file_size;
    uint32_t         position;        
    uint32_t         cluster_index;   
    uint32_t         dirent_cluster;  
    uint32_t         dirent_offset;   
    uint8_t          attr;
    bool             is_dir;
    bool             writable;
} fat_file_t;

typedef struct {
    char     name[256];   
    uint8_t  attr;
    uint32_t size;
    uint32_t first_cluster;
} fat_dirent_info_t;

typedef struct {
    fat32_volume_t *vol;
    uint32_t         dir_first_cluster;
    uint32_t         cur_cluster;
    uint32_t         cur_index;     
} fat_dir_iter_t;

fat_status_t fat32_mount(fat32_volume_t *vol, const fat_blockdev_t *dev, bool read_only);

fat_status_t fat32_unmount(fat32_volume_t *vol);

fat_status_t fat32_flush(fat32_volume_t *vol);

fat_status_t fat32_open(fat32_volume_t *vol, const char *path, bool create, bool write, fat_file_t *out);

fat_status_t fat32_close(fat_file_t *file);

fat_status_t fat32_read(fat_file_t *file, void *buffer, uint32_t size, uint32_t *out_read);

fat_status_t fat32_write(fat_file_t *file, const void *buffer, uint32_t size, uint32_t *out_written);

fat_status_t fat32_seek(fat_file_t *file, int32_t offset, fat_seek_whence_t whence, uint32_t *out_pos);

fat_status_t fat32_truncate(fat_file_t *file, uint32_t new_size);

fat_status_t fat32_mkdir(fat32_volume_t *vol, const char *path);

fat_status_t fat32_remove(fat32_volume_t *vol, const char *path);

fat_status_t fat32_rename(fat32_volume_t *vol, const char *old_path, const char *new_path);

fat_status_t fat32_dir_open(fat32_volume_t *vol, const char *path, fat_dir_iter_t *iter);

fat_status_t fat32_dir_read(fat_dir_iter_t *iter, fat_dirent_info_t *out);  

fat_status_t fat32_dir_close(fat_dir_iter_t *iter);

fat_status_t fat32_stat(fat32_volume_t *vol, const char *path, fat_dirent_info_t *out);
#endif 
