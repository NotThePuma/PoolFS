# Portable PoolFS member images

The target is a password-encrypted PoolFS drive built from **1 to 16 member
images**. A member is an ordinary file on a host partition. Members may have
different fixed sizes and may be on the same or different partitions, host
filesystems, and physical drives. Every member recorded at format time is
required to mount that pool. Adding or removing a member later requires a new
pool and a file copy.

The v6 image header stores member identities and sizes, not host paths or
drive letters. To transport a pool, unmount it, copy **all** member images,
keep each image's exact length and bytes, and give `poolfsctl` one image as a
seed. It finds other members in the seed folder and each drive's `PoolFS`
folder. Additional image paths or folders can be passed as search roots:

```powershell
.\build\poolfsctl.exe discover 'E:\Moved\member-a.img' 'F:\OtherFolder\member-b.img'
.\build\poolfsctl.exe mount P: 'E:\Moved\member-a.img' 'F:\OtherFolder\member-b.img'
```

A one-member encrypted pool uses the same password prompt:

```powershell
.\build\poolfs-format.exe --encrypted 384 'E:\PoolFS\member.img'
.\build\poolfsctl.exe mount P: 'E:\PoolFS\member.img'
```

Use `poolfsctl unmount P:` and wait for it to finish before disconnecting or
copying member images. To move one member and update an existing config in place,
select **Move image…** in the manager or use:

```powershell
.\build\poolfsctl.exe move-member '.\My Pool.json' 'E:\PoolFS\member-a.img' 'F:\Archive\member-a.img'
```

The destination must be a new file in an existing folder with enough free space
to hold the image. PoolFS removes the original after it saves the updated config.
For a manual copy, compare source and destination SHA-256 hashes. Run `poolfs-check`
and `poolfs-scrub` with the complete set before relying on the copy. Do not mount
two writable copies of the same pool
at once. PoolFS opens writable member images exclusively on Windows to prevent
two local processes from writing the same images.

This implementation mounts on **Windows** with the WinFsp runtime and Windows
CNG. The host filesystem must provide a writable, seekable regular file and
preserve its length and contents. NTFS and exFAT can host large images. FAT32
has a per-file limit below 4 GiB, so use member images below that limit or
place multiple member images on the host. Host filesystem names, free-space
rules, and removable-media behavior still apply. Mounting the pool on Linux or
macOS requires a separate platform adapter and cryptographic implementation.
The current password format uses the UTF-8 bytes of console input without
Unicode normalization. Until a versioned normalization rule is added, use an
ASCII passphrase to avoid visually identical Unicode text that has different
byte sequences on another machine or keyboard.

For security, the current encrypted v6 layout protects file contents and
names at rest, but AES-XTS does not authenticate encrypted sectors. The public
member manifest is authenticated with the password; data and metadata sectors
are checked for accidental corruption rather than deliberate tampering. A
single-member pool has no second member to witness an interrupted journal
commit and fails closed when it cannot establish a valid history. See
[encryption details](encryption.md) and [format recovery notes](v6-integrity.md).
