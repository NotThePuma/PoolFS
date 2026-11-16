# Replace the two disposable PoolFS v2 VHDX data partitions with three
# unformatted v3 test partitions. Read-only by default; -Apply writes.
# This helper intentionally cannot resume after any partial write.
[CmdletBinding()]
param([switch]$Apply)

$ErrorActionPreference = 'Stop'
$basicData = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$msrType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
$firstOffset = [uint64](512MB)

# Exact identities and old layout observed on 2026-11-12. Never select a disk
# by number alone. MSR is checked, preserved, and never passed to Remove-Partition.
$targets = @(
    [pscustomobject]@{
        Number = 1
        UniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
        DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
        DiskBytes = [uint64]10737418240
        DevicePath = '\\?\scsi#disk&ven_msft&prod_virtual_disk#5&1adfedf9&0&000001#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}'
        Msr = [pscustomobject]@{ Number = 1; Guid = '{72ce6dee-9830-4da1-a1be-89f650ee22b4}'; Offset = [uint64]17408; Size = [uint64]16759808 }
        OldData = [pscustomobject]@{ Number = 2; Guid = '{9edffd66-b3ed-4648-bae4-322b732cb6c3}'; Offset = [uint64]16777216; Size = [uint64]10718543872 }
        Planned = @(
            [pscustomobject]@{ Offset = $firstOffset; Size = [uint64](4GB) },
            [pscustomobject]@{ Offset = [uint64]($firstOffset + 4GB); Size = [uint64](4GB) }
        )
    },
    [pscustomobject]@{
        Number = 2
        UniqueId = '60022480542A978B5E84E922C041AF43'
        DiskGuid = '{b0644b17-4e34-4604-a371-d2900c0dfbb7}'
        DiskBytes = [uint64]8589934592
        DevicePath = '\\?\scsi#disk&ven_msft&prod_virtual_disk#5&1adfedf9&0&000002#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}'
        Msr = [pscustomobject]@{ Number = 1; Guid = '{4ea1d2c9-13f0-4813-989f-1d1a1e5fb428}'; Offset = [uint64]17408; Size = [uint64]16759808 }
        OldData = [pscustomobject]@{ Number = 2; Guid = '{11eca817-1337-4f5f-bdff-5b7c5a163011}'; Offset = [uint64]16777216; Size = [uint64]8571060224 }
        Planned = @([pscustomobject]@{ Offset = $firstOffset; Size = [uint64](6GB) })
    }
)

