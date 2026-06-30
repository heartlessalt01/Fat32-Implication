# FAT32 Driver for a Freestanding OS

A full read/write/create/delete FAT32 driver in C, written for a bare-metal
kernel (no libc dependency). Tested against a synthetic FAT32 image and
verified for: mount, nested mkdir, long-filename (LFN) create, single- and
multi-cluster file read/write, seek, directory listing, cross-directory
rename, and delete (including empty-directory protection).

## Files

- `fat32_types.h`
- `fat32.h`
- `fat32.c`
- `fat32_core.c`
- `fat32_dirent.c`
- `fat32_path.c`
- `fat32_create.c`

## Integrating with your kernel

You need to provide disk read/write callbacks for your storage device.

```c
#include "fat32.h"

static int my_disk_read(void *ctx, uint32_t lba, uint32_t count, void *buffer) {
}

static int my_disk_write(void *ctx, uint32_t lba, uint32_t count, const void *buffer) {
}

fat_blockdev_t dev = {
    .read = my_disk_read,
    .write = my_disk_write,
    .ctx = NULL,
    .sector_size = 512
};

fat32_volume_t vol;
fat_status_t st = fat32_mount(&vol, &dev, false);
```

## Basic usage

```c
fat_file_t f;

fat32_open(&vol, "/boot/config.txt", true, true, &f);
fat32_write(&f, "hello", 5, NULL);
fat32_close(&f);

fat32_open(&vol, "/boot/config.txt", false, false, &f);

char buf[64];
uint32_t got;

fat32_read(&f, buf, sizeof(buf), &got);
fat32_close(&f);

fat32_mkdir(&vol, "/boot");
fat32_remove(&vol, "/boot/config.txt");

fat32_rename(&vol, "/a.txt", "/b.txt");

fat_dir_iter_t it;
fat_dirent_info_t info;

fat32_dir_open(&vol, "/", &it);

while (fat32_dir_read(&it, &info) == FAT_OK) {
}

fat32_dir_close(&it);
```

## Known limitations

- Long filename lookup currently supports ASCII matching only.
- No write caching.
- Timestamps are not implemented.
- `fat32_truncate` only updates file size until new data is written.
- No built-in locking for multi-threaded access.
- Tested with `sectors_per_cluster = 1`.
- Larger cluster sizes should be tested on real disk images.

## Testing

A FAT32 image formatter and hosted-mode test harness were used to validate
the driver against a FAT32 volume image.

Test coverage includes:

- Mounting
- Nested directories
- Long filename support
- Multi-cluster file operations
- File seeking
- Directory listing
- Rename operations
- Delete operations
