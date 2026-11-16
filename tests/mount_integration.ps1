param(
    [string]$BinaryDirectory = 'build',
    [ValidatePattern('^[D-Zd-z]$')]
    [string]$DriveLetter = 'V',
    [string]$FirstMemberDirectory = [IO.Path]::GetTempPath(),
    [string]$SecondMemberDirectory = [IO.Path]::GetTempPath(),
    [switch]$KeepImages
)

# Exercises the Windows/WinFsp path against two newly formatted v6 images.
# Optional member directories let the same test run across separate host disks.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path $PSScriptRoot -Parent
$binaryDir = if ([IO.Path]::IsPathRooted($BinaryDirectory)) {
    [IO.Path]::GetFullPath($BinaryDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $BinaryDirectory))
}
$formatter = Join-Path $binaryDir 'poolfs-format.exe'
$mounter = Join-Path $binaryDir 'poolfs-mount.exe'
$controller = Join-Path $binaryDir 'poolfsctl.exe'
$scrubber = Join-Path $binaryDir 'poolfs-scrub.exe'
$checker = Join-Path $binaryDir 'poolfs-check.exe'
foreach ($binary in @($formatter, $mounter, $controller, $scrubber, $checker)) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Required binary is missing: $binary"
    }
}

$DriveLetter = $DriveLetter.ToUpperInvariant()
$volume = $DriveLetter + ':\'
if ((Get-PSDrive -Name $DriveLetter -ErrorAction SilentlyContinue) -or
    (Test-Path -LiteralPath $volume)) {
    throw "Refusing to use occupied drive $volume"
}
$firstRoot = (Resolve-Path -LiteralPath $FirstMemberDirectory).Path
$secondRoot = (Resolve-Path -LiteralPath $SecondMemberDirectory).Path
foreach ($root in @($firstRoot, $secondRoot)) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Member directory does not exist: $root"
    }
}

$id = [Guid]::NewGuid().ToString('N')
$first = Join-Path $firstRoot "poolfs-mount-test-$id-a.img"
$second = Join-Path $secondRoot "poolfs-mount-test-$id-b.img"
$stdout = Join-Path ([IO.Path]::GetTempPath()) "poolfs-mount-test-$id.stdout.log"
$stderr = Join-Path ([IO.Path]::GetTempPath()) "poolfs-mount-test-$id.stderr.log"
$mount = $null
$passed = $false
$originalPath = $env:PATH

# GetFileInformationByHandle exposes the Win32 file index returned by WinFsp.
# Paths alone cannot show whether a renamed open handle kept its identity.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class PoolFsTestFileIdentity
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct FileInformation
    {
        public uint Attributes;
        public uint CreationTimeLow;
        public uint CreationTimeHigh;
        public uint AccessTimeLow;
        public uint AccessTimeHigh;
        public uint WriteTimeLow;
        public uint WriteTimeHigh;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle handle, out FileInformation information);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string path, uint access, uint share, IntPtr security, uint creation,
        uint flags, IntPtr template);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileExW(
        string oldPath, string newPath, uint flags);

    public static int TryMove(string oldPath, string newPath, bool replaceExisting)
    {
        const uint MOVEFILE_REPLACE_EXISTING = 0x00000001;
        return MoveFileExW(oldPath, newPath,
            replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0) ?
            0 : Marshal.GetLastWin32Error();
    }

    public static ulong Read(SafeFileHandle handle)
    {
        FileInformation information;
        if (!GetFileInformationByHandle(handle, out information))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return ((ulong)information.FileIndexHigh << 32) | information.FileIndexLow;
    }

    public static int TryOpenDeleteOnClose(string path, bool directory)
    {
        const uint DELETE = 0x00010000;
        const uint FILE_SHARE_ALL = 0x00000007;
        const uint OPEN_EXISTING = 3;
        const uint FILE_FLAG_DELETE_ON_CLOSE = 0x04000000;
        const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
        uint flags = FILE_FLAG_DELETE_ON_CLOSE |
            (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
        using (SafeFileHandle handle = CreateFileW(path, DELETE, FILE_SHARE_ALL,
            IntPtr.Zero, OPEN_EXISTING, flags, IntPtr.Zero))
        {
            return handle.IsInvalid ? Marshal.GetLastWin32Error() : 0;
        }
    }
}
'@

