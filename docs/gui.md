# PoolFS Manager for Windows

Run `poolfs-manager.exe` from the build or ZIP folder alongside
`poolfs-gui-native.dll`, `poolfs-mount.exe`, `poolfsctl.exe`, and
`poolfs-partitions.ps1`. Install the WinFsp runtime before mounting. The
manager interface is built with Python and PySide6; it calls PoolFS's C++
backend through the native DLL. Mounting, disk inventory, and partition
operations currently run only on Windows. Its partition controls verify that
the helper script matches the native backend build. Use a trusted copy of the
manager, DLL, and helper when running as Administrator.

To build locally after the native `.\build.ps1` build, use Python 3.13,
install `requirements-manager.txt`, and run `.\build-manager.ps1` with
`-PythonPath` pointing to that Python environment. The README has the exact
PowerShell commands.

For interface development, run `.\.venv\Scripts\python.exe .\manager\app.py`
after building the native tools. The DLL locates `poolfs-mount.exe`,
`poolfsctl.exe`, and `poolfs-partitions.ps1` beside itself, including when the
manager runs from Python source. Keep that native build together.

## Create a pool

1. On **Overview**, choose **Choose host volumes** to open **Disks & volumes**. Tick **Use**
   for one or more formatted entries in **Host volumes**. The **Disk / part**
   column shows which physical partition holds
   each volume. Click a disk below to inspect its partitions; that selection
   is optional when creating a pool. Click **Continue with N volumes**.
   The manager queues a new `PoolFS\member-N.img` path on each selected
   volume and switches to **Overview**. Member numbers are unique across the new
   pool, starting at 1 and increasing in selection order; an occupied name
   is skipped. It does not create the images yet.
2. In **Pool settings**, keep the default **PoolFS** name or enter your own, then choose
   an unused mount drive. Each queued
   image starts at 384 MiB. Select a new image in the member list, then drag
   the size slider or enter an exact size in MiB. The maximum uses current
   free space on that image's host volume after reserving the sizes of other
   planned images there; FAT32 also limits each image to 4095 MiB. Member
   sizes may differ. Repeat the Storage selection if needed. A pool may have
   1 to 16 members on different partitions or disks. Free space is checked
   again when you create the pool.
3. New pools start **unencrypted**. Select **Create pool** to create the
   listed image files without a password. To protect the images, tick
   **Encrypt new pool**, select **Create encrypted pool**, and enter a new
   password twice. Review the paths and creation summary before formatting.
   Existing image files are never overwritten.
4. Save the suggested JSON config when the save dialog opens. Keep it with the
   images when transporting the pool. The config stores the pool name, drive
   letter, encryption requirement, and complete member list. The manager
   stores relative paths where possible and absolute paths for members on
   other drives.
5. Click **Mount** to open the pooled drive. Encrypted pools prompt for the
   password; unencrypted pools do not. All member images listed in the config
   must be present.

While a pool is mounted, choose **Reclaim space** to publish a fresh recovery
checkpoint. Blocks held only by the previous checkpoint become available to
the mounted drive, and Windows is notified to refresh the drive display. This
does not shrink the member image files or securely erase old blocks. You can also run
`poolfsctl reclaim V:` for a mounted `V:` pool in the same Windows logon session.
Flush, unmount, and remount do not force checkpoint rotation.

Open **Activity** to choose a mounted drive and see its current read and write
speeds, a recent-speed graph, cumulative bytes and operations, and used and
available pool space. The view updates each second. Read and write speeds
measure completed file activity served by PoolFS; they exclude physical disk
I/O, journal and checkpoint writes, checksums, and metadata operations. The
same counters and space values are available with `poolfsctl stats V:`. Use
`poolfsctl stats V: --watch` for continuous updates or add `--json` for one
JSON object per sample. The mount must use a build that includes live stats.

After opening a saved config, use **Health** to inspect every listed member,
its host drive and available host space, and whether the complete set appears
mountable. The same check is `poolfsctl health-config pool.json`; add `--json`
for a structured report. A successful **Verify data** check records a timestamp
in a `.verified` file beside the config. This timestamp is informational and
does not prove that the member images are still intact or unchanged. Health
checks public image headers and paths; it does not test an encryption password
or scrub file data.

While a pool is mounted, Health can match the config against the member files
opened by the mount and show their status as **mounted**. If an older mount
keeps those images locked, Health explains that their headers cannot be read.
Remount with the current build to see live member status.

If image creation fails partway through, the manager reports the files already
created and leaves them in place. Inspect those paths before starting again.

## Expand an existing pool

Open its saved config, unmount the pool, and choose **Grow images…**. Enter a
larger MiB size for one or more members. PoolFS makes a new copy of every
member beside its original, checks that the copies can be opened and scrubbed,
and saves a new `-grown.json` config. The manager switches to that config only
after verification succeeds. The original images and config remain intact.
Space for all new copies is required on their host volumes. Do not resize the
image files with Windows tools; shrinking is not supported.

An unencrypted pool's member images have no PoolFS password protection. If
you choose encryption, the password is not saved in the config, command line,
environment, or log. Keep that password and separate backups. Any mounted
drive is readable by Windows processes with access to it.

