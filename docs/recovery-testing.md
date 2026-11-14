# Process-crash and recovery test record

These dated results include pre-release formats. The initial PoolFS product
format is v6; use the [README](../README.md) for current commands.

For an abrupt crash of this Windows host using the attached disposable test
volumes, see the [on-host crash probe](host-crash-testing.md). It records ACKs
on `C:` and does not trigger the crash itself.

## Hyper-V guest reset trial (2026-11-13)

A fresh v6 pool used 384 MiB and 512 MiB member images on `T:` and `U:` and
mounted at `X:`. Run `cc60d60d6ad846199264348cc6ee6f66` wrote a 4 KiB
deterministic probe, flushed each write, and flushed its ACK to a separate
`C:` log. The writer was active when the user was instructed to reset the VM
from the Hyper-V host. On return, the guest had booted at 12:32:06 Eastern.
Windows recorded Kernel-Power event 41 and EventLog event 6008, with no orderly
shutdown event in the inspected
window; [Microsoft describes these events as an unclean restart](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/troubleshoot-unexpected-reboots-system-event-logs).
The VM checkpoint was not restored before verification.

The last complete ACK was generation 378. After remount, the probe was exactly
generation 378 with SHA-256
`68380D704CD249774998BB73E562D7D70B49305C86903FA54BF817A7A885F727`.
The offline scrub scanned the live file and reported `CLEAN`; the checker saw
both members, 380 common journal records, and reported `CLEAN` with no repair.
The older `Q:`, `R:`, and `S:` test mounts were restored. The known SHA-256 of
`Q:\cross-member-v5.bin` remained
`F2D3B5F702707CCCD93BA5CE040449DC2B9B17A8F1B3215D6C29581FB5866A15`.

This is one abrupt guest reset with the hypervisor and storage host still
powered. It does not test physical host power loss, volatile device-cache loss,
or other interruption timings.

## External power-cut protocol for v6

`tests/power_cut_oracle.ps1` provides a one-write-at-a-time durability probe
for a **disposable VM and pool**. Initialize its 4 KiB file, then run the
write loop on the mounted PoolFS drive:

```powershell
.\tests\power_cut_oracle.ps1 -Phase Initialize -PoolFile V:\power-cut.bin
.\tests\power_cut_oracle.ps1 -Phase WriteLoop -PoolFile V:\power-cut.bin
```

An external controller must record the latest complete `ACK generation=N`
line and abruptly turn off the VM without asking the guest to shut down.
After booting and remounting the complete pool, verify with that recorded
generation, then unmount and run `poolfs-scrub` and `poolfs-check`:

```powershell
.\tests\power_cut_oracle.ps1 -Phase Verify -PoolFile V:\power-cut.bin -AcknowledgedGeneration N
```

The probe accepts generation N or the one complete write that may have
committed before its ACK was observed. Any other bytes fail verification.
Repeat the power cut at varied times and on separate host disks. The local
machine has no VM power-control tool available, so this is a prepared protocol,
not a physical power-loss result.

## V4 process-crash testing

`poolfs-v4-crash-tests.exe` runs a parent/child sweep against fresh disposable
512 MiB image members. The child sets `POOLFS_TEST_V4_CRASH_AT=before:N` or
`after:N` and exits immediately at the selected v4 member write or flush call.
For each boundary, the parent restores a fresh image pair, restarts PoolFS,
and checks an already acknowledged anchor plus the operation under test.

On 2026-11-12, the full sweep passed in 540.6 seconds:

| Operation | I/O boundaries | Old state | New state | Failed closed |
| --- | ---: | ---: | ---: | ---: |
| Create | 6 | 7 | 5 | 0 |
| Write | 8 | 11 | 5 | 0 |
| Unaligned copy-on-write overwrite | 8 | 11 | 5 | 0 |
| Delete | 6 | 7 | 5 | 0 |
| Checkpoint rotation | 26 | 47 | 5 | 0 |

There were 54 distinct I/O boundaries, tested both before and after the call.
Every successful reopen showed exactly the old or new target state and the
acknowledged anchor remained intact. The fully flushed final boundary showed
the new state. The harness cleans up its temporary member images.

This exercises abrupt **PoolFS process exit** and resulting member-file
contents. It does not simulate host power loss, controller cache failure,
torn physical-sector writes, or silent file-data corruption. Those require
separate validation before storing important data.

## Offline checker retry

`tests/check_repair.ps1` creates three fresh v4 members and interrupts a
mirrored commit after two members have flushed its final record. The checker
reports `INTERRUPTED_FINAL_COMMIT` and leaves the members untouched. The test
then exits the checker after it has zeroed only the first of those two records.
Durable undo sidecars remain, so read-only inspection reports
`PENDING_REPAIR`. Rerunning repair restores the original records, repeats the
narrow final-record discard, removes the sidecars, and reports `CLEAN`.

The three-member retry test passed on 2026-11-12. A separate rotated
generation-2 checkpoint fixture also reported `CLEAN`. The checker accepts
only complete matching member sets and performs structural checks; it cannot
verify or reconstruct file-data bytes.

