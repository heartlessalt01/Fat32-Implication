# FAT32 Driver for a Freestanding OS

A full read/write/create/delete FAT32 driver in C, written for a bare-metal
kernel (no libc dependency). Tested against a synthetic FAT32 image and
verified for: mount, nested mkdir, long-filename (LFN) create, single- and
multi-cluster file read/write, seek, directory listing, cross-directory
rename, and delete (including empty-directory protection).

## Files

- `fat32_types.h` — on-disk structures (BPB, FSInfo, 8.3 dirent, LFN dirent)
  and freestanding integer typedefs. **If your OS already has its own
  `stdint.h`-equivalent, delete the typedef block at the top and `#include`
  your own header instead.**
- `fat32.h` — the public API you call from the rest of your kernel.
- `fat32.c` — top-level entry point; `#include`s the other `.c` files below.
  Compile **only this file** — don't add `fat32_core.c` etc. to your build
  separately, they're internal translation units pulled in via `#include`.
- `fat32_core.c` — disk I/O wrappers, FAT table read/write, cluster
  allocation/freeing, cluster-chain walking.
- `fat32_dirent.c` — low-level directory-entry iteration primitive.
- `fat32_path.c` — path resolution (`/a/b/c` → dirent), 8.3/LFN name
  matching.
- `fat32_create.c` — directory entry creation (LFN generation, 8.3 alias
  generation with numeric tails), and deletion.

## Integrating with your kernel

You need to provide two callbacks that talk to your actual disk (ATA/AHCI/
NVMe/ramdisk/whatever) — the driver never touches hardware directly:

```c
#include "fat32.h"

static int my_disk_read(void *ctx, uint32_t lba, uint32_t count, void *buffer) {
    // read `count` sectors starting at LBA `lba` into `buffer`
    // return 0 on success, nonzero on failure
}

static int my_disk_write(void *ctx, uint32_t lba, uint32_t count, const void *buffer) {
    // write `count` sectors starting at LBA `lba` from `buffer`
    // return 0 on success, nonzero on failure
}

fat_blockdev_t dev = {
    .read = my_disk_read,
    .write = my_disk_write,
    .ctx = NULL,          // passed back to your callbacks, use for device handle
    .sector_size = 512
};

fat32_volume_t vol;
fat_status_t st = fat32_mount(&vol, &dev, /*read_only=*/false);
```

Since you mentioned you don't have a disk driver yet (only WAV codec
drivers so far) — `my_disk_read`/`my_disk_write` are the next thing you'll
need to write, against whatever storage controller your OS targets (ATA
PIO is the simplest starting point if this is for x86).

## Basic usage

```c
fat_file_t f;
fat32_open(&vol, "/boot/config.txt", /*create=*/true, /*write=*/true, &f);
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
fat32_dir_open(&vol, "/", &it);
fat_dirent_info_t info;
while (fat32_dir_read(&it, &info) == FAT_OK) {
    // info.name, info.attr, info.size, info.first_cluster
}
fat32_dir_close(&it);
```

## Known limitations / things to be aware of

- **8.3 names only contain ASCII filtering for LFN matching.** Long names
  are stored/round-tripped as UCS-2 per spec, but lookup/compare currently
  treats non-ASCII LFN characters as non-matching. Extend
  `lfn_chars_match_ascii` if you need full Unicode filenames.
- **No write caching** — every `fat32_write`/`fat32_read` call does
  synchronous sector I/O. Fine for correctness; if you need performance,
  add a sector cache layer in your disk callbacks (not in this driver).
- **Timestamps are zeroed.** Directory entries don't get real
  creation/modify times. Hook your RTC driver into `dir_create_entry` in
  `fat32_create.c` (search for "Timestamps intentionally left zero") and
  into `fat32_write`'s size-update block if you want this.
- **`fat32_truncate` growing a file** just updates the logical size field;
  it doesn't allocate/zero new clusters until you actually write to them.
  This matches common embedded FAT driver behavior but means reading past
  the old EOF before writing there will return stale/garbage cluster data
  rather than zeros — usually fine for an OS's own filesystem use, but
  worth knowing.
- **Single-threaded.** No locking. If multiple kernel threads can touch
  the same `fat32_volume_t` concurrently, add your own mutex around calls.
- Tested with `sectors_per_cluster = 1` (worst case for boundary bugs);
  also exercised multi-cluster file read/write/seek. Larger cluster sizes
  (4, 8, 64 etc.) use the same code paths and should work, but you may
  want to test against your real disk image once you have a working disk
  driver.

## Testing this yourself

A pure-Python FAT32 image formatter (`make_test_image.py`, not included
in the deliverable but easy to recreate) and a hosted-mode test harness
were used to validate this driver against a real 32MB FAT32 volume image,
covering mount, nested directories, LFN names, multi-cluster files, seek,
listing, rename, and delete — all passing. If you want the test harness
itself to adapt for your own regression testing, let me know and I'll
hand it over too.
