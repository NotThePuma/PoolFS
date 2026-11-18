# PoolFS prototype

PoolFS is an experimental pooled filesystem for Windows, written in C++20.
WinFsp provides the Windows mount interface; PoolFS owns the on-disk format,
file catalog, allocation, checksums, and recovery. The initial PoolFS format is
v6. It combines 1–16 image files into one drive letter, and the member files
may have different fixed sizes.

The desktop manager uses Python and PySide6 for its interface. It calls the
same C++ storage backend through a native DLL. PoolFS mounting, disk inventory,
and partition operations currently require Windows.

You can give a pool a name in its mount config. That name appears as the
mounted Windows drive label. It is config metadata, not part of the member
images; carry the config with the images if you want to preserve the name.

New v6 pools can be password encrypted. PoolFS prompts once when mounting and
encrypts file contents, names, journal records, checkpoints, and checksum pages
inside each member image. A small public header keeps the member IDs, sizes,
salt, and password derivation settings needed to find and unlock the pool.

PoolFS is under active development. It has no redundancy: every member is
required to mount, and losing one can make the pool unavailable. Keep an
independent backup of files you care about. The mounted filesystem supports
nested, case-sensitive directories and persists basic file attributes and
timestamps. It can atomically replace an existing file through a rename.
It does not yet support per-file ACLs or named streams; the mount
reports an Everyone-full-access security descriptor. Physical power-loss
behavior and full Windows compatibility remain unverified.

## Download and source

