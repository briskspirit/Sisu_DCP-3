# littlefs

Based on upstream v2.11.3, commit
`6cb4e86540eca0d9ba62500a298385c9d863c8be`:
https://github.com/littlefs-project/littlefs

Only the four library source/header files and upstream license are vendored.
Sisu supplies static buffers (`LFS_NO_MALLOC`) and its own block-device adapter.
The BSD-3-Clause license in LICENSE.md applies to these upstream files.

Local change in `lfs_mount_`: defer adopting and comparing the on-disk block
count until the superblock tail chain has been traversed. After filesystem
growth, an older superblock can retain the old size while a newer root has
moved beyond that extent. Applying the old size during discovery rejects the
valid newer root. All visited metadata pairs are checked against the final
durable size before mount succeeds. Normal post-mount bounds are unchanged.
For automatic-size mounts (`block_count=0`), the Sisu block-device adapter
enforces the physical partition bounds throughout discovery, and checks the
final size before running writable consistency recovery.

Regression coverage: `tests/test_storage_lfs_growth.c`; the split-volume and
object tests cover interrupted writes, remounts, and storage-full recovery.

Two optional/read-only extensions support Sisu's per-category admission policy:

- `lfs_config.alloc_guard` runs before a free block is handed to the allocator.
  It can return `LFS_ERR_NOSPC` without changing the output block or consuming
  the candidate. Leaving it null preserves upstream allocation behavior. The
  guard counts both blocks of a metadata pair, unlike an erase callback.
- `lfs_dir_blocks` traverses one directory's complete split chain and immediate
  files' data blocks. It does not descend into child directories or include
  uncommitted open files. Empty split pairs are included; reconstructing usage
  from visible filenames alone would miss them. This is a read-only operation
  using the existing metadata and CTZ traversal helpers.

These extensions do not change the disk format or littlefs atomic-commit
protocol. `tests/test_storage_objects.c` covers block attribution, independent
category exhaustion, quota denial, recovery/reuse and repeated inline updates
at a full quota (including the allocator's NOSPC wear-relocation fallback).
