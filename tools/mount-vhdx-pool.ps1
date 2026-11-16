# Legacy PoolFS v2 mount helper for the former VHDX layout. The current test
# pool uses tools/mount-vhdx-v3-pool.ps1. This script performs no provisioning
# or formatting and rejects the current partition identities.
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z]:$')]
    [string]$MountPoint = 'P:'
)

$ErrorActionPreference = 'Stop'
$expected = @(
    [pscustomobject]@{
        UniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
        DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
        VolumePath = '\\?\Volume{9edffd66-b3ed-4648-bae4-322b732cb6c3}'
        PartitionBytes = [uint64]10718543872
    },
    [pscustomobject]@{
        UniqueId = '60022480542A978B5E84E922C041AF43'
        DiskGuid = '{b0644b17-4e34-4604-a371-d2900c0dfbb7}'
        VolumePath = '\\?\Volume{11eca817-1337-4f5f-bdff-5b7c5a163011}'
        PartitionBytes = [uint64]8571060224
    }
)
$basicData = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$msr = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The raw PoolFS mount requires an elevated PowerShell window.'
}

$diskNumbers = @()
foreach ($item in $expected) {
    $matches = @(Get-Disk -ErrorAction Stop | Where-Object { $_.UniqueId -eq $item.UniqueId })
    if ($matches.Count -ne 1) { throw "Could not identify exactly one disk $($item.UniqueId)." }
    $disk = $matches[0]
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.IsOffline -or $disk.IsReadOnly -or
        $disk.PartitionStyle -ne 'GPT' -or $disk.Guid -ne $item.DiskGuid) {
        throw "Disk $($disk.Number) failed identity or safety checks."
    }
    $parts = @(Get-Partition -DiskNumber $disk.Number -ErrorAction Stop)
    $basic = @($parts | Where-Object { $_.GptType -eq $basicData })
    $reserved = @($parts | Where-Object { $_.GptType -eq $msr })
    if ($parts.Count -ne 2 -or $basic.Count -ne 1 -or $reserved.Count -ne 1 -or
        [uint64]$basic[0].Size -ne $item.PartitionBytes -or
        [int][char]$basic[0].DriveLetter -ne 0 -or
        @($basic[0].AccessPaths | Where-Object {
            $_ -and $_.TrimEnd('\') -eq $item.VolumePath
        }).Count -ne 1) {
        throw "Disk $($disk.Number) partition layout changed."
    }
    $diskNumbers += $disk.Number
}
if ($diskNumbers[0] -eq $diskNumbers[1]) { throw 'Both members resolved to one disk.' }
if (Test-Path "$MountPoint\") { throw "$MountPoint is already in use." }

$repo = Split-Path $PSScriptRoot -Parent
$binary = Join-Path $repo 'build\poolfs-mount.exe'
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw 'Build PoolFS before mounting the VHDX pool.'
}
$env:PATH = 'C:\Program Files (x86)\WinFsp\bin;' + $env:PATH
& $binary --raw $expected[0].VolumePath $expected[0].DiskGuid `
    $expected[1].VolumePath $expected[1].DiskGuid $MountPoint
exit $LASTEXITCODE