The pool name appears as the mounted Windows drive label when mounting from
the config. It is stored in the JSON config, not the member images. Moving
images without their config does not carry the name. A name must be a valid
Windows volume label of 1 to 32 UTF-16 code units. Existing configs without
a name display **PoolFS**.

The selected host volumes store **image files**. Selecting a disk or partition
does not make its existing files part of the pooled drive. To use a partition
without a drive letter, assign a letter first. **New pool** clears the current
draft without deleting any image files or saved config.

## Open and mount an existing pool

Choose **Open configs…** in the sidebar. Use **Recent configs** to reopen one of the
last eight configs you opened or saved. The list is saved for your current
Windows account and can be cleared from the menu.

Select several JSON files in **Open configs…** to add them to **Open pools**.
Select a pool in the list to switch to it, or choose **+ New** to start a draft.
Mounted entries show a usage bar, percent used, and free space out of total capacity.
Each mounted pool needs a different drive letter. **Close selected**
leaves its drive mounted. Reopen its config to manage it again.

You can open several configs from PowerShell:

```powershell
.\poolfs-manager.exe '.\Work pool.json' '.\Archive pool.json'
```

The member list shows present and missing
paths. **Mount** checks the saved config against the displayed member list and
prompts for the password if the pool is encrypted. **Unmount** stops the
selected drive's PoolFS mount. **Overview** shows the mount status next to
the pool's main controls. In **Maintenance**, **Verify data** opens the complete member set
read-only and scrubs live file data while unmounted. Verification does not
repair damage. A wrong or incomplete member list cannot mount. Use
**Save config…** after editing the member list, pool name, or drive letter.

To relocate a member, open the saved config and unmount the pool. With all member
images present, select one in **Member images** and click **Move image…**.
Choose a new file path on a host volume with enough free space for the entire
image. PoolFS copies the image,
saves the new path in the same JSON config, and then removes the original. You
can reopen or mount the pool using the same config. This works for encrypted
images without a password. The command-line equivalent is
`poolfsctl move-member pool.json <source.img> <destination.img>`.

After saving a config, tick **Mount when I sign in to Windows** to mount that
pool for the current Windows account at the next sign-in. Each saved config has
its own setting; use different mount letters when enabling multiple configs.
Encrypted pools show a password prompt at sign-in; the password is never
stored. PoolFS waits up to one minute for the config and all member images to
appear, then leaves the manager open with an error if they remain unavailable.
Untick the option to remove that pool's sign-in entry. This setting belongs to
the current Windows account and machine, not the portable JSON config. Keep
the manager executable and its companion binaries in the same location after
enabling it.

In **Find images**, choose **Scan again** to scan the `PoolFS` folders on mounted
host volumes. The scan is bounded and does not follow directory links. Select
any member image. When every member of that pool is present, choose **Load
complete pool**; the manager loads all of its members in order. Name the pool
and choose **Save config to mount**. The manager suggests an unused drive
letter; after saving, **Mount** becomes available. If members are missing,
the action stays disabled until the complete set is available.

## Host disks and partitions

**Disks & volumes** lists physical disks and their partitions, including partitions
without drive letters when Windows exposes them. Clicking a disk filters the
partition list, which shows each partition's drive letter, file system, size,
and label when known. **Host volumes** lists physical volumes with drive
letters; tick several to queue new member images. This works without
Administrator rights. **Disk tools** jumps to the partition controls below;
**Run as admin** appears there because only partition changes need elevation.
If a host volume shows **Unformatted**, it cannot hold an image yet. Select
its disk and partition above, use **Run as admin**, and choose **Quick format**
after unmounting PoolFS drives. Formatting erases that partition's data.

To change a host disk, choose **Run as admin**, then select a disk or partition
and use **Initialize GPT**, **Create + format**,
**Quick format**, **Remove**, or **Change letter**. The manager shows a read-only
preview with the exact disk and partition identity and requires you to type
that target before it applies a change. It rechecks the identity and system
safety guards immediately before using Windows' Storage cmdlets. Boot,
system, page-file, EFI, reserved, and recovery targets are refused. A normal
reserved partition elsewhere on a data disk does not stop you creating a new
basic-data partition. Unmount all PoolFS drives before changing host partitions.

**Create + format** can leave a newly created partition unformatted if its
second step fails. Refresh Storage and inspect that partition before deciding
what to do next; the manager never removes it automatically. Quick format and
Remove can erase data. NTFS or exFAT is suitable for larger member images;
[FAT32 limits one file to 4 GiB](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison/).
The current v6 backend uses image files on
host file systems; the older direct raw-partition backend is a separate v3
prototype and is not exposed for formatting in this GUI. The partition controls use
[Initialize-Disk](https://learn.microsoft.com/en-us/powershell/module/storage/initialize-disk),
[New-Partition](https://learn.microsoft.com/en-us/powershell/module/storage/new-partition),
[Format-Volume](https://learn.microsoft.com/en-us/powershell/module/storage/format-volume),
and [Remove-Partition](https://learn.microsoft.com/en-us/powershell/module/storage/remove-partition).

**Remove** in **Member images** changes only the current config draft.
See [mount configs](mount-config.md) for the JSON schema
and [encryption](encryption.md) for the protection boundary.
