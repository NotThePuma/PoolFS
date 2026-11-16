# PoolFS on GPT partitions and formatted volumes

This is a historical record of disposable pre-release partition and image
experiments. The initial PoolFS product format is v6 on image files. Use the
[README](../README.md) for current format and mount commands; the raw and older
format commands below require their historical binaries.

PoolFS v3 combines 2–16 dedicated raw GPT partitions or image files on
formatted volumes into one WinFsp drive letter. V4 supports image files and
adds metadata checkpoints and data-space reuse. V5 adds per-block file-data
checksums; v6 makes checksum-page updates copy-on-write. New v3-v6 image pools
can assign a different fixed size to each member with the `--v3-sized` through
`--v6-sized` formatter options. The count and identities are fixed when the
pool is formatted; all members are required to
mount. This is capacity pooling, not mirroring. Use disposable data.

With raw members, PoolFS owns bytes inside each selected partition. Windows
still owns each disk's GPT. The raw formatter does not create, resize, or
remove partitions. Raw members may be on the same disk or different disks.

## File-backed pools on NTFS or FAT32 hosts

Each member can instead be a fixed-size file on a normal Windows volume. For
example, an NTFS partition on external Disk 1 and another on external Disk 2
can each hold one PoolFS member file. The host partitions remain NTFS and keep
their own files. The PoolFS drive shows files stored *inside* the member files;
it does not merge files already present on the NTFS volumes. A union drive for
existing files would need a separate directory and metadata layer.

The historical v4 test pool on `T:` and `U:` was formatted with:

~~~powershell
New-Item -ItemType Directory -Path 'T:\PoolFS', 'U:\PoolFS' -Force | Out-Null
.\build-v4\poolfs-format.exe --v4 512 'T:\PoolFS\member-v4.img' 'U:\PoolFS\member-v4.img'
$env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
.\build-v4\poolfs-mount.exe --images-v4 Q: 'T:\PoolFS\member-v4.img' 'U:\PoolFS\member-v4.img'
~~~

