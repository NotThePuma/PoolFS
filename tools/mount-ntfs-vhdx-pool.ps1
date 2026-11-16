# Mount an already-formatted file-backed PoolFS v4 or v5 test pool on Q:.
# Run in an ordinary PowerShell session; this performs no formatting.
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z]:$')][string]$MountPoint = 'Q:',
    [ValidateSet('v4', 'v5')][string]$FormatVersion = 'v5',
    [string]$BinaryPath
)

$ErrorActionPreference = 'Stop'
$expectedMemberBytes = if ($FormatVersion -eq 'v5') { 384MB } else { 512MB }
$hosts = @(
    [pscustomobject]@{
        DiskNumber = 1
        DiskUniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
        DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
        PartitionNumber = 4
        PartitionId = '{1114cc44-83b4-4370-a30c-5b75113a54c3}'
        Offset = [uint64]9126805504
        Bytes = [uint64]1073741824
        Letter = 'T'
        Label = 'PoolFS-Host1'
        VolumeGuidPath = '\\?\Volume{1114cc44-83b4-4370-a30c-5b75113a54c3}\'
        MemberFile = "T:\PoolFS\member-$FormatVersion.img"
    },
    [pscustomobject]@{
        DiskNumber = 2
        DiskUniqueId = '60022480542A978B5E84E922C041AF43'
        DiskGuid = '{b0644b17-4e34-4604-a371-d2900c0dfbb7}'
        PartitionNumber = 3
        PartitionId = '{f1870e12-e900-4ba8-8ff2-0f0a8bfbc4cd}'
        Offset = [uint64]6979321856
        Bytes = [uint64]1073741824
        Letter = 'U'
        Label = 'PoolFS-Host2'
        VolumeGuidPath = '\\?\Volume{f1870e12-e900-4ba8-8ff2-0f0a8bfbc4cd}\'
        MemberFile = "U:\PoolFS\member-$FormatVersion.img"
    }
)

if (Test-Path "$MountPoint\") { throw "$MountPoint is already in use." }
$members = @()
foreach ($memberHost in $hosts) {
    $disk = Get-Disk -Number $memberHost.DiskNumber -ErrorAction Stop
    if ($disk.UniqueId -ne $memberHost.DiskUniqueId -or $disk.Guid -ne $memberHost.DiskGuid -or
        $disk.IsBoot -or $disk.IsSystem -or $disk.IsOffline -or $disk.IsReadOnly -or
        $disk.PartitionStyle -ne 'GPT') {
        throw "Host disk $($memberHost.DiskNumber) identity or safety state changed."
    }
    $part = Get-Partition -DiskNumber $memberHost.DiskNumber -PartitionNumber $memberHost.PartitionNumber -ErrorAction Stop
    if ($part.Guid -ne $memberHost.PartitionId -or $part.GptType -ne '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}' -or
        [uint64]$part.Offset -ne $memberHost.Offset -or [uint64]$part.Size -ne $memberHost.Bytes -or
        [string]$part.DriveLetter -ne $memberHost.Letter -or
        @($part.AccessPaths | Where-Object { $_ -eq $memberHost.VolumeGuidPath }).Count -ne 1) {
        throw "Host partition $($memberHost.PartitionId) identity changed."
    }
    $volume = Get-Volume -Partition $part -ErrorAction Stop
    if ($volume.FileSystem -ne 'NTFS' -or $volume.FileSystemLabel -ne $memberHost.Label -or
        $volume.UniqueId -ne $memberHost.VolumeGuidPath -or
        [string]$volume.DriveLetter -ne $memberHost.Letter) {
        throw "Host volume $($memberHost.Letter): changed."
    }
    $file = Get-Item -LiteralPath $memberHost.MemberFile -ErrorAction Stop
    if ($file.PSIsContainer -or [uint64]$file.Length -ne $expectedMemberBytes) {
        throw "PoolFS member file $($memberHost.MemberFile) is missing or has the wrong size."
    }
    if (Test-Path -LiteralPath ($memberHost.MemberFile + '.poolfs-check-undo')) {
        throw "PoolFS repair is incomplete for $($memberHost.MemberFile); run poolfs-check offline."
    }
    $members += $memberHost.MemberFile
}

$repo = Split-Path $PSScriptRoot -Parent
if (-not $BinaryPath) {
    $candidates = @('build-v5-final', 'build-v5-opt2', 'build-v5-opt', 'build-v5',
                    'build-recovery', 'build-v4', 'build') |
        ForEach-Object { Join-Path $repo "$_\poolfs-mount.exe" }
    $BinaryPath = $candidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1
    if (-not $BinaryPath) { $BinaryPath = $candidates[-1] }
}
if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) {
    throw "PoolFS mount binary is missing: $BinaryPath"
}
$env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
& $BinaryPath "--images-$FormatVersion" $MountPoint @members
exit $LASTEXITCODE