## V5 integrity and process crashes (2026-11-13)

The v5 image tests passed normal reads, unaligned overwrite, truncate and
regrow, reversed member-path order, and reopening. Flipping one file-data byte
raised `Corrupt` on read while another file remained readable. Flipping its
checksum cell raised the same error. Offline read-only scrub identified the
damaged file and member; a clean pool returned no issues. Scrub also refuses
to run while the member images are mounted.

The final optimized v5 binary passed a write and overwrite process-crash
sweep against fresh 512 MiB member images. Each operation had nine distinct
write/flush boundaries tested immediately before and after the call. In each
18-trial sweep, 13 reopened to the old state and five to the new state; none
returned wrong bytes or lost the previously acknowledged anchor. The harness
uses the same `POOLFS_TEST_V4_CRASH_AT` hook for v5.

The live v5 pool on `T:` and `U:` also passed a clean offline scrub of its
150 MiB cross-member file and preserved SHA-256 across forced mount restarts.
These tests do not simulate host power loss, torn NTFS sectors, or controller
cache failure. V5's checksum table is updated in place and can damage
neighboring live checksum cells if its containing sector tears.

## V6 copy-on-write checksum tests (2026-11-13)

The current v6 layout pins file-data extents and checksum-page slots named by
its selected durable checkpoint. Fresh fixtures discarded all later journal
records after deleting checkpoint data and after repeated checksum-page
updates; reopening the checkpoint returned the original bytes and scrubbed
clean. A near-full fixture rotated the checkpoint before reusing retained
space. A pre-retention v6 header is rejected instead of being read under the
new layout. These tests exercise process-level failure and deliberate journal
discard, not host power loss.

Fresh 512 MiB v6 member images passed normal reads and writes, reversed-path
reopen, repeated checksum-root toggles, truncation and regrowth, and data-space
reclamation. Separate data-block and selected-checksum-page byte flips were
detected on read and by the read-only offline scrub. A child process wrote and
flushed half of an inactive checksum page, then exited before committing a
journal record. After four acknowledged small overwrites had alternated both
members' first checksum groups back to slot 0, recovery retained the last
acknowledged target and anchor; scrub reported clean.

The v6 process-crash sweep passed write, overwrite, and checkpoint-rotation
scenarios. Write and overwrite each exercised nine distinct I/O boundaries:
13 trials reopened to the old state and five to the new state. Rotation
exercised 26 boundaries: 47 old and five new. No trial returned wrong bytes or
lost its acknowledged anchor. These are process-exit and deliberate partial
page-write simulations. They do not establish physical power-loss safety,
controller-cache behavior, or recovery after damaged journal history.

## V6 offline final-tail repair (2026-11-13)

The v6 checker test used three fresh 384 MiB images. An injected process exit
after the second member's final journal flush left two copies of the new
record and one old end marker. Read-only inspection reported
`INTERRUPTED_FINAL_COMMIT`. Repair saved durable undo sidecars, then another
injected exit stopped it after zeroing the first record. Inspection reported
`PENDING_REPAIR`; both writable open and read-only scrub refused the sidecars.
Rerunning repair restored the original records from the sidecars, discarded
only the uncertain final operation, and removed the sidecars. V6 reopened
writable, accepted a new file, returned its bytes, and scrubbed clean.

Separate fixtures with a damaged journal record and divergent valid records
were rejected by both inspection and repair without creating sidecars. The
existing v4 repair test still passed. This validates the specific process-exit
and retry path; it does not cover physical power loss or reconstruct damaged
data.

The mixed-size v6 fixture repeated the interrupted-commit and repair-retry
sequence with three members of 384, 512, and 640 MiB. It passed writable
reopen and clean scrub. Separate mixed-size format/open tests passed for v3,
v4, and v5; the v6 data test wrote and reread a 300 MiB file across 384 and
512 MiB members, then reclaimed its space. The mounted `S:` test retained its
SHA-256 after a forced restart and passed offline check and scrub. These tests
exercise different member lengths without changing the on-disk format.

## V6 atomic replacement (2026-11-13)

The v6 integrity suite replaced an existing file in one journal operation,
reopened the pool, and checked that the source bytes and stable file ID moved
to the destination while the old destination ID disappeared. A selected
checkpoint retained the old target's data when later journal records were
discarded. A separate fixture forced a checkpoint after replacement and
reopened the new snapshot. Both scrubbed clean.

The focused process-crash sweep interrupted replacement before and after each
of six member write/flush boundaries. Across 12 trials, seven reopened with
the old source and destination and five with the new destination. None lost
the acknowledged anchor, returned a mixed state, or failed closed.

The mounted WinFsp test exercised `MoveFileExW` replacement with an open source
handle, an open destination, read-only and directory targets, same-name rename,
and an absent destination. Its 152 MiB cross-member file retained SHA-256
after remount; the final scrub and checker reported `CLEAN`. The open-handle
results matched a disposable NTFS baseline. These tests cover process exits
and one mounted workload, not a new abrupt VM reset during replacement.
