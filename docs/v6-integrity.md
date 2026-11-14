# Initial format: copy-on-write checksums

PoolFS starts with the v6 on-disk format. It uses a mirrored metadata journal,
alternating checkpoints, and copy-on-write file data and checksum pages.
The current experimental v6 header accepts unencrypted layout revision 2 and
password-encrypted layout revision 3; earlier local v6 test images are rejected.
Format new member images; changing an existing image header cannot create or
convert a pool.
The pool still requires all members and has no data redundancy. V6 accepts
one to sixteen members; a one-member pool has no second journal copy to
resolve an interrupted commit and fails closed if its journal is damaged. Members are
fixed-size image files of at least 384 MiB each and may have different sizes.
The first 256 MiB of each member
is reserved for metadata. File data starts there, and paired checksum-page
slots are reserved at the tail of each member.

On host volumes with enough free space, use new member paths:

```powershell
.\build\poolfs-format.exe 384 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
.\build\poolfs-mount.exe R: 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
```

For different sizes, give each new member its own size/path pair:

```powershell
.\build\poolfs-format.exe --sized 384 'X:\PoolFS\member.img' 768 'Y:\PoolFS\member.img'
.\build\poolfs-mount.exe R: 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
```

The image sizes are part of the pool manifest. Do not resize a member file with
Windows tools. To grow an unmounted pool, use **Grow images** in the manager or
`poolfs-format --grow-copy` with every member. PoolFS writes new copies,
relocates the checksum pages, updates the complete manifest, and verifies the
result. Original member images remain intact. Shrinking is not supported.

## Checksum placement

The allocation unit is 4 KiB. Each physical group of 512 data blocks (2 MiB)
has two dedicated 4 KiB checksum-page slots at the member tail. Each page
contains 512 eight-byte CRC-64/XZ cells. A sparse root map in the journal and
checkpoint selects one slot for each used group and stores that page's CRC-64/XZ
hash. Data-block `i` uses cell `i % 512` in the selected page of group
`floor(i / 512)`. New groups start in slot 0; later updates copy the group's
checksums to the other slot and change only the new data blocks' cells.

For each member of size `M`, metadata prefix `D = 256 MiB`, and `B = 4096`, the maximum
data-block count `N` satisfies
`N*B + 2*ceil(N/512)*B <= M-D`. The checksum-page region begins at
`M - 2*ceil(N/512)*B`. It reserves roughly 0.39% of the post-metadata member
space. The pages are reserved even for groups that have not yet been written.

For a write, PoolFS allocates new data blocks, writes them and the replacement
checksum page, flushes both, then commits the new file extents and selected
page slot through the mirrored journal. A one-block overwrite writes one new
data block and one replacement checksum page; a large sequential write can
amortize a page across 2 MiB of data. Each write leaves the currently selected
page intact until its new root is committed. After more writes to the same
group, the selected durable checkpoint still names its original data blocks
and checksum-page slots. PoolFS keeps those blocks out of free space and
rotates to a new checkpoint before overwriting a page slot named by the old
one. A checkpoint can therefore be reopened after discarding later journal
records without reading overwritten file data or checksum pages. Reclaiming
blocks held only by an older checkpoint can require a checkpoint rotation.
An entirely full pool still needs a free data block for a copy-on-write
overwrite. A damaged journal remains a fail-closed condition; checkpoint
retention does not reconstruct its later acknowledged operations.

This design detects accidental damage to live data blocks and checksum pages.
It cannot reconstruct damaged bytes or a missing member. Journal and checkpoint
checksums are still prototype-grade, and the fixed checkpoint slots can fill
on highly fragmented workloads. Checksum root entries remain for groups that
were used and then emptied, up to the fixed number of physical groups. If an
obsolete page is damaged, reuse of that group starts a fresh page only after
PoolFS confirms that no live extent still depends on its checksums. Simulated
process crashes and torn writes exercise specific failure cases; they do not
prove recovery after host power loss, controller-cache failure, or all NTFS
failure modes. Keep independent backups of PoolFS data you care about and
validate recovery for your workload.

## Windows delete limitation

The WinFsp `Cleanup` callback commits a delete to the mirrored journal before
`Close`, so a successful commit survives a mount-process crash. `Cleanup` has
no return status. If its durable commit fails, PoolFS logs the error and stops
further writes, but Windows cannot receive that failure from `Cleanup`. Reopen
the complete member set with the offline checker before trusting the result of
such a delete. Operations on a handle after its successful delete cleanup
return `DELETE_PENDING` until that handle closes.

## Atomic file replacement

Renaming a regular file over another existing regular file uses one mirrored
journal operation. Recovery sees either the old source and destination or the
new destination; the source file keeps its stable ID, and the displaced target
ID and extents are retired. Checkpoint retention protects any displaced blocks
still named by the selected durable checkpoint. A directory or read-only
destination cannot be replaced. WinFsp rejects the operation before the
callback if the destination has an open handle.

The unencrypted header layout remains revision 2. Earlier revision-2 executables do not
understand replacement journal records and may refuse to reopen a pool after
the first replacement. Keep the current mount, checker, and scrubber binaries
together for pools that have used this operation.

Encrypted revision-3 pools use the same logical journal, checkpoint, data, and
checksum structure behind a 4 KiB-sector cipher. The offline checker and
scrubber request the password before inspecting those structures. See
[encryption details](encryption.md) for the protection boundary.

## Offline scrub

Unmount the PoolFS drive before running the read-only scrub against the complete
member set:

```powershell
.\build\poolfs-scrub.exe 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
```

The scrubber opens members exclusively, validates live data and selected
checksum pages, and reports the affected file, member index, and offsets. It
counts every mismatch while printing at most the first 1,000 locations. Exit
code 0 means no detected damage, 2 means damage or invalid metadata, and 1
means a usage or access error, including a mounted member. Scrub is read-only
and cannot reconstruct damaged bytes.

## Interrupted final commit recovery

With the PoolFS mount stopped, inspect the complete member set:

```powershell
.\build\poolfs-check.exe 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
```

The checker identifies the format from the headers. Exit code 0 means clean, 2 means one
cleanly interrupted final mirrored journal commit, 3 means corrupt or
incomplete history, and 1 means usage or I/O failure. Only in the exit-code-2
case, this command can discard the uncertain final operation and restore a
writable common prefix:

```powershell
.\build\poolfs-check.exe --repair-final-tail 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
```

Before changing a journal slot, the checker writes a durable undo sidecar
beside each affected member. If repair is interrupted, keep the sidecars and
rerun the same command with the complete member set. Mount and scrub refuse
members with a pending sidecar. The checker refuses damaged or divergent
records and cannot repair file data, checksum pages, or a missing member. The
repair test covers process-exit interruption and retry; it does not establish
physical power-loss safety.
