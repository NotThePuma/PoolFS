# PoolFS benchmark harness

For a quick sequential test on a current mounted pool, use **Activity → Run
benchmark** in the manager or run:

```powershell
.\build\poolfsctl.exe benchmark Q: --size-mib 32 --json
```

The quick test writes and reads one temporary file in 1 MiB unbuffered chunks,
includes a final flush in write time, and verifies every byte read outside the
read timer. Backing image I/O may still be cached by the host filesystem. Use
the development harness below for the historical workloads and metrics.

Run `build.ps1` to build `poolfs-bench.exe`, then run it against a
mounted test pool with enough unallocated space:

```powershell
.\build\poolfs-bench.exe Q:\ baseline-001 8
```

Arguments are the mounted directory, a unique ASCII file prefix, and an
optional sequential-file size in MiB (default 8, maximum 32). The harness uses
`CREATE_NEW` and removes only files it created. V1–V3 do not reclaim deleted
space, so **use separate fresh images** for controlled baseline and
optimized comparisons on those formats. V4 reclaims file data after deletion.
The default v1 image run writes about 25.5 MiB total
across two members; 32 MiB images are sufficient for that workload. Raw v2
and v3 members require at least 256 MiB of usable space each.

One JSON object is written to stdout. The metrics are:

| Field | Measurement |
| --- | --- |
| `seq_write_mib_s` | 1 MiB sequential writes, plus one final `FlushFileBuffers` |
| `seq_read_mib_s` | 1 MiB noncached sequential reads with byte verification |
| `small_write_ms_mean` | 128 noncached 4 KiB overwrites of the sequential file |
| `clean_flush_ms_mean` | 64 `FlushFileBuffers` calls after the small writes |
| `overwrite_read_ms_median` | Median of nine noncached reads after 16 full 1 MiB overwrites |
| `create_ms_mean` | Mean of 40 empty-file creates |
| `delete_ms_mean` | Mean of 40 empty-file deletes |

The harness reports elapsed wall-clock time through `QueryPerformanceCounter`.
It is a workload comparison, not a claim about absolute physical-disk speed.
The dated results below used unencrypted member images; encrypted revision-3
pools need separate measurements before making a performance comparison.
Run baseline and optimized binaries on the same host, with the same image size,
member-image location, mount settings, and benchmark size. Close other I/O
workloads where practical. Because the pool's callbacks and storage are still
experimental, inspect benchmark errors and verify the pool contents after a
run before drawing conclusions.

## 2026-11-13 v6 image-backed functional run

Three 8 MiB harness runs used a fresh v6 pool mounted at `R:` from two 384 MiB
member images on `C:` NTFS. The raw runs are in
[results-v6-image-2026-11-13.json](results-v6-image-2026-11-13.json).

| Metric | Median of three runs |
| --- | ---: |
| Sequential write, MiB/s | 97.13 |
| Sequential read, MiB/s | 1,121.89 |
| 4 KiB overwrite, mean ms | 3.41 |
| Clean flush, mean ms | 0.03 |
| Overwritten read, median ms | 0.77 |
| Create, mean ms | 1.56 |
| Delete, mean ms | 1.66 |

Sequential write varied from 89.62 to 292.44 MiB/s across the three runs,
so this is a functional performance snapshot rather than a stable estimate.
The images share one host volume; these numbers do not compare v6 with the
older `T:`/`U:` pools or measure separate physical disks. After the harness
removed its files, the v6 pool reported all 267,386,880 data bytes free. A
150 MiB file then crossed the members and retained SHA-256
`83439FF603F9AAEC15759BB640972377BDFB9C2B2DFF6D593681755B7626C473`
after a forced mount restart and clean offline scrub.

## 2026-11-12 comparison

Baseline is commit `9288bfe`. The optimized version skips redundant
`Pool::Flush()` disk syncs, reads only visible extents after overwrites, and
suppresses expected missing-name logs. Three runs per version used fresh 32 MiB
member images on `C:` and the same Windows 11 Pro host. Baseline was mounted on
`Q:` and optimized on `R:`; the then-existing `P:` pool was not benchmarked or modified.
The raw results and run order are in [results-2026-11-12.json](results-2026-11-12.json).