Do not rerun the formatter on these existing member files. V4 accepts 2–16
distinct new file paths and a size in MiB of at least 512; all members get
that fixed size. It refuses to overwrite existing member files. The files do
not grow automatically. V4 reuses data blocks freed by deletion, truncation,
or overwrite; v3 does not. The image mount uses a drive letter in the mounting
user's logon session, so a writable host
directory is enough; it does not open or lock the underlying NTFS volumes as
raw devices. Keep the mount process running and unmount before removing a host
volume. FAT32 limits one file to roughly 4 GiB, so larger FAT32-backed members
would require a chunked-file backend. [Microsoft's file-system comparison](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison/)
lists the different NTFS, exFAT, and FAT32 limits.

### Current file-backed NTFS VHDX pool

On 2026-11-12, one 1 GiB NTFS partition was added in the free tail of each
disposable VHDX disk. They host separate 512 MiB `PoolFS\member-v4.img` files
and, as of 2026-11-13, 384 MiB `PoolFS\member-v5.img` files.
The raw partitions and `P:` pool were preserved. The one-time
`tools/reconfigure-vhdx-ntfs.ps1` helper was used to create these host
partitions; do not rerun it on the completed layout.

| Host | Disk and partition | GPT PartitionId | NTFS label |
| --- | --- | --- | --- |
| `T:` | Disk 1, partition 4 | `{1114cc44-83b4-4370-a30c-5b75113a54c3}` | `PoolFS-Host1` |
| `U:` | Disk 2, partition 3 | `{f1870e12-e900-4ba8-8ff2-0f0a8bfbc4cd}` | `PoolFS-Host2` |

The two v4 member files form a 512 MiB file-data pool when mounted. The new
v5 pair currently mounts as `Q:` with 267,911,168 bytes of data capacity in
the mounting user's logon session. To remount this **already formatted** v5
pool in an ordinary PowerShell window, run:

~~~powershell
.\tools\mount-ntfs-vhdx-pool.ps1
~~~

The helper checks the exact disk, partition, NTFS label, volume path, and
member-file size before mounting. It performs no formatting. Keep its process
running, and unmount before disconnecting either VHDX disk.

The v5 pair was created without removing the historical v4 images. A 150 MiB
file exceeded either v5 member's roughly 127.75 MiB data area and spanned
both. SHA-256 stayed
`F2D3B5F702707CCCD93BA5CE040449DC2B9B17A8F1B3215D6C29581FB5866A15`
after forced mount restarts and a checksum-code optimization. The offline
`poolfs-scrub` reported one clean file. The original v4 images remain present
and unmounted. The raw v3 partitions also remain configured, although `P:` is
currently unmounted. See [the integrity benchmark](../bench/BENCHMARKS.md)
for the measured read cost.

A 300 MiB random file exceeded either v4 member's 256 MiB file-data capacity,
so it spanned both. Its SHA-256 before and after a forced mount restart was
`A9158D099E616EFC5EE0D416420246A0D8B491236B90F00D58237BC4E8562C6C`.
Deleting it restored `Q:` to 536,870,912 free bytes. Sentinel files on `T:`
and `U:` remained intact and did not appear in `Q:`.
The 2,149,580,800-byte file in the separate raw `P:` pool retained SHA-256
`0CD6080E406754F554314E062556F57ED047BD3CA57464861ABA0BD4CFF07B22`
after the NTFS partitions were added. The earlier v3 file-backed `Q:` pool
used `PoolFS\member.img` files and passed a 385 MiB cross-member hash check;
those disposable member files were removed before v4 formatting. Its single
8 MiB benchmark is preserved in [the v3 file-backed result](../bench/results-file-backed-ntfs-vhdx-2026-11-12.json).
These results verify basic operation and restart persistence on the VHDX test
media, not power-loss safety or physical-disk performance.

## Current raw three-partition VHDX pool

On 2026-11-12, the disposable test pool on `P:` uses these exact members:

| Disk | VHDX size | Windows UniqueId | GPT disk GUID | PoolFS partitions |
| --- | ---: | --- | --- | --- |
| 1 | 10 GiB | `60022480AB8EFFB1ADF43DD1FF8F83A9` | `{82f01061-6453-40fe-82b4-a3204208f02f}` | Two 4 GiB partitions |
| 2 | 8 GiB | `60022480542A978B5E84E922C041AF43` | `{b0644b17-4e34-4604-a371-d2900c0dfbb7}` | One 6 GiB partition |

| Disk and partition | GPT PartitionId | Volume path |
| --- | --- | --- |
| Disk 1, partition 2 | `{4f42f97a-f0c1-4b12-82ae-0a9b72f92c08}` | `\\?\Volume{4f42f97a-f0c1-4b12-82ae-0a9b72f92c08}` |
| Disk 1, partition 3 | `{2ffed969-cbbc-464e-b835-3e8544623e70}` | `\\?\Volume{2ffed969-cbbc-464e-b835-3e8544623e70}` |
| Disk 2, partition 2 | `{d746f67b-f9b0-4777-959c-7b8bb19b724e}` | `\\?\Volume{d746f67b-f9b0-4777-959c-7b8bb19b724e}` |

The first two members start at 512 MiB and 4.5 GiB on Disk 1; the third
starts at 512 MiB on Disk 2. Windows-created Microsoft Reserved partitions
remain outside the PoolFS members. Disk 0, which contains Windows, was not
changed. PoolFS reserves the final 16 MiB of each selected partition and
starts file data at 128 MiB within each member. This layout provides
13,904 MiB of initial file-data capacity across the three members.

To remount this **already formatted** pool after a reboot, open an elevated
PowerShell window in the repository and run:

~~~powershell
.\tools\mount-vhdx-v3-pool.ps1
~~~

Keep the process running. Press Ctrl+C to unmount before removing a VHDX.
The script verifies the current disk UniqueIds, GPT disk GUIDs, partition
GUIDs, sizes, offsets, and volume paths before invoking the raw v3 mount.
The mount uses WinFsp's global `\\.\P:` Mount Manager form, so ordinary
Windows processes can access `P:` while the elevated process holds the locks.
The script performs no formatting or repartitioning. Do not rerun the
one-time `reconfigure-vhdx-v3.ps1` helper or `--erase` on this pool.

## Validation completed on this raw pool

- The v1, v2, and v3 image-backed core test suites passed.
- An ordinary user process wrote and read a small text file, an 8,193-byte
  binary file, and a 2,050 MiB file through `P:`. The large file's SHA-256
  was `0CD6080E406754F554314E062556F57ED047BD3CA57464861ABA0BD4CFF07B22`.
- A raw read found a nonzero first data block on each of the three members,
  confirming placement across the partitions.
- All files remained readable after stopping and restarting the mount.
- An 8 MiB benchmark run is recorded in
  [`bench/results-v3-raw-2026-11-12.json`](../bench/results-v3-raw-2026-11-12.json).

These checks establish basic operation and restart persistence on the VHDX
pool. They do not establish power-loss safety, corruption recovery, long-term
stability, or physical-disk performance.

### Temporary two-member same-disk test (2026-11-12)

Two temporary 512 MiB partitions on Disk 1 formed a separate raw v3 pool
mounted globally on `Q:`. A 370 MiB file exceeded either member's 368 MiB
usable file-data capacity and therefore spanned both. Its SHA-256 was
`93C6FA478548A2748A0C226D959567FB068DF5BA8D7DDF38B146F0CBD7108216`.
After a mount restart, the same hash and a small text file were verified.
Temporary Disk 1 partitions 4 and 5 (PartitionIds
`{7134e31a-cedf-4b22-8238-f278e1ddaf06}` and
`{d2dbbd35-505d-4865-89a1-dc11760ea41f}`) were then removed. `Q:` now hosts
the separate file-backed NTFS test pool. The permanent three-member `P:` pool
was remounted. Its small files
and 2,050 MiB file persisted, with the large file's original SHA-256 intact.

## Preparing a new disposable pool

1. Inspect the target disks and partitions with `tools/inspect-storage.ps1`.
   Identify each disk by a stable Windows UniqueId and its GPT disk GUID.
   Do not select the Windows, EFI, or page-file disk. Confirm the selected
   partitions contain no data that must be kept.
2. As administrator, create 2–16 dedicated GPT basic-data partitions. They
   may share a disk, but must have distinct GPT PartitionIds and nonoverlapping
   physical extents. Leave them unformatted and without normal drive letters.
   Record each exact volume GUID path, GPT disk GUID, and GPT PartitionId.
   The older `prepare-raw-members.ps1` provisions only the historical
   two-disk layout; it is not a general v3 partitioning tool.
3. Run the read-only inspection, review every listed target, then run the
   explicit destructive format command. Add one volume path, disk GUID, and
   partition GUID triple for each member:

   ~~~powershell
   $members = @(
       '<volume-1>'
       '<disk-guid-1>'
       '<partition-guid-1>'
       '<volume-2>'
       '<disk-guid-2>'
       '<partition-guid-2>'
   )
   .\build\poolfs-format-raw.exe --v3 --inspect @members
   .\build\poolfs-format-raw.exe --v3 --erase @members
   ~~~

   Add further triples inside `$members` for a larger pool. Volume paths
   have the form `\\?\Volume{GUID}` without a final
   backslash. The formatter locks all volumes and rechecks identities before
   writing. V3 scans the first 128 MiB metadata region of every member and
   refuses to format if that region contains nonzero data.
4. Mount with the same member set from an elevated shell. The drive letter
   comes immediately after `--raw-v3`:

   ~~~powershell
   .\build\poolfs-mount.exe --raw-v3 P: @members
   ~~~

   If the WinFsp runtime DLL is outside `PATH`, add its installed `bin`
   directory first. Keep the mount process running and unmount before
   detaching any member.

The raw backend opens each exact volume GUID path, takes an exclusive volume
lock, verifies the expected GPT disk and partition GUIDs, checks the selected
GPT extent against the volume extent, and rejects overlapping members on one
disk. It rejects the active Windows disk, disks with an EFI partition, and
configured page-file disks. It requires 4 KiB compatible sector alignment
and bounds I/O by each partition's usable length.

## Format and recovery limits

V3 headers contain a shared pool ID and a manifest of member IDs and
capacities. The append-only catalog journal is mirrored on every member.
File data may span members, but is not mirrored. Writes flush data before
committing the catalog record. Mount requires all members and replays their
shared valid journal prefix; a divergent tail opens read-only pending an
offline repair tool.

There is no free-space reclamation or data scrub. A missing or failed member
can make the pool unavailable and can lose files placed on it. The formatter's
128 MiB metadata-region check is a limited blankness guard; it does not prove
the entire partition is empty. Formatting across multiple partitions is not
atomic if I/O fails mid-operation. The WinFsp
compatibility suite and power-loss testing remain open. Keep this prototype
on disposable media.

## Historical v2 path

The previous `P:` pool used one dedicated basic-data partition on each of
the two VHDX disks. V2 required separate disks and exactly one basic-data
partition per disk. Its two-member raw command syntax remains available:

~~~powershell
.\build\poolfs-format-raw.exe '<volume-1>' '<disk-guid-1>' '<volume-2>' '<disk-guid-2>' --inspect
.\build\poolfs-format-raw.exe '<volume-1>' '<disk-guid-1>' '<volume-2>' '<disk-guid-2>' --erase
.\build\poolfs-mount.exe --raw '<volume-1>' '<disk-guid-1>' '<volume-2>' '<disk-guid-2>' P:
~~~

The earlier v2 measurements are retained in
[`bench/BENCHMARKS.md`](../bench/BENCHMARKS.md). The old member partitions
were removed during the v3 reconfiguration; `P:` now uses v3.

## Windows API references

- [CreateFile raw disk and volume handles](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)
- [FSCTL_LOCK_VOLUME](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume)
- [IOCTL_DISK_GET_LENGTH_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_disk_get_length_info)
- [IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddvol/ni-ntddvol-ioctl_volume_get_volume_disk_extents)
- [STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-storage_access_alignment_descriptor)
- [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
- [WinFsp global Mount Manager drive syntax](https://github.com/winfsp/winfsp/issues/410)
- [FSCTL_ALLOW_EXTENDED_DASD_IO](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_allow_extended_dasd_io)