The current Windows x64 ZIP is available at [poolfs.com](https://poolfs.com/#downloads).
The source is published under the [MIT license](LICENSE). The download and
source repository are separate: the repository does not contain built binaries.
See [third-party notices](THIRD_PARTY_NOTICES.md) for dependency licenses.

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos.
[WinFsp source](https://github.com/winfsp/winfsp).

## Build and test

Install [Zig](https://ziglang.org/download/) and
[WinFsp](https://winfsp.dev/rel/) on Windows. Building requires the WinFsp
**Developer** feature, or an SDK root containing `inc` and `lib`. Mounting also
requires the WinFsp runtime driver.

From this directory in PowerShell:

```powershell
.\build.ps1
.\build\poolfs-v6-integrity-tests.exe
.\build\poolfs-v6-encryption-tests.exe
.\build\poolfs-v6-portable-members-tests.exe
.\build\poolfs-crypto-tests.exe
.\build\poolfs-gui-backend-tests.exe --integration
.\build\poolfs-gui-partition-tests.exe --integration
.\build\poolfs-v6-crash-tests.exe
.\tests\check_repair.ps1 -BinaryDirectory build -MixedSize
.\tests\mount_integration.ps1 -BinaryDirectory build -DriveLetter V
.\tests\poolfsctl.ps1 -BinaryDirectory build -Drive V:
```

The crash sweep uses fresh temporary test images and simulates process exits at
write and flush boundaries. It can take several minutes. If `zig.exe` is not
on `PATH`, `build.ps1` searches its WinGet installation or accepts `-ZigPath`.
Release uses `-O2`; use `-Configuration Debug` for an unoptimized build. Use
`-OutputDirectory` to build without replacing an executable that is mounted.

To build the Qt desktop manager, install Python 3.13 and its pinned
GUI packages, then package the manager after the native build:

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\requirements-manager.txt
.\build.ps1
.\build-manager.ps1 -PythonPath .\.venv\Scripts\python.exe
.\.venv\Scripts\python.exe .\tests\manager_qt_tests.py
.\build\poolfs-manager.exe
```

Use `-PythonPath` with a different Python installation if needed. Keep
`poolfs-gui-native.dll`, `poolfs-mount.exe`, `poolfsctl.exe`, and
`poolfs-partitions.ps1` beside the packaged manager. The native PoolFS backend
and WinFsp mount remain Windows-only.

For interface development, run `.\.venv\Scripts\python.exe .\manager\app.py`
after the native build. The DLL locates its helper tools beside itself, even
when the interface runs from Python source.

## Create and mount a pool

The formatter creates new image files and refuses to overwrite existing paths.
Each member must be at least 384 MiB. This example gives both members 384 MiB:

```powershell
New-Item -ItemType Directory -Path .\work -Force | Out-Null
.\build\poolfs-format.exe 384 .\work\member-a.img .\work\member-b.img
$env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
.\build\poolfsctl.exe discover .\work\member-a.img
.\build\poolfsctl.exe mount R: .\work\member-a.img
```

The controller finds the other member in the seed image's folder and keeps
the mount running after the command returns. Use `R:\` as a drive, then run
`.\build\poolfsctl.exe reclaim R:` to publish a new checkpoint and make
space retained only by the old checkpoint available to the mounted drive. The
**Reclaim space** button in the manager does the same thing. It does not shrink
member image files. Run
`.\build\poolfsctl.exe unmount R:` before removing any host volume. Choose an
unused drive letter if `R:` is occupied. Direct `poolfs-mount.exe` remains
available for a foreground mount. See [pool management](docs/pool-management.md)
for discovery across volumes and controller status.

Open **Activity** in the manager to see live read and write speeds, totals,
operation counts, and available pool space. The same measurements are available
from the command line with `.\build\poolfsctl.exe stats R:`; add `--watch` for
continuous updates or `--json` for machine-readable output. These speeds count
completed file reads and writes served by PoolFS, not physical disk traffic.

Open **Health** after loading a saved config to see each required member's
status, host drive, and available host space. The same report is available as
`.\build\poolfsctl.exe health-config .\my-pool.json` (add `--json` for structured
output). A successful **Verify data** check records an informational timestamp
beside the config as `.verified`; it does not guarantee that the images have
remained unchanged since verification.

For different member sizes, supply a size in MiB before each new image path:

```powershell
.\build\poolfs-format.exe --sized 384 'X:\PoolFS\member.img' 768 'Y:\PoolFS\member.img'
.\build\poolfsctl.exe mount R: 'X:\PoolFS\member.img'
```

The member files can share one host volume or live on separate NTFS or exFAT
partitions and drives. PoolFS
presents files stored **inside** those images; existing files on the host
volumes do not appear in the pooled drive. FAT32 can also host a member file,
but its roughly 4 GiB per-file limit caps that member's size. The usable pool
capacity is the sum of member data areas after each image's 256 MiB metadata
reservation and checksum pages. A file can span members. Member identities and
sizes are recorded when formatted: changing an image length with host tools
does not grow the pool. To expand one or more images offline, use **Grow
images** in the manager, or make verified expanded copies of every member:

```powershell
.\build\poolfs-format.exe --grow-copy 'X:\PoolFS\member.img' 768 'X:\PoolFS\member-grown.img' 'Y:\PoolFS\member.img' 384 'Y:\PoolFS\member-grown.img'
```

All members must be supplied, and every destination must be a new path. The
unchanged-size members are copied too because the member manifest is shared.
Encrypted pools prompt for their password. Originals remain intact; mount the
new copies together after the command succeeds. Shrinking is not supported.
The member images contain no host paths, so a complete pool can be moved to
other drive letters or Windows machines with WinFsp installed. Every member
recorded at format time is still required. A single-member pool can also be
created; it cannot compare interrupted journal commits with another member.
See [portable member images](docs/portable-pools.md) for the move procedure and
host filesystem limits.

## Inspect an unmounted pool

Stop the mount, then pass the **complete** member set to the offline tools:

```powershell
.\build\poolfs-scrub.exe .\work\member-a.img .\work\member-b.img
.\build\poolfs-check.exe .\work\member-a.img .\work\member-b.img
```

`poolfs-scrub` reads live file data and selected checksum pages and reports
detected damage; it cannot rebuild data. `poolfs-check` inspects metadata and
can discard one cleanly interrupted final mirrored journal commit when it
reports that specific condition:

```powershell
.\build\poolfs-check.exe --repair-final-tail .\work\member-a.img .\work\member-b.img
```

The repair saves undo sidecars beside affected images before changing a journal
slot. If interrupted, keep the sidecars and rerun the same command with the
complete member set. Do not mount while a repair sidecar is pending. See the
[format and recovery notes](docs/v6-integrity.md) for exact exit codes and
limits.

PoolFS stores a CRC-64/XZ checksum for every 4 KiB file-data block. Checksum
pages are updated copy-on-write and selected through the mirrored metadata
journal. This detects accidental damage, but cannot repair it or authenticate
against deliberate changes. File data freed by deletion, truncation, and
overwrite can be reused. An entirely full pool needs a free data block for a
copy-on-write overwrite. The member count and total capacity cannot yet be
changed online.

## Password encrypted pools

The Python/Qt Windows manager shows host disks, partitions, volumes, PoolFS
member images, and configs. Run `.\build\poolfs-manager.exe` beside the other
built tools. It can create v6 image pools with individual member sizes, save a
portable JSON config, mount and unmount, and verify an unmounted pool. Select
one or more formatted host volumes on **Disks & volumes**, then **Continue with N
volumes** to queue new member images on those partitions. Its built-in
partition controls require Administrator rights. See the [GUI guide](docs/gui.md).

New pools in the manager start **unencrypted**. Select **Encrypt new pool**
before creating one if its member images need password protection. An
unencrypted pool mounts without a password; its image files have no PoolFS
password protection. The manager saves the chosen encryption requirement in
the JSON config.

Create **new** encrypted member images with a password entered at the local
console. The formatter asks for confirmation and never accepts a password on
its command line:

```powershell
.\build\poolfs-format.exe --encrypted 384 'X:\PoolFS\member.img' 'Y:\PoolFS\member.img'
.\build\poolfsctl.exe mount R: 'X:\PoolFS\member.img'
.\build\poolfsctl.exe unmount R:
```

For different sizes, use `--encrypted-sized` followed by size/path pairs.
For one member, provide one image path after `--encrypted 384`.
To keep the drive letter and complete member list together, edit
[`mount-config.example.json`](mount-config.example.json) and run
`.\build\poolfsctl.exe mount-config .\mount-config.example.json`.
Relative member paths are resolved from the config file's folder; the password
is entered at mount time only for encrypted pools and is never stored in JSON.
The example config is unencrypted; set `require_encrypted` to `true` when using
encrypted member images. See
[mount config](docs/mount-config.md) for the exact schema and direct-mount command.

`poolfsctl mount`, direct `poolfs-mount`, `poolfs-check`, and `poolfs-scrub`
prompt when opening an encrypted pool. The controller passes the password to
its hidden mount process through a private inherited pipe, never through a
command argument or environment variable. Keep the password: this version has
no recovery key or password reset. If you lose it, the encrypted files cannot
be recovered by PoolFS.

Encryption protects the member images while the pool is unmounted. The mounted
drive exposes plaintext to Windows processes with access to it; PoolFS still
reports an Everyone-full-access security descriptor. Existing unencrypted v6
images stay unencrypted. To protect their files, copy them into a newly
formatted encrypted pool, then handle old images, backups, and VM checkpoints
as plaintext copies. The on-disk cipher is AES-256-XTS with a password-derived
key. It does not authenticate file data against deliberate tampering; current
checksums detect accidental damage. See [encryption details](docs/encryption.md).

The native benchmark harness and dated test results are in
[bench/BENCHMARKS.md](bench/BENCHMARKS.md). Older prototype formats and raw
partition experiments are described in historical test notes; they are not
part of the initial PoolFS product format.