function Same([string]$a, [string]$b) {
    return [string]::Equals($a, $b, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoPMount {
    if ((Get-PSDrive -Name P -ErrorAction SilentlyContinue) -or
        @(Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='P:'" -ErrorAction Stop).Count -ne 0) {
        throw 'P: is mounted. Stop the PoolFS mount before reconfiguring test partitions.'
    }
}

function Resolve-PageFileDisk([string]$path, [object[]]$partitions) {
    if ($path -match '(?i)Volume\{([0-9a-f-]{36})\}\\') {
        $volumeId = $Matches[1]
        $volumeFragment = "Volume{$volumeId}\"
        $matches = @($partitions | Where-Object {
            $p = $_
            @($p.AccessPaths | Where-Object {
                $_ -and $_.IndexOf($volumeFragment, [StringComparison]::OrdinalIgnoreCase) -ge 0
            }).Count -gt 0
        })
    } elseif ($path -match '(?i)([A-Z]):\\') {
        $letter = $Matches[1]
        $matches = @($partitions | Where-Object { Same ([string]$_.DriveLetter) $letter })
    } else {
        throw "Cannot identify page-file disk from '$path'; refusing partition changes."
    }
    if ($matches.Count -ne 1) {
        throw "Cannot uniquely resolve page-file disk from '$path'; refusing partition changes."
    }
    return [uint32]$matches[0].DiskNumber
}

function Assert-NoTargetPageFile {
    $partitions = @(Get-Partition -ErrorAction Stop)
    $usage = @(Get-CimInstance Win32_PageFileUsage -ErrorAction Stop | ForEach-Object { [string]$_.Name } | Where-Object { $_ })
    $settings = @(Get-CimInstance Win32_PageFileSetting -ErrorAction Stop | ForEach-Object { [string]$_.Name } | Where-Object { $_ })
    $registry = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' -ErrorAction Stop
    $active = @($usage) + @($registry.ExistingPageFiles | Where-Object { $_ })
    $configured = @($settings) + @($registry.PagingFiles | Where-Object { $_ })
    foreach ($path in @($active) + @($configured)) {
        if ($path -match '(?i)\?:\\') {
            if ($active.Count -eq 0) { throw 'Wildcard page-file location has no active resolved path.' }
            continue
        }
        $number = Resolve-PageFileDisk ([string]$path) $partitions
        if ($number -eq 1 -or $number -eq 2) {
            throw "Page file '$path' is on target Disk $number."
        }
    }
}

function Assert-GlobalSafety {
    Assert-NoPMount
    Assert-NoTargetPageFile
}

function Assert-Disk($spec) {
    if ($spec.Number -eq 0 -or $spec.Number -notin @(1, 2)) { throw 'Target must be Disk 1 or Disk 2.' }
    $disk = Get-Disk -Number $spec.Number -ErrorAction Stop
    if (-not (Same ([string]$disk.UniqueId) $spec.UniqueId) -or
        -not (Same ([string]$disk.Guid) $spec.DiskGuid) -or
        -not (Same ([string]$disk.Path) $spec.DevicePath) -or
        [uint64]$disk.Size -ne $spec.DiskBytes -or
        [string]$disk.PartitionStyle -ne 'GPT' -or
        [string]$disk.FriendlyName -ne 'Msft Virtual Disk' -or
        $disk.IsBoot -or $disk.IsSystem -or $disk.IsOffline -or $disk.IsReadOnly) {
        throw "Disk $($spec.Number) identity, GPT, geometry, or safety state changed."
    }
    return $disk
}

function Assert-Partition($actual, $expected, [string]$type, [uint32]$diskNumber) {
    if ([uint32]$actual.DiskNumber -ne $diskNumber -or
        [uint32]$actual.PartitionNumber -ne [uint32]$expected.Number -or
        -not (Same ([string]$actual.Guid) ([string]$expected.Guid)) -or
        -not (Same ([string]$actual.GptType) $type) -or
        [uint64]$actual.Offset -ne [uint64]$expected.Offset -or
        [uint64]$actual.Size -ne [uint64]$expected.Size -or
        $actual.IsBoot -or $actual.IsSystem -or
        [int][char]$actual.DriveLetter -ne 0) {
        throw "Disk $diskNumber partition $($expected.Number) identity or geometry changed."
    }
}

function Assert-Layout($spec, [object[]]$expectedData) {
    $null = Assert-Disk $spec
    $parts = @(Get-Partition -DiskNumber $spec.Number -ErrorAction Stop)
    if ($parts.Count -ne (1 + $expectedData.Count)) {
        throw "Disk $($spec.Number) has an unexpected number of partitions."
    }
    $msr = @($parts | Where-Object { $_.PartitionNumber -eq $spec.Msr.Number })
    if ($msr.Count -ne 1) { throw "Disk $($spec.Number) MSR partition is missing." }
    Assert-Partition $msr[0] $spec.Msr $msrType $spec.Number
    if (@($msr[0].AccessPaths | Where-Object { $_ }).Count -ne 0) {
        throw "Disk $($spec.Number) MSR has an unexpected access path."
    }
    foreach ($expected in $expectedData) {
        $found = @($parts | Where-Object { $_.PartitionNumber -eq $expected.Number })
        if ($found.Count -ne 1) { throw "Disk $($spec.Number) has an unexpected data partition." }
        Assert-Partition $found[0] $expected $basicData $spec.Number
        $paths = @($found[0].AccessPaths | Where-Object { $_ })
        if ($paths.Count -ne 1 -or
            $paths[0] -notmatch '^\\\\\?\\Volume\{[0-9a-f-]{36}\}\\$') {
            throw "Disk $($spec.Number) data partition has unexpected access paths."
        }
    }
    return $parts
}

function Add-DataPartition($spec, $plan, [object[]]$alreadyCreated) {
    Assert-GlobalSafety
    $null = Assert-Layout $spec $alreadyCreated
    if ($plan.Offset -lt $firstOffset -or $plan.Offset % 4096 -ne 0 -or
        $plan.Size -lt 1GB -or $plan.Size % 4096 -ne 0 -or
        $plan.Offset + $plan.Size -gt $spec.DiskBytes - 1MB) {
        throw 'Planned partition geometry is unsafe.'
    }
    $new = New-Partition -DiskNumber $spec.Number -Offset $plan.Offset -Size $plan.Size -GptType $basicData -ErrorAction Stop
    $null = Set-Partition -DiskNumber $spec.Number -PartitionNumber $new.PartitionNumber -NoDefaultDriveLetter $true -Confirm:$false -ErrorAction Stop
    # Mount Manager can register the new volume GUID path a moment after the
    # partition itself becomes visible.
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        $created = Get-Partition -DiskNumber $spec.Number -PartitionNumber $new.PartitionNumber -ErrorAction Stop
        if (@($created.AccessPaths | Where-Object { $_ }).Count -gt 0) { break }
        Start-Sleep -Milliseconds 200
    }
    $record = [pscustomobject]@{
        Number = [uint32]$created.PartitionNumber
        Guid = [string]$created.Guid
        Offset = [uint64]$plan.Offset
        Size = [uint64]$plan.Size
    }
    if ([string]::IsNullOrWhiteSpace($record.Guid)) { throw 'New GPT partition has no PartitionId.' }
    $null = Assert-Layout $spec (@($alreadyCreated) + @($record))
    return $record
}

function Get-CreatedResult($spec, [object[]]$created) {
    $null = Assert-Layout $spec $created
    foreach ($entry in $created) {
        $part = Get-Partition -DiskNumber $spec.Number -PartitionNumber $entry.Number -ErrorAction Stop
        $path = @($part.AccessPaths | Where-Object { $_ })[0]
        [pscustomobject]@{
            DiskNumber = [uint32]$spec.Number
            DiskUniqueId = $spec.UniqueId
            DiskGuid = $spec.DiskGuid
            PartitionNumber = [uint32]$part.PartitionNumber
            PartitionId = [string]$part.Guid
            OffsetBytes = [uint64]$part.Offset
            SizeBytes = [uint64]$part.Size
            VolumeGuidPath = [string]$path
            VolumeDevicePath = ([string]$path).TrimEnd('\')
        }
    }
}

Assert-GlobalSafety
foreach ($spec in $targets) { $null = Assert-Layout $spec @($spec.OldData) }

if (-not $Apply) {
    $plan = @($targets | ForEach-Object {
        $spec = $_
        [pscustomobject]@{
            DiskNumber = $spec.Number
            DiskUniqueId = $spec.UniqueId
            DiskGuid = $spec.DiskGuid
            RemovePartitionId = $spec.OldData.Guid
            RemoveOffsetBytes = $spec.OldData.Offset
            RemoveSizeBytes = $spec.OldData.Size
            PreserveMsrPartitionId = $spec.Msr.Guid
            Create = @($spec.Planned | ForEach-Object { [pscustomobject]@{ OffsetBytes = $_.Offset; SizeBytes = $_.Size } })
        }
    })
    [pscustomobject]@{ Status = 'DryRun'; Plan = $plan } | ConvertTo-Json -Depth 8
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '-Apply requires elevated PowerShell.'
}

# Each write is preceded by a fresh check. Any failure leaves a partial layout
# for manual inspection; this script will not accept it on the next run.
try {
    foreach ($spec in $targets) {
        Assert-GlobalSafety
        $null = Assert-Layout $spec @($spec.OldData)
        $null = Remove-Partition -DiskNumber $spec.Number -PartitionNumber $spec.OldData.Number -Confirm:$false -ErrorAction Stop
        $null = Assert-Layout $spec @()
    }

    $createdByDisk = @{}
    foreach ($spec in $targets) {
        $created = @()
        foreach ($plan in $spec.Planned) {
            $created += Add-DataPartition $spec $plan $created
        }
        $createdByDisk[$spec.Number] = $created
    }
    Assert-GlobalSafety
    $result = @($targets | ForEach-Object { Get-CreatedResult $_ $createdByDisk[$_.Number] })
    [pscustomobject]@{ Status = 'Applied'; DataPartitions = $result } | ConvertTo-Json -Depth 8
} catch {
    throw "Reconfiguration stopped. Inspect both disk layouts before any retry: $($_.Exception.Message)"
}
