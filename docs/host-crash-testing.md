# On-host crash probe

`tests/host_crash_probe.ps1` prepares a fresh v6 pool on the disposable `T:`
and `U:` host partitions, mounts it at `X:`, and records each acknowledged
4 KiB overwrite in a flushed log under `work\host-crash` on `C:`. It never
crashes or reboots Windows. It does not delete its images or run state.

The test is an **abrupt host OS crash** experiment, not proof of physical
power-loss or controller-cache safety. [Windows shutdown flushes file
buffers](https://learn.microsoft.com/en-us/windows/win32/shutdown/how-to-shut-down-the-system),
so a normal `Restart-Computer -Force` is not the same test. An intentional
Windows crash can be triggered separately by the
user with a tool such as [Microsoft Sysinternals NotMyFault](https://learn.microsoft.com/en-us/sysinternals/downloads/notmyfault),
which requires elevation. A manual hardware reset also interrupts the host.
Either action affects the entire machine, including unrelated applications and
the Windows system volume. The `Q:`, `R:`, and `S:` PoolFS mounts currently in
use will disappear and are not restored automatically.

## Prepare and write

From the repository directory, with both host volumes attached and `X:` free:

```powershell
.\tests\host_crash_probe.ps1 -Phase Prepare
```

Save the printed `run_id`. The script creates uniquely named 384 MiB and
512 MiB member images at `T:\PoolFS\host-crash-<run_id>-a.img` and
`U:\PoolFS\host-crash-<run_id>-b.img`, plus `<run_id>.json` run state and
`<run_id>.acks` on `C:`. The state includes exact member paths, the binary
directory, and a read-only snapshot of existing PoolFS mount command lines for
manual restoration if needed. Preparation refuses existing image names and an
occupied `X:`.

Start the writer in a separate PowerShell window, replacing the ID:

```powershell
.\tests\host_crash_probe.ps1 -Phase WriteLoop -RunId <run_id> -PauseMilliseconds 0
```

Each `ACK generation=N` line appears only after the probe write and its
`FileStream.Flush(true)` succeed and the ACK record has been flushed to the
separate `C:` log. A photo of the latest displayed ACK provides an independent
record if the host log is lost. Initiate the crash **separately** while this
loop runs. The script deliberately contains no crash command.

The Codex session and both PowerShell windows will be interrupted. After
Windows starts again, reopen this chat or run verification manually. This is
not an unattended test.

## Verify after reboot

Confirm that `T:` and `U:` and their exact member images are attached. From
the same repository directory:

```powershell
.\tests\host_crash_probe.ps1 -Phase Verify -RunId <run_id>
```

The verifier refuses missing or resized members. It reads the last complete,
validated ACK record from `C:`, remounts the pool on `X:`, and accepts either
that generation or the one next complete generation that could have committed
before its ACK. It then unmounts `X:` and runs offline scrub and check. If a
photo recorded a newer completed ACK, or the host log is missing or corrupt,
pass its number with `-ObservedAcknowledgedGeneration N` and retain both
pieces of evidence when available.

The ACK log is on the same host, so a physical host power loss could also lose
or tear its newest record. The verifier ignores an incomplete final record and
rejects malformed complete records. No result should be inferred if the run
state or both member images are unavailable. Repeating the experiment with
different crash timing provides more coverage, but does not prove that a
physical disk honors cache flushes.

## Safe dry run

The three phases can be checked without crashing Windows:

```powershell
.\tests\host_crash_probe.ps1 -Phase Prepare
.\tests\host_crash_probe.ps1 -Phase WriteLoop -RunId <run_id> -Count 3
.\site\dist\downloads\poolfsctl.exe unmount X:
.\tests\host_crash_probe.ps1 -Phase Verify -RunId <run_id>
```

The dry run verifies the script and recovery path, not abrupt crash behavior.
The exact images and run-state files remain for inspection afterward.
To use that same pool for a later crash trial, remount `X:` with the two
member paths printed by Prepare and start `WriteLoop` again with the same
`run_id`; it resumes after the last complete ACK. This avoids allocating a
second image pair on the small `T:` and `U:` test partitions. In a new
PowerShell window, ensure the WinFsp runtime is on `PATH` before invoking the
controller directly:

```powershell
$env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
.\site\dist\downloads\poolfsctl.exe mount X: `
    'T:\PoolFS\host-crash-<run_id>-a.img' `
    'U:\PoolFS\host-crash-<run_id>-b.img'
.\tests\host_crash_probe.ps1 -Phase WriteLoop -RunId <run_id>
```