| Metric (median of three runs) | Baseline | Optimized | Difference |
| --- | ---: | ---: | ---: |
| 8 MiB sequential write, MiB/s | 326.20 | 324.80 | -0.4% |
| 8 MiB sequential read, MiB/s | 1931.48 | 1869.47 | -3.2% |
| 4 KiB write, mean ms | 2.270 | 2.225 | -2.0% |
| Clean flush, mean ms | 0.373 | 0.032 | -91.4% |
| Read after 16 full overwrites, median ms | 3.145 | 0.352 | -88.8% |
| Create empty file, mean ms | 1.622 | 1.637 | +0.9% |
| Delete empty file, mean ms | 1.790 | 2.229 | +24.5% |

The flush and overwritten-read reductions were consistent across all three
pairs and match the changed code paths. The other measurements varied or changed
little; they do not establish a throughput improvement. The 40-file delete
result showed a slowdown, but a separate 400-file PowerShell probe on existing
test pools gave delete medians of 2.308 ms (baseline) and 1.766 ms (optimized),
the opposite direction. Treat delete performance as unresolved until a larger,
controlled benchmark isolates it.

These numbers include WinFsp and cached image-file storage on this host. The
noncached read flag applies at the PoolFS mount; backing image I/O still goes
through buffered C file operations. The figures should be used for relative
comparisons of this prototype, not as physical-disk throughput claims.

## 2026-11-12 file-backed NTFS VHDX pool

Two 512 MiB member files on separate 1 GiB NTFS partitions (`T:` on Disk 1,
`U:` on Disk 2) formed a v3 pool on `Q:` with 768 MiB of initial file-data
capacity. One 8 MiB benchmark run is recorded in
[results-file-backed-ntfs-vhdx-2026-11-12.json](results-file-backed-ntfs-vhdx-2026-11-12.json):

| Metric | File-backed v3 run |
| --- | ---: |
| Sequential write, MiB/s | 156.570 |
| Sequential read, MiB/s | 1712.182 |
| 4 KiB write, mean ms | 2.907 |
| Clean flush, mean ms | 0.033 |
| Overwritten read, median ms | 0.339 |
| Create, mean ms | 1.950 |
| Delete, mean ms | 2.059 |

A 385 MiB file spanned the two members and retained SHA-256
`CA89FDE61997BF068001B01B40C9EE682FFD8F15701CCE0556F1F224F547C887`
after remount. Files already on the NTFS host volumes stayed intact and were
not exposed through `Q:`. The raw `P:` pool remained mounted and its large
test file hash stayed unchanged. This is one functional benchmark run, not a
stable speed estimate. Its member size, backing NTFS/VHDX I/O, cache state,
and concurrent `P:` mount differ from the raw run below, so the two numbers
do not support a direct file-backed-versus-raw speed comparison.

## 2026-11-12 file-backed v4 VHDX pool

After replacing the earlier v3 test member files with v4, two 512 MiB image
members on the same `T:` and `U:` NTFS partitions formed a 512 MiB data pool
on `Q:`. One 8 MiB benchmark run is recorded in
[the v4 result](results-v4-file-backed-ntfs-vhdx-2026-11-12.json):

| Metric | File-backed v4 run |
| --- | ---: |
| Sequential write, MiB/s | 249.524 |
| Sequential read, MiB/s | 1594.451 |
| 4 KiB write, mean ms | 2.473 |
| Clean flush, mean ms | 0.031 |
| Overwritten read, median ms | 0.454 |
| Create, mean ms | 1.792 |
| Delete, mean ms | 1.722 |

The harness removed its files and `Q:` returned to its full 536,870,912 free
bytes. This one run checks for a gross performance regression; it does not
establish a speedup over v3 because the two runs were not controlled pairs.

## 2026-11-13 v5 checksum cost and read optimization

Three 8 MiB harness runs per build used `Q:` on the same two NTFS-backed VHDX
partitions. The v4 baseline used its existing 512 MiB member files. V5 used
new 384 MiB member files on those same host partitions, since the v4 images
remained in place. A 150 MiB cross-member file was present during the two
optimized v5 series. The raw results are in
[v4 baseline](results-v4-v5-integrity-comparison-2026-11-13.json),
[initial v5](results-v5-integrity-2026-11-13.json),
[batched CRC v5](results-v5-crc-optimized-2026-11-13.json), and
[direct-read v5](results-v5-direct-read-2026-11-13.json).
The JSON `version` field is the benchmark-output schema version.