function Get-TestFileId([string]$Path) {
    $file = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try { return [PoolFsTestFileIdentity]::Read($file.SafeFileHandle) }
    finally { $file.Dispose() }
}

function Assert-Metadata([string]$Path, [datetime]$CreationUtc,
                         [datetime]$WriteUtc, [IO.FileAttributes]$Attributes) {
    $actual = [IO.File]::GetAttributes($Path)
    foreach ($flag in @([IO.FileAttributes]::Hidden, [IO.FileAttributes]::Archive)) {
        if (([int]$actual -band [int]$flag) -ne ([int]$Attributes -band [int]$flag)) {
            throw "Attribute $flag changed for $Path (actual: $actual)"
        }
    }
    if ([IO.File]::GetCreationTimeUtc($Path).ToFileTimeUtc() -ne
        $CreationUtc.ToFileTimeUtc()) {
        throw "Creation time changed for $Path"
    }
    if ([IO.File]::GetLastWriteTimeUtc($Path).ToFileTimeUtc() -ne
        $WriteUtc.ToFileTimeUtc()) {
        throw "Last-write time changed for $Path"
    }
}

function Assert-ReadOnly([string]$Path) {
    if (-not (([int][IO.File]::GetAttributes($Path) -band
                [int][IO.FileAttributes]::ReadOnly))) {
        throw "Read-only attribute missing for $Path"
    }
    $writeRejected = $false
    try { [IO.File]::WriteAllText($Path, 'changed') }
    catch { $writeRejected = $true }
    if (-not $writeRejected -or [IO.File]::ReadAllText($Path) -ne 'immutable') {
        throw "Read-only file accepted a write: $Path"
    }
    $deleteRejected = $false
    try { [IO.File]::Delete($Path) }
    catch { $deleteRejected = $true }
    if (-not $deleteRejected -or -not [IO.File]::Exists($Path)) {
        throw "Read-only file accepted deletion: $Path"
    }
}

