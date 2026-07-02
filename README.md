# FAT32 Driver for a Freestanding OS

A full read/write/create/delete FAT32 driver in C, written for a bare-metal kernel (no libc dependency). Tested against a synthetic FAT32 image and verified for: mount, nested mkdir, long-filename (LFN) creation, single- and multi-cluster file read/write, seek, directory listing, cross-directory rename, and delete (including empty-directory protection).

---

## Files

- `fat32_types.h` — on-disk structures (BPB, FSInfo, 8.3 dirent, LFN dirent) and freestanding integer typedefs. If your OS already provides a `stdint.h` equivalent, remove the typedef block and include your own header.
- `fat32.h` — public API for the kernel.
- `fat32.c` — main entry point; includes all internal modules. **Compile only this file**.
- `fat32_core.c` — disk I/O wrappers, FAT management, cluster allocation, and chain traversal.
- `fat32_dirent.c` — directory iteration logic.
- `fat32_path.c` — path resolution and 8.3 + LFN name matching.
- `fat32_create.c` — file/dir creation, LFN generation, and delete logic.

---

## Integrating with your kernel

You must provide block device callbacks. The driver does not access hardware directly.

```c
#include "fat32.h"

static int my_disk_read(void *ctx, uint32_t lba, uint32_t count, void *buffer) {
    // Read sectors starting at LBA into buffer
    return 0;
}

static int my_disk_write(void *ctx, uint32_t lba, uint32_t count, const void *buffer) {
    // Write sectors from buffer starting at LBA
    return 0;
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

---

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
    // iterate files
}
fat32_dir_close(&it);
```

---

## Known limitations

- Long filename lookup supports ASCII matching only.
- No write caching (all I/O is synchronous).
- No timestamps implemented.
- `fat32_truncate` only updates file size until new writes occur.
- Not thread-safe (no internal locking).
- Tested with `sectors_per_cluster = 1`.
- Larger cluster sizes should be validated on real hardware/images.

---

## Testing

Validated using a synthetic FAT32 disk image and a hosted-mode test harness.

Test coverage includes:
- Mounting
- Nested directory creation
- Long filename creation and lookup
- Single- and multi-cluster file I/O
- Seek operations
- Directory listing
- Rename operations
- File and directory deletion (including safety checks)
