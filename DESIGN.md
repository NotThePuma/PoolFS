# PoolFS design

PoolFS is an experimental Windows filesystem implemented in native C++20.
WinFsp translates Windows file operations to PoolFS callbacks. The core manages
the file catalog, member placement, allocation, checksums, and recovery.

```text
Windows applications
        |
WinFsp native API callbacks
        |
PoolFS C++ core
        |
1–16 fixed-size member images on formatted host volumes
```

## Initial format

The first PoolFS product format is v6, identified internally by `POOLFS06`.
Each member has two checksummed headers with the pool ID, member identity, and
complete member manifest. All members are required to open the pool. Their
image lengths may differ but are fixed at formatting time.
Host paths and drive letters are absent from the manifest, so a complete set
can be relocated. A one-member v6 pool is permitted, but a torn journal record
cannot be compared against another member and must fail closed.
New encrypted v6 pools use header revision 3. The headers expose only member
discovery and password-derivation information; the journal, checkpoints,
checksums, and data sectors are encrypted with Windows CNG AES-256-XTS.
The password derives the XTS key and an HMAC key for the public headers.
See [docs/encryption.md](docs/encryption.md) for details and limits.

Each image reserves a 256 MiB metadata prefix, a data region, and paired
checksum-page slots at its tail. The core allocates 4 KiB data blocks and can
place one file across multiple members. It reuses blocks freed by deletion,
truncation, or overwrite. Writes copy changed data to new blocks, write an
inactive checksum page, flush them, then commit the new extents and selected
checksum-page root through a mirrored metadata journal. Alternating
checkpoints bound normal replay time. An offline scrub verifies live data and
selected checksum pages. The selected durable checkpoint retains its data
blocks and checksum-page slots until a newer checkpoint is flushed to every
member; writes rotate before reusing a protected page slot.

The WinFsp callback layer translates paths and file operations; allocation
and recovery stay in the core so they can be tested without a mounted drive.
The current callback path is serialized, and catalog commits are flushed before
acknowledgement. The v6 catalog gives each file and directory a stable ID,
stores basic Windows attributes and timestamps, and resolves nested paths
through directory entries. Open WinFsp handles use IDs so a rename can change
the path without changing the file identity. Replacing an existing file by
rename uses one mirrored journal record, so recovery sees the old or new
namespace and releases displaced extents through the checkpoint-aware allocator.

## Recovery boundary

On reopen, PoolFS requires a complete matching member set and a common valid
journal history. The offline checker can discard one cleanly interrupted final
mirrored commit and restore a writable common prefix. It uses durable undo
sidecars so an interrupted repair can be retried. It refuses damaged records,
larger divergence, and incomplete member sets. Neither the checker nor scrub
reconstructs missing or damaged file data.

The current format has no parity or mirroring. Keep independent backups while
physical power loss, controller-cache effects, torn writes,
missing-member behavior, and wider Windows compatibility remain unvalidated. The
mounted namespace is case-sensitive and has no persistent per-file ACLs or
named streams. Broader Windows application compatibility, member evacuation,
online expansion, and data reconstruction remain future work.

## Performance work

Native C++ and WinFsp's native API avoid another language runtime in the file
operation path. Measure sequential and random I/O, metadata operations, CPU,
and memory use before changing the allocation or journal paths. The benchmark
harness and dated measurements are in [bench/BENCHMARKS.md](bench/BENCHMARKS.md).