function Start-TestMount([int]$Attempt) {
    $out = $stdout + ".$Attempt"
    $err = $stderr + ".$Attempt"
    $arguments = $DriveLetter + ': "' + $first + '" "' + $second + '"'
    $script:mount = Start-Process -FilePath $mounter -ArgumentList $arguments `
        -WorkingDirectory $repo -RedirectStandardOutput $out `
        -RedirectStandardError $err -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 80; $i++) {
        if (Test-Path -LiteralPath $volume) { return }
        if ($script:mount.HasExited) {
            $details = if (Test-Path -LiteralPath $err) { Get-Content -LiteralPath $err -Raw } else { '' }
            throw "Mount exited $($script:mount.ExitCode): $details"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $volume"
}

function Stop-TestMount {
    if ($script:mount -and -not $script:mount.HasExited) {
        Stop-Process -Id $script:mount.Id -Force
        Wait-Process -Id $script:mount.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
    for ($i = 0; $i -lt 40; $i++) {
        if (-not (Test-Path -LiteralPath $volume)) {
            $script:mount = $null
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw "$volume remained mounted after the test process exited"
}

try {
    $formatOutput = @(& $formatter --sized 384 $first 512 $second 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Format failed: $($formatOutput -join [Environment]::NewLine)"
    }
    $env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
    Start-TestMount 1

    # Query the mounted drive through Windows, not only Pool::Available().
    $spaceBefore = [IO.DriveInfo]::new($DriveLetter).AvailableFreeSpace
    $spaceProbe = Join-Path $volume 'free-space-probe.bin'
    $spaceBlock = New-Object byte[] (1MB)
    $spaceStream = [IO.FileStream]::new($spaceProbe, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write)
    try {
        for ($i = 0; $i -lt 8; $i++) {
            $spaceStream.Write($spaceBlock, 0, $spaceBlock.Length)
        }
        $spaceStream.Flush($true)
    } finally { $spaceStream.Dispose() }
    $readBack = [IO.File]::ReadAllBytes($spaceProbe)
    if ($readBack.Length -ne 8MB) { throw 'Statistics read probe was incomplete' }
    $statsOutput = @(& $controller stats ($DriveLetter + ':') --json --interval-ms 100 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot read live statistics: $($statsOutput -join [Environment]::NewLine)"
    }
    $stats = ($statsOutput -join "`n" | ConvertFrom-Json)
    if ($stats.writeBytes -lt 8MB -or $stats.readBytes -lt 8MB -or
        $stats.writeOperations -lt 1 -or $stats.readOperations -lt 1 -or
        $stats.capacityBytes -le 0 -or $stats.availableBytes -le 0 -or
        $stats.availableBytes -gt $stats.capacityBytes) {
        throw "Live statistics did not record file I/O and capacity: $($statsOutput -join ' ')"
    }
    Write-Host "Live statistics: $($stats.readBytes) bytes read, $($stats.writeBytes) bytes written."
    $spaceAfterWrite = [IO.DriveInfo]::new($DriveLetter).AvailableFreeSpace
    if ($spaceAfterWrite -ne $spaceBefore - 8MB) {
        throw "Mounted free space did not fall by 8 MiB: $spaceBefore -> $spaceAfterWrite"
    }
    $firstReclaim = @(& $controller reclaim ($DriveLetter + ':') 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot publish checkpoint: $($firstReclaim -join [Environment]::NewLine)"
    }
    [IO.File]::Delete($spaceProbe)
    $spaceAfterDelete = [IO.DriveInfo]::new($DriveLetter).AvailableFreeSpace
    if ($spaceAfterDelete -ne $spaceAfterWrite) {
        throw "Checkpoint-held space became reusable before rotation: $spaceAfterWrite -> $spaceAfterDelete"
    }
    $secondReclaim = @(& $controller reclaim ($DriveLetter + ':') 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot reclaim checkpoint space: $($secondReclaim -join [Environment]::NewLine)"
    }
    $spaceAfterReclaim = [IO.DriveInfo]::new($DriveLetter).AvailableFreeSpace
    if ($spaceAfterReclaim -ne $spaceBefore) {
        throw "Mounted free space did not recover after reclaim: $spaceBefore -> $spaceAfterReclaim"
    }
    Write-Host "Mounted free space: $spaceBefore -> $spaceAfterWrite -> $spaceAfterDelete -> $spaceAfterReclaim bytes (write/checkpoint/delete/reclaim)."

    $hello = Join-Path $volume 'hello.txt'
    [IO.File]::WriteAllText($hello, 'hello')
    [IO.File]::AppendAllText($hello, ' world')
    if ([IO.File]::ReadAllText($hello) -ne 'hello world') {
        throw 'Append/read failed'
    }
    $renamed = Join-Path $volume 'renamed.txt'
    [IO.File]::Move($hello, $renamed)
    if ([IO.File]::Exists($hello) -or [IO.File]::ReadAllText($renamed) -ne 'hello world') {
        throw 'Rename failed'
    }
    $stream = [IO.FileStream]::new($renamed, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    try { $stream.SetLength(5); $stream.Flush($true) } finally { $stream.Dispose() }
    if ([IO.File]::ReadAllText($renamed) -ne 'hello') { throw 'Truncate failed' }

    $deleted = Join-Path $volume 'delete-me.txt'
    [IO.File]::WriteAllText($deleted, 'remove')
    [IO.File]::Delete($deleted)
    if ([IO.File]::Exists($deleted)) { throw 'Delete failed' }

    $unicodeName = 'café-雪.txt'
    [IO.File]::WriteAllText((Join-Path $volume $unicodeName), 'Unicode round trip')
    if ([IO.File]::ReadAllText((Join-Path $volume $unicodeName)) -ne 'Unicode round trip') {
        throw 'Unicode filename failed'
    }
    [IO.File]::WriteAllText((Join-Path $volume 'Case.txt'), 'upper')
    [IO.File]::WriteAllText((Join-Path $volume 'case.txt'), 'lower')
    if ([IO.File]::ReadAllText((Join-Path $volume 'Case.txt')) -ne 'upper' -or
        [IO.File]::ReadAllText((Join-Path $volume 'case.txt')) -ne 'lower') {
        throw 'Case-sensitive filename behavior failed'
    }

    $tree = Join-Path $volume 'tree'
    $child = Join-Path $tree 'child'
    [void][IO.Directory]::CreateDirectory($child)
    if (-not [IO.Directory]::Exists($child)) { throw 'Nested directory creation failed' }
    $nested = Join-Path $child 'deep.txt'
    [IO.File]::WriteAllText($nested, 'nested')
    [IO.File]::AppendAllText($nested, ' content')
    if ([IO.File]::ReadAllText($nested) -ne 'nested content') {
        throw 'Nested file write/read failed'
    }
    $removedNested = Join-Path $child 'remove-me.txt'
    [IO.File]::WriteAllText($removedNested, 'remove')
    [IO.File]::Delete($removedNested)
    if ([IO.File]::Exists($removedNested)) { throw 'Nested file deletion failed' }

    $share = [IO.FileShare]([int][IO.FileShare]::ReadWrite -bor [int][IO.FileShare]::Delete)
    $openNested = [IO.FileStream]::new($nested, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, $share)
    $moved = Join-Path $child 'moved.txt'
    $renamedTree = Join-Path $volume 'tree-renamed'
    try {
        $nestedId = [PoolFsTestFileIdentity]::Read($openNested.SafeFileHandle)
        if ($nestedId -eq 0) { throw 'Nested file returned zero Win32 file ID' }
        [IO.File]::Move($nested, $moved)
        if ([IO.File]::Exists($nested)) {
            throw 'Open nested file rename failed'
        }
        $bytes = New-Object byte[] 64
        [void]$openNested.Seek(0, [IO.SeekOrigin]::Begin)
        $count = $openNested.Read($bytes, 0, $bytes.Length)
        if ([Text.Encoding]::UTF8.GetString($bytes, 0, $count) -ne 'nested content') {
            throw 'Open nested handle lost its contents after file rename'
        }
        $suffix = [Text.Encoding]::UTF8.GetBytes(' through rename')
        [void]$openNested.Seek(0, [IO.SeekOrigin]::End)
        $openNested.Write($suffix, 0, $suffix.Length)
        $openNested.Flush($true)
        if ([PoolFsTestFileIdentity]::Read($openNested.SafeFileHandle) -ne $nestedId) {
            throw 'Nested handle or file ID changed after file rename'
        }
        # WinFsp's kernel layer rejects ancestor rename while a descendant is
        # open, even with FILE_SHARE_DELETE. Require that refusal explicitly.
        $ancestorRejected = $false
        try { [IO.Directory]::Move($tree, $renamedTree) }
        catch { $ancestorRejected = $true }
        if (-not $ancestorRejected -or -not [IO.Directory]::Exists($tree)) {
            throw 'Ancestor directory rename was not rejected while child was open'
        }
    } finally { $openNested.Dispose() }

    [IO.Directory]::Move($tree, $renamedTree)
    $renamedChild = Join-Path $renamedTree 'child'
    $renamedNested = Join-Path $renamedChild 'moved.txt'
    if ([IO.Directory]::Exists($tree) -or
        [IO.File]::ReadAllText($renamedNested) -ne 'nested content through rename' -or
        (Get-TestFileId $renamedNested) -ne $nestedId) {
        throw 'Directory rename changed nested path, data, or file ID'
    }

    # Match Windows' save-by-temp-and-rename path. The destination may be
    # replaced only after its own handles close; source handles remain valid.
    $replaceSource = Join-Path $renamedChild 'save.tmp'
    $replaceTarget = Join-Path $renamedChild 'document.txt'
    [IO.File]::WriteAllText($replaceSource, 'new version')
    [IO.File]::WriteAllText($replaceTarget, 'old version')
    $replaceSourceId = Get-TestFileId $replaceSource
    $replaceTargetId = Get-TestFileId $replaceTarget
    if ($replaceSourceId -eq $replaceTargetId) {
        throw 'Replacement fixture returned duplicate file IDs'
    }
    if ([PoolFsTestFileIdentity]::TryMove($replaceSource, $replaceTarget, $false) -eq 0 -or
        [IO.File]::ReadAllText($replaceTarget) -ne 'old version') {
        throw 'Rename without replace flag overwrote an existing destination'
    }
    [IO.File]::SetAttributes($replaceTarget, [IO.FileAttributes]::ReadOnly)
    try {
        if ([PoolFsTestFileIdentity]::TryMove($replaceSource, $replaceTarget, $true) -eq 0 -or
            [IO.File]::ReadAllText($replaceTarget) -ne 'old version') {
            throw 'Replacement overwrote a read-only destination'
        }
    } finally {
        [IO.File]::SetAttributes($replaceTarget, [IO.FileAttributes]::Normal)
    }
    $replaceDirectory = Join-Path $renamedChild 'replace-dir'
    [void][IO.Directory]::CreateDirectory($replaceDirectory)
    if ([PoolFsTestFileIdentity]::TryMove($replaceSource, $replaceDirectory, $true) -eq 0 -or
        -not [IO.Directory]::Exists($replaceDirectory) -or
        [IO.File]::ReadAllText($replaceSource) -ne 'new version') {
        throw 'Replacement overwrote a directory destination'
    }
    $targetOpen = [IO.FileStream]::new($replaceTarget, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, $share)
    try {
        if ([PoolFsTestFileIdentity]::TryMove($replaceSource, $replaceTarget, $true) -eq 0) {
            throw 'Replacement overwrote a destination with an open handle'
        }
    } finally { $targetOpen.Dispose() }
    if ([IO.File]::ReadAllText($replaceTarget) -ne 'old version' -or
        [IO.File]::ReadAllText($replaceSource) -ne 'new version') {
        throw 'Open-target replacement changed source or destination data'
    }
    $sourceOpen = [IO.FileStream]::new($replaceSource, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, $share)
    $replaceExpected = 'new version via open handle'
    try {
        if ([PoolFsTestFileIdentity]::Read($sourceOpen.SafeFileHandle) -ne $replaceSourceId) {
            throw 'Open source ID changed before replacement'
        }
        $moveResult = [PoolFsTestFileIdentity]::TryMove($replaceSource, $replaceTarget, $true)
        if ($moveResult -ne 0) { throw "MoveFileEx replace failed with Win32 $moveResult" }
        if ([IO.File]::Exists($replaceSource)) {
            throw 'Replacement retained the source path'
        }
        $buffer = New-Object byte[] 64
        [void]$sourceOpen.Seek(0, [IO.SeekOrigin]::Begin)
        $readCount = $sourceOpen.Read($buffer, 0, $buffer.Length)
        if ([Text.Encoding]::UTF8.GetString($buffer, 0, $readCount) -ne 'new version') {
            throw 'Open source handle lost its original bytes after replacement'
        }
        $suffix = [Text.Encoding]::UTF8.GetBytes(' via open handle')
        [void]$sourceOpen.Seek(0, [IO.SeekOrigin]::End)
        $sourceOpen.Write($suffix, 0, $suffix.Length)
        $sourceOpen.Flush($true)
        [void]$sourceOpen.Seek(0, [IO.SeekOrigin]::Begin)
        $readCount = $sourceOpen.Read($buffer, 0, $buffer.Length)
        if ([PoolFsTestFileIdentity]::Read($sourceOpen.SafeFileHandle) -ne $replaceSourceId -or
            [Text.Encoding]::UTF8.GetString($buffer, 0, $readCount) -ne $replaceExpected) {
            throw 'Open source handle lost its data or ID after replacement'
        }
    } finally { $sourceOpen.Dispose() }
    if ([IO.File]::ReadAllText($replaceTarget) -ne $replaceExpected -or
        (Get-TestFileId $replaceTarget) -ne $replaceSourceId -or
        (Get-TestFileId $replaceTarget) -eq $replaceTargetId) {
        throw 'Replacement did not move source data and ID to destination'
    }
    foreach ($replaceFlag in @($false, $true)) {
        $moveResult = [PoolFsTestFileIdentity]::TryMove($replaceTarget, $replaceTarget,
            $replaceFlag)
        if ($moveResult -ne 0 -or (Get-TestFileId $replaceTarget) -ne $replaceSourceId -or
            [IO.File]::ReadAllText($replaceTarget) -ne $replaceExpected) {
            throw "Same-name rename changed replacement target (replace=$replaceFlag, Win32=$moveResult)"
        }
    }
    $absentSource = Join-Path $renamedChild 'absent-source.tmp'
    $absentTarget = Join-Path $renamedChild 'absent-target.txt'
    [IO.File]::WriteAllText($absentSource, 'absent destination')
    $absentId = Get-TestFileId $absentSource
    $moveResult = [PoolFsTestFileIdentity]::TryMove($absentSource, $absentTarget, $true)
    if ($moveResult -ne 0 -or [IO.File]::Exists($absentSource) -or
        [IO.File]::ReadAllText($absentTarget) -ne 'absent destination' -or
        (Get-TestFileId $absentTarget) -ne $absentId) {
        throw "Replace flag could not rename to an absent destination (Win32=$moveResult)"
    }
    $directorySource = Join-Path $renamedChild 'directory-source'
    [void][IO.Directory]::CreateDirectory($directorySource)
    if ([PoolFsTestFileIdentity]::TryMove($directorySource, $replaceTarget, $true) -eq 0 -or
        -not [IO.Directory]::Exists($directorySource) -or
        [IO.File]::ReadAllText($replaceTarget) -ne $replaceExpected) {
        throw 'Directory source replaced a file destination'
    }

    $nestedNames = @([IO.Directory]::EnumerateFiles($renamedChild) |
        ForEach-Object { [IO.Path]::GetFileName($_) })
    if ($nestedNames -cnotcontains 'moved.txt' -or
        $nestedNames -ccontains 'deep.txt' -or
        $nestedNames -ccontains 'remove-me.txt') {
        throw 'Nested directory enumeration is inconsistent'
    }
    # Long names force WinFsp to return this directory in multiple buffers.
    $paged = Join-Path $renamedTree 'paged'
    [void][IO.Directory]::CreateDirectory($paged)
    $pagedNames = @(for ($i = 0; $i -lt 40; $i++) {
        ('entry-{0:D2}-' -f $i) + ('x' * 170) + '.txt'
    })
    foreach ($name in $pagedNames) {
        [IO.File]::WriteAllText((Join-Path $paged $name), 'page')
    }
    $actualPagedNames = @([IO.Directory]::EnumerateFiles($paged) |
        ForEach-Object { [IO.Path]::GetFileName($_) })
    if ($actualPagedNames.Count -ne $pagedNames.Count) {
        throw "Paged directory returned $($actualPagedNames.Count) of $($pagedNames.Count) entries"
    }
    foreach ($name in $pagedNames) {
        if ($actualPagedNames -cnotcontains $name) {
            throw "Paged directory enumeration missed $name"
        }
    }
    $metadata = Join-Path $renamedChild 'metadata.txt'
    [IO.File]::WriteAllText($metadata, 'metadata')
    $metaAttributes = [IO.FileAttributes]([int][IO.FileAttributes]::Hidden -bor
        [int][IO.FileAttributes]::Archive)
    [IO.File]::SetAttributes($metadata, $metaAttributes)
    $creationUtc = [datetime]::new(2020, 5, 12, 13, 14, 15, [DateTimeKind]::Utc)
    $writeUtc = [datetime]::new(2021, 6, 13, 14, 15, 16, [DateTimeKind]::Utc)
    [IO.File]::SetCreationTimeUtc($metadata, $creationUtc)
    [IO.File]::SetLastWriteTimeUtc($metadata, $writeUtc)
    Assert-Metadata $metadata $creationUtc $writeUtc $metaAttributes

    $readOnly = Join-Path $renamedChild 'read-only.txt'
    [IO.File]::WriteAllText($readOnly, 'immutable')
    [IO.File]::SetAttributes($readOnly, [IO.FileAttributes]::ReadOnly)
    Assert-ReadOnly $readOnly
    if ([PoolFsTestFileIdentity]::TryOpenDeleteOnClose($readOnly, $false) -eq 0 -or
        -not [IO.File]::Exists($readOnly)) {
        throw 'Read-only file accepted FILE_FLAG_DELETE_ON_CLOSE'
    }

    # A 384 MiB member has less than 128 MiB of file-data space, so 152 MiB
    # must cross into the 512 MiB member.
    $spanning = Join-Path $volume 'spanning.bin'
    $block = New-Object byte[] (4MB)
    [Security.Cryptography.RandomNumberGenerator]::Fill($block)
    $stream = [IO.FileStream]::new($spanning, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        for ($i = 0; $i -lt 38; $i++) { $stream.Write($block, 0, $block.Length) }
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $spanning).Hash
    $names = @([IO.Directory]::EnumerateFiles($volume) | ForEach-Object { [IO.Path]::GetFileName($_) })
    foreach ($expected in @('renamed.txt', $unicodeName, 'Case.txt', 'case.txt', 'spanning.bin')) {
        if ($names -cnotcontains $expected) { throw "Directory enumeration missed $expected" }
    }
    if (-not [IO.Directory]::Exists($renamedTree) -or [IO.Directory]::Exists($tree)) {
        throw 'Root directory enumeration lost the renamed directory'
    }
    if ($names -ccontains 'delete-me.txt' -or $names -ccontains 'hello.txt') {
        throw 'Directory enumeration retained a deleted or renamed name'
    }

    # Force the mount process to exit, then require full data and metadata
    # recovery on a fresh mount of the same member images.
    $spaceBeforeRemount = [IO.DriveInfo]::new($DriveLetter).AvailableFreeSpace
    Stop-TestMount
    Start-TestMount 2
    $spaceAfterRemount = [IO.DriveInfo]::new($DriveLetter).AvailableFreeSpace
    if ($spaceAfterRemount -ne $spaceBeforeRemount) {
        throw "Mounted free space changed on remount: $spaceBeforeRemount -> $spaceAfterRemount"
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $spanning).Hash -ne $hash) {
        throw 'Cross-member file hash changed after remount'
    }
    if ([IO.File]::ReadAllText($renamed) -ne 'hello' -or
        [IO.File]::ReadAllText((Join-Path $volume $unicodeName)) -ne 'Unicode round trip') {
        throw 'Small files changed after remount'
    }
    if (-not [IO.Directory]::Exists($renamedChild) -or
        [IO.File]::ReadAllText($renamedNested) -ne 'nested content through rename' -or
        (Get-TestFileId $renamedNested) -ne $nestedId) {
        throw 'Nested directory, data, or stable file ID changed after remount'
    }
    if ([IO.File]::Exists($replaceSource) -or
        [IO.File]::ReadAllText($replaceTarget) -ne $replaceExpected -or
        (Get-TestFileId $replaceTarget) -ne $replaceSourceId -or
        [IO.File]::ReadAllText($absentTarget) -ne 'absent destination' -or
        (Get-TestFileId $absentTarget) -ne $absentId) {
        throw 'Replace-existing rename lost data or stable source IDs after remount'
    }
    Assert-Metadata $metadata $creationUtc $writeUtc $metaAttributes
    Assert-ReadOnly $readOnly
    $actualPagedNames = @([IO.Directory]::EnumerateFiles($paged) |
        ForEach-Object { [IO.Path]::GetFileName($_) })
    if ($actualPagedNames.Count -ne $pagedNames.Count) {
        throw 'Paged directory entry count changed after remount'
    }
    foreach ($name in $pagedNames) {
        if ($actualPagedNames -cnotcontains $name) {
            throw "Paged directory lost $name after remount"
        }
    }

    $nonemptyRejected = $false
    try { [IO.Directory]::Delete($renamedTree) }
    catch { $nonemptyRejected = $true }
    if (-not $nonemptyRejected -or -not [IO.Directory]::Exists($renamedTree)) {
        throw 'Nonempty directory deletion was not rejected'
    }
    if ([PoolFsTestFileIdentity]::TryOpenDeleteOnClose($renamedTree, $true) -eq 0 -or
        -not [IO.Directory]::Exists($renamedTree)) {
        throw 'Nonempty directory accepted FILE_FLAG_DELETE_ON_CLOSE'
    }
    [IO.File]::SetAttributes($readOnly, [IO.FileAttributes]::Normal)
    [IO.File]::Delete($readOnly)
    [IO.File]::Delete($metadata)
    [IO.File]::Delete($renamedNested)
    [IO.File]::Delete($replaceTarget)
    [IO.File]::Delete($absentTarget)
    [IO.Directory]::Delete($replaceDirectory)
    [IO.Directory]::Delete($directorySource)
    [IO.Directory]::Delete($renamedChild)
    foreach ($name in $pagedNames) { [IO.File]::Delete((Join-Path $paged $name)) }
    [IO.Directory]::Delete($paged)
    [IO.Directory]::Delete($renamedTree)
    if ([IO.Directory]::Exists($renamedTree)) { throw 'Nested directory deletion failed' }
    Stop-TestMount

    # Temporarily hide one uniquely named image, verify the pool refuses to
    # mount, then restore it on the same host volume before final validation.
    $missingMember = $second + '.missing'
    [IO.File]::Move($second, $missingMember)
    $missingMount = $null
    try {
        $arguments = $DriveLetter + ': "' + $first + '" "' + $second + '"'
        $missingMount = Start-Process -FilePath $mounter -ArgumentList $arguments `
            -WorkingDirectory $repo -RedirectStandardOutput "$stdout.missing" `
            -RedirectStandardError "$stderr.missing" -WindowStyle Hidden -PassThru
        if (-not $missingMount.WaitForExit(10000)) {
            throw 'Mount process stayed alive with one missing member'
        }
        if ($missingMount.ExitCode -eq 0 -or (Test-Path -LiteralPath $volume)) {
            throw 'Pool mounted or reported success with one missing member'
        }
    } finally {
        if ($missingMount -and -not $missingMount.HasExited) {
            Stop-Process -Id $missingMount.Id -Force
            Wait-Process -Id $missingMount.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
        [IO.File]::Move($missingMember, $second)
    }
    Start-TestMount 3
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $spanning).Hash -ne $hash -or
        [IO.Directory]::Exists($renamedTree)) {
        throw 'Restored-member remount changed data or resurrected deleted directory'
    }
    Stop-TestMount

    $scrubOutput = @(& $scrubber $first $second 2>&1)
    if ($LASTEXITCODE -ne 0 -or ($scrubOutput -join "`n") -notmatch 'status: CLEAN') {
        throw "Scrub failed: $($scrubOutput -join [Environment]::NewLine)"
    }
    $checkOutput = @(& $checker $first $second 2>&1)
    if ($LASTEXITCODE -ne 0 -or ($checkOutput -join "`n") -notmatch 'status: CLEAN') {
        throw "Check failed: $($checkOutput -join [Environment]::NewLine)"
    }
    $passed = $true
    Write-Host "Mounted v6 integration passed on $volume (nested namespace, atomic replacement, metadata, missing-member refusal, 384+512 MiB, 152 MiB cross-member file, SHA256 $hash)."
} finally {
    try { Stop-TestMount } finally { $env:PATH = $originalPath }
    if ($passed -and -not $KeepImages) {
        # Only the exact, uniquely named files created above are removed.
        foreach ($path in @($first, $second, "$stdout.1", "$stderr.1", "$stdout.2", "$stderr.2",
                             "$stdout.3", "$stderr.3", "$stdout.missing", "$stderr.missing")) {
            if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
        }
    } else {
        Write-Warning "Test images retained: $first ; $second"
    }
}
