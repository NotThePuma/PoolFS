# Pool management

`poolfsctl.exe` discovers v6 image members and controls a PoolFS mount in the
current Windows logon session. It runs outside the filesystem data path; file
I/O remains in the native PoolFS core and WinFsp callback layer.
The controller accepts unencrypted v6 layout revision 2 and password-encrypted
revision 3, with 1 to 16 members. Discovery reports whether a password is required.

```powershell
.\build\poolfsctl.exe discover 'T:\PoolFS\member.img' 'U:\MyMembers'
.\build\poolfsctl.exe mount R: 'T:\PoolFS\member.img' 'U:\MyMembers'
.\build\poolfsctl.exe status R:
.\build\poolfsctl.exe stats R:
.\build\poolfsctl.exe unmount R:
```

Discovery starts with one member image. It scans that image's folder and a
`PoolFS` folder at the root of each available drive. Pass additional search
folders or image paths when members live elsewhere. The command validates the
two v6 header copies, checksum, pool and member IDs, manifest, and exact image
sizes. It lists members in pool order and marks absent members `MISSING`.
Discovery returns code 0 for a complete set, 2 for missing members, and 1 for
invalid or conflicting members.
Member paths are not stored in the image headers. A complete pool can be moved
to other host volumes or Windows drive letters; see [portable pools](portable-pools.md).

`mount` requires a complete set and an unused drive letter. It starts
`poolfs-mount.exe` from the same folder and waits for its ready signal. The
mount continues after the controller exits. `unmount` signals the mount to
stop its WinFsp dispatcher and remove the drive letter. `status` and `unmount`
can control only mounts using this named shutdown event in the same Windows
logon session; mounts from older binaries without that event remain
independent.

`stats` samples a mounted pool for one second and reports read and write
bytes per second, cumulative byte and operation counts, uptime, and available
and total pool capacity. Add `--watch` for repeated samples, `--json` for
one JSON object per sample, or `--interval-ms 500` to change the sampling
interval (100 to 10000 ms). The manager's **Stats** page uses the same live
counters. They measure completed file reads and writes served by WinFsp, not
physical drive throughput or PoolFS internal I/O. Older mounts must be
remounted with the updated binary to publish these counters.

For an encrypted pool, `mount` prompts once in the local console and transfers
the password to the hidden mount process through a private inherited pipe.
The password never appears in the process command line or environment.
Unmount the drive to close the unlocked process. See [encryption details](encryption.md).

The controller does not repair or reconstruct an incomplete pool. Stop the
mount before disconnecting a host volume or running the offline checker and
scrubber. Member paths are scanned read-only, and the mount's core validates
the complete set again before serving files.