| Median of three runs | V4 baseline | V5 initial | V5 batched CRC | V5 direct read |
| --- | ---: | ---: | ---: | ---: |
| Sequential write, MiB/s | 332.4 | 215.9 | 293.7 | 256.8 |
| Sequential read, MiB/s | 2,004.6 | 422.8 | 845.0 | 1,094.5 |
| 4 KiB overwrite, mean ms | 2.185 | 1.983 | 2.059 | 2.183 |
| Overwritten read, median ms | 0.445 | 2.203 | 1.010 | 0.743 |

Batching adjacent checksum reads, using a slicing-by-eight CRC-64/XZ lookup,
and reading aligned blocks directly into the caller buffer raised v5's
sequential read median about 2.6 times over the initial implementation. It
remains about 45% below the v4 baseline because v5 verifies every data block.
The member sizes, prior allocation history, and retained cross-member file
differ; these numbers indicate the local cost of integrity checking, not a
controlled v4-to-v5 speed ratio or a physical-disk guarantee. The first v4
sequential-write run was a 52.5 MiB/s outlier; the two later runs were 338.5
and 332.4 MiB/s.

## 2026-11-12 current v3 raw VHDX pool

The current `P:` pool combines two 4 GiB partitions on Disk 1 and one 6 GiB
partition on Disk 2. One 8 MiB benchmark run on this elevated raw v3 mount
produced [results-v3-raw-2026-11-12.json](results-v3-raw-2026-11-12.json):

| Metric | V3 raw run |
| --- | ---: |
| Sequential write, MiB/s | 337.607 |
| Sequential read, MiB/s | 1193.353 |
| 4 KiB write, mean ms | 2.226 |
| Clean flush, mean ms | 0.031 |
| Overwritten read, median ms | 0.713 |
| Create, mean ms | 2.448 |
| Delete, mean ms | 3.081 |

An ordinary user process also verified text, an 8,193-byte binary, and a
2,050 MiB file through `P:`. The large file's SHA-256 was
`0CD6080E406754F554314E062556F57ED047BD3CA57464861ABA0BD4CFF07B22`.
The files remained readable after a mount restart, and a raw check found a
nonzero first data block on all three members. See
the historical physical-member notes in the repository for the layout and
operation. This single benchmark run is a functional performance snapshot,
not a stable throughput estimate or a direct v2-versus-v3 comparison. VHDX
backing and host caches were not isolated, and deleted benchmark files do not
return their space to the pool.

## 2026-11-12 historical v2 raw VHDX pool

Two separate 10 GiB and 8 GiB VHDX disks were partitioned as GPT with one
dedicated PoolFS v2 member each. The benchmark ran on the former elevated `P:` raw
mount with an 8 MiB sequential file. A text file and an 8,193-byte binary file
were verified before and after a mount restart. The detailed provenance is in
[results-raw-vhdx-2026-11-12.json](results-raw-vhdx-2026-11-12.json).

| Metric | Run 1 (rounded summary) | Run 2 (JSON) |
| --- | ---: | ---: |
| Sequential write, MiB/s | 247.51 | 428.249 |
| Sequential read, MiB/s | 1155.30 | 1268.372 |
| 4 KiB write, mean ms | 2.15 | 1.704 |
| Clean flush, mean ms | 0.03 | 0.024 |
| Overwritten read, median ms | 0.69 | 0.801 |
| Create, mean ms | 1.47 | 1.809 |
| Delete, mean ms | 1.65 | 1.912 |

The first benchmark process succeeded and printed its rounded summary, but a
PowerShell stdout/stderr capture error discarded its JSON. The second process
produced full JSON. The sequential write difference between two runs is large,
so this is a functional and rough performance baseline, not a stable speed
estimate or an image-versus-raw comparison. The VHDX files' physical backing
and host cache behavior were not isolated. PoolFS still does not reclaim space
after the benchmark deletes its files. The v2 partitions were later replaced
with the three-member v3 layout; the live `P:` pool is v3.
