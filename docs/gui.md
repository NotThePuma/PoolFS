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

1. On **Storage**, tick **Use** for one or more formatted entries in **Host
   volumes**. The **Disk / part** column shows which physical partition holds
   each volume. Click a disk below to inspect its partitions; that selection
   is optional when creating a pool. Click **Add selected volumes to pool**.
   The manager queues a new `PoolFS\member-N.img` path on each selected
   volume and switches to **Pool**. Member numbers are unique across the new
   pool, starting at 1 and increasing in selection order; an occupied name
   is skipped. It does not create the images yet.
2. On **Pool**, enter a name and choose an unused mount drive. Each queued
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

Choose **Open config** on **Pool**. The member list shows present and missing
paths. **Mount** checks the saved config against the displayed member list and
prompts for the password if the pool is encrypted. **Unmount** stops the
selected drive's PoolFS mount. **Verify data** opens the complete member set
read-only and scrubs live file data while unmounted. Verification does not
repair damage. A wrong or incomplete member list cannot mount. Use
**Save config…** to save a new JSON config after moving images or changing the
pool name or drive letter.

On **Images**, choose **Scan again** to scan the `PoolFS` folders on mounted
host volumes. The scan is bounded and does not follow directory links. Select
an image and choose **Add selected image to Pool** to build or update a config.
All member images must be listed before mounting.

## Host disks and partitions

**Storage** lists physical disks and their partitions, including partitions
without drive letters when Windows exposes them. Clicking a disk filters the
partition list, which shows each partition's drive letter, file system, size,
and label when known. **Host volumes** lists physical volumes with drive
letters; tick several to queue new member images. This works without
Administrator rights.
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

The manager does not delete member images. Remove from list changes only the
current config draft. See [mount configs](mount-config.md) for the JSON schema
and [encryption](encryption.md) for the protection boundary.
