# Recovery helper for the two PoolFS test VHDX disks left with GPT plus the
# automatic Microsoft Reserved partition after Initialize-Disk. Default is
# read-only; -Apply adds one unformatted basic-data partition to each disk.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$FirstDiskUniqueId,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$FirstDiskGuid,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$SecondDiskUniqueId,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$SecondDiskGuid,

    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$basicDataGptType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$msrGptType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
$knownFirst = [pscustomobject]@{
    UniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
    DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
    SizeBytes = [uint64]10737418240
}
$knownSecond = [pscustomobject]@{
    UniqueId = '60022480542A978B5E84E922C041AF43'
    DiskGuid = '{b0644b17-4e34-4604-a371-d2900c0dfbb7}'
    SizeBytes = [uint64]8589934592
}

function Assert-ExpectedInput([string]$uniqueId, [string]$guid, $known) {
    if (-not [string]::Equals($uniqueId, $known.UniqueId, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals($guid, $known.DiskGuid, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'This recovery helper is restricted to the two verified test VHDX disks and their current GPT disk GUIDs.'
    }
}

function Assert-MsrOnlyTarget([string]$uniqueId, [string]$expectedGuid, [uint64]$expectedSize) {
    $matches = @(Get-Disk -ErrorAction Stop | Where-Object {
        [string]::Equals([string]$_.UniqueId, $uniqueId, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($matches.Count -ne 1) { throw "Expected exactly one disk with UniqueId '$uniqueId'; found $($matches.Count)." }
    $disk = $matches[0]
    if ($disk.Number -eq 0 -or $disk.IsBoot -or $disk.IsSystem -or
        $disk.IsOffline -or $disk.IsReadOnly -or
        [string]$disk.PartitionStyle -ne 'GPT' -or
        [uint64]$disk.Size -ne $expectedSize -or
        -not [string]::Equals([string]$disk.Guid, $expectedGuid, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Disk $($disk.Number) failed GPT identity, size, or safety checks."
    }
    $parts = @(Get-Partition -DiskNumber $disk.Number -ErrorAction Stop)
    if ($parts.Count -ne 1 -or [string]$parts[0].GptType -ne $msrGptType -or
        [int][char]$parts[0].DriveLetter -ne 0 -or
        @($parts[0].AccessPaths | Where-Object { $_ }).Count -ne 0 -or
        [uint64]$parts[0].Size -lt 8MB -or [uint64]$parts[0].Size -gt 32MB) {
        throw "Disk $($disk.Number) must have only one unmounted 8-32 MiB Microsoft Reserved partition."
    }
    return $disk
}

function Get-AlignedPartitionSize($disk) {
    $free = [uint64]$disk.LargestFreeExtent
    if ($free -lt (1GB + 1MB)) { throw "Disk $($disk.Number) has insufficient free space for an aligned member." }
    $planned = $free - 1MB
    $planned -= $planned % 1MB
    if ($planned -lt 1GB -or $planned % 4096 -ne 0) { throw "Disk $($disk.Number) has no valid aligned member size." }
    return [uint64]$planned
}

function Get-FinishedMember([string]$uniqueId, [string]$expectedGuid, [uint64]$expectedSize, [uint32]$number) {
    $disk = Get-Disk -Number $number -ErrorAction Stop
    if ($disk.Number -eq 0 -or $disk.IsBoot -or $disk.IsSystem -or
        $disk.IsOffline -or $disk.IsReadOnly -or
        [string]$disk.PartitionStyle -ne 'GPT' -or
        [uint64]$disk.Size -ne $expectedSize -or
        -not [string]::Equals([string]$disk.UniqueId, $uniqueId, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals([string]$disk.Guid, $expectedGuid, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Disk $number changed identity or safety state after partition creation."
    }
    $parts = @(Get-Partition -DiskNumber $number -ErrorAction Stop)
    $basicParts = @($parts | Where-Object { [string]$_.GptType -eq $basicDataGptType })
    $msrParts = @($parts | Where-Object { [string]$_.GptType -eq $msrGptType })
    if ($parts.Count -ne 2 -or $basicParts.Count -ne 1 -or $msrParts.Count -ne 1 -or
        [int][char]$basicParts[0].DriveLetter -ne 0 -or
        [uint64]$basicParts[0].Size % 4096 -ne 0) {
        throw "Disk $number has an unexpected partition layout after completion."
    }
    $volumePaths = @($basicParts[0].AccessPaths | Where-Object {
        $_ -and $_.StartsWith('\\?\Volume{', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($volumePaths.Count -ne 1) {
        throw "Disk $number has no unique volume GUID path; inspect it before raw formatting."
    }
    [pscustomobject]@{
        DiskNumber = $disk.Number
        DiskUniqueId = $disk.UniqueId
        DiskGuid = $disk.Guid
        PartitionNumber = $basicParts[0].PartitionNumber
        PartitionBytes = [uint64]$basicParts[0].Size
        VolumeGuidPath = $volumePaths[0]
        VolumeDevicePath = $volumePaths[0].TrimEnd('\')
    }
}

Assert-ExpectedInput $FirstDiskUniqueId $FirstDiskGuid $knownFirst
Assert-ExpectedInput $SecondDiskUniqueId $SecondDiskGuid $knownSecond

$first = Assert-MsrOnlyTarget $FirstDiskUniqueId $FirstDiskGuid $knownFirst.SizeBytes
$second = Assert-MsrOnlyTarget $SecondDiskUniqueId $SecondDiskGuid $knownSecond.SizeBytes
if ($first.Number -eq $second.Number) { throw 'Both IDs resolved to the same disk.' }

foreach ($disk in @($first, $second)) {
    $sizeGiB = [math]::Round([uint64]$disk.Size / 1GB, 2)
    $plannedSize = Get-AlignedPartitionSize $disk
    "Disk $($disk.Number): $sizeGiB GiB, UniqueId $($disk.UniqueId), GPT disk GUID $($disk.Guid), one MSR partition, planned data partition $plannedSize bytes"
}

if (-not $Apply) {
    'Read-only preview complete. -Apply in elevated PowerShell will add one unformatted basic-data partition to each disk.'
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '-Apply requires an elevated PowerShell session. No disk changes were made.'
}

try {
    # Revalidate immediately before each partition write. A partial result
    # after the first write is intentionally not resumable by this helper.
    $first = Assert-MsrOnlyTarget $FirstDiskUniqueId $FirstDiskGuid $knownFirst.SizeBytes
    $second = Assert-MsrOnlyTarget $SecondDiskUniqueId $SecondDiskGuid $knownSecond.SizeBytes

    $firstPartitionSize = Get-AlignedPartitionSize $first
    $firstPartition = New-Partition -DiskNumber $first.Number -Size $firstPartitionSize -GptType $basicDataGptType -ErrorAction Stop
    if ([uint64]$firstPartition.Size -ne $firstPartitionSize) { throw "Disk $($first.Number) partition size differs from the aligned request." }
    $null = Set-Partition -DiskNumber $first.Number -PartitionNumber $firstPartition.PartitionNumber -NoDefaultDriveLetter $true -Confirm:$false -ErrorAction Stop

    $second = Assert-MsrOnlyTarget $SecondDiskUniqueId $SecondDiskGuid $knownSecond.SizeBytes
    $secondPartitionSize = Get-AlignedPartitionSize $second
    $secondPartition = New-Partition -DiskNumber $second.Number -Size $secondPartitionSize -GptType $basicDataGptType -ErrorAction Stop
    if ([uint64]$secondPartition.Size -ne $secondPartitionSize) { throw "Disk $($second.Number) partition size differs from the aligned request." }
    $null = Set-Partition -DiskNumber $second.Number -PartitionNumber $secondPartition.PartitionNumber -NoDefaultDriveLetter $true -Confirm:$false -ErrorAction Stop

    Get-FinishedMember $FirstDiskUniqueId $FirstDiskGuid $knownFirst.SizeBytes $first.Number
    Get-FinishedMember $SecondDiskUniqueId $SecondDiskGuid $knownSecond.SizeBytes $second.Number
    'Both partitions are ready. No filesystem was formatted and no PoolFS metadata was written.'
} catch {
    Write-Warning "Completion stopped: $($_.Exception.Message) Inspect both disk layouts before retrying; this helper refuses a partial result."
    throw
}
