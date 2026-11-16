# Mount the already-formatted three-partition PoolFS v3 test pool.
# Performs no formatting or repartitioning; exact identities must still match.
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z]:$')][string]$MountPoint = 'P:',
    [string]$BinaryPath
)

$ErrorActionPreference = 'Stop'
$expected = @(
    [pscustomobject]@{
        UniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
        DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
        PartitionNumber = 2
        PartitionId = '{4f42f97a-f0c1-4b12-82ae-0a9b72f92c08}'
        Offset = [uint64]536870912
        Bytes = [uint64]4294967296
        VolumePath = '\\?\Volume{4f42f97a-f0c1-4b12-82ae-0a9b72f92c08}'
    },
    [pscustomobject]@{
        UniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
        DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
        PartitionNumber = 3
        PartitionId = '{2ffed969-cbbc-464e-b835-3e8544623e70}'
        Offset = [uint64]4831838208
        Bytes = [uint64]4294967296
        VolumePath = '\\?\Volume{2ffed969-cbbc-464e-b835-3e8544623e70}'
    },
    [pscustomobject]@{
        UniqueId = '60022480542A978B5E84E922C041AF43'
        DiskGuid = '{b0644b17-4e34-4604-a371-d2900c0dfbb7}'
        PartitionNumber = 2
        PartitionId = '{d746f67b-f9b0-4777-959c-7b8bb19b724e}'
        Offset = [uint64]536870912
        Bytes = [uint64]6442450944
        VolumePath = '\\?\Volume{d746f67b-f9b0-4777-959c-7b8bb19b724e}'
    }
)

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The raw PoolFS mount requires an elevated PowerShell window.'
}
if (Test-Path "$MountPoint\") { throw "$MountPoint is already in use." }

$arguments = @('--raw-v3', $MountPoint)
foreach ($item in $expected) {
    $disks = @(Get-Disk -ErrorAction Stop | Where-Object { $_.UniqueId -eq $item.UniqueId })
    if ($disks.Count -ne 1) { throw "Could not identify exactly one disk $($item.UniqueId)." }
    $disk = $disks[0]
    if ($disk.Number -eq 0 -or $disk.Guid -ne $item.DiskGuid -or
        $disk.IsBoot -or $disk.IsSystem -or $disk.IsOffline -or $disk.IsReadOnly -or
        $disk.PartitionStyle -ne 'GPT') {
        throw "Disk $($disk.Number) failed identity or safety checks."
    }
    $part = Get-Partition -DiskNumber $disk.Number -PartitionNumber $item.PartitionNumber -ErrorAction Stop
    if ($part.Guid -ne $item.PartitionId -or $part.GptType -ne '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}' -or
        [uint64]$part.Offset -ne $item.Offset -or [uint64]$part.Size -ne $item.Bytes -or
        [int][char]$part.DriveLetter -ne 0 -or $part.IsBoot -or $part.IsSystem -or
        @($part.AccessPaths | Where-Object { $_ -and $_.TrimEnd('\') -eq $item.VolumePath }).Count -ne 1) {
        throw "Partition $($item.PartitionId) failed identity or geometry checks."
    }
    $arguments += @($item.VolumePath, $item.DiskGuid, $item.PartitionId)
}

$repo = Split-Path $PSScriptRoot -Parent
if (-not $BinaryPath) { $BinaryPath = Join-Path $repo 'build\poolfs-mount.exe' }
if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) {
    throw "PoolFS mount binary is missing: $BinaryPath"
}
$env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
& $BinaryPath @arguments
exit $LASTEXITCODE
