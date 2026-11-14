# Historical member-storage experiments

This is a historical record of pre-release v2–v5 experiments on image files
and dedicated GPT partitions. The current PoolFS product format is **v6 on
member image files**. Use the [PoolFS README](../README.md) and
[v6 integrity guide](v6-integrity.md) for current behavior. The
older raw-partition formats and commands are not part of the Windows ZIP.

## How the earlier formats differed

V2 used dedicated raw partitions on separate disks. V3 allowed 2–16 raw GPT
partitions or image files on formatted host volumes. V4 used image files and
added metadata checkpoints and reuse of space freed by deletion, truncation,
or overwrite. V5 added per-block file-data checksums. V6 moved checksum-page
updates to a copy-on-write design.

Across these experiments, capacity pooling did not mirror file data. Each
member was required to mount. An image-backed pool stored its files *inside*
member image files on ordinary host volumes; it did not merge pre-existing
files from those volumes. Host partitions remained under Windows control.

## Image-backed VHDX results (November 12–13, 2026)

Two NTFS host volumes each held a v4 member image. A 300 MiB random file
crossed the members, retained SHA-256
`A9158D099E616EFC5EE0D416420246A0D8B491236B90F00D58237BC4E8562C6C`
after a forced mount restart, and releasing it restored the pool's reported
free space. Sentinel files on the host volumes stayed intact and did not
appear in the PoolFS drive.

The same host volumes later held separate v5 member images. A 150 MiB file
crossed both members and retained SHA-256
`F2D3B5F702707CCCD93BA5CE040449DC2B9B17A8F1B3215D6C29581FB5866A15`
after forced mount restarts and a checksum-code optimization. Offline scrub
reported the file clean. The original v4 images were preserved during this
test. These are functional results on VHDX media, not evidence of physical
power-loss safety or physical-disk performance. See the
[dated benchmarks](../bench/BENCHMARKS.md) for measurements.

## Raw GPT partition results (November 12, 2026)

A three-member v3 VHDX pool used two partitions on one virtual disk and one
on another, giving 13,904 MiB of initial file-data capacity. An ordinary
process wrote and read small files and a 2,050 MiB file. The large file kept
SHA-256 `0CD6080E406754F554314E062556F57ED047BD3CA57464861ABA0BD4CFF07B22`
after a mount restart. A raw read found a nonzero first data block on all
three members, confirming placement across them. The
[v3 benchmark result](../bench/results-v3-raw-2026-11-12.json) records one
8 MiB run.

A separate two-member same-disk v3 trial wrote a 370 MiB file that crossed
both members. Its SHA-256 remained
`93C6FA478548A2748A0C226D959567FB068DF5BA8D7DDF38B146F0CBD7108216`
after a mount restart. These checks established basic operation and restart
persistence on the VHDX test layout. They did not establish power-loss safety,
corruption recovery, long-term stability, or physical-disk performance.

## Limits of the historical raw backend

The raw formatter worked inside existing GPT partitions; it did not create,
resize, or remove partitions. It verified member identities and extents before
opening them, and rejected the active Windows disk, EFI and page-file disks,
and overlapping members. Its metadata blankness check covered only the first
128 MiB of each selected partition. That check could not prove the entire
partition was empty, and formatting several partitions was not atomic if I/O
failed partway through.

V3 had no free-space reclamation or file-data scrub. A missing or failed
member could make the pool unavailable, and the raw backend did not mirror
file data. The [Windows ZIP](https://downloads.poolfs.com/poolfs-windows-x64-v6.zip)
contains the current image-backed v6 tools instead.
