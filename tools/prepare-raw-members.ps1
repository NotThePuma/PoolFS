# Prepare exactly two empty disks for PoolFS raw-member testing. The default
# mode is read-only. -Apply requires an elevated PowerShell session and writes
# GPT plus one unformatted basic-data partition to each selected disk.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$FirstDiskUniqueId,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$SecondDiskUniqueId,

    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$basicDataGptType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$msrGptType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'

function Assert-EmptyTarget([string]$uniqueId) {
    $matches = @(Get-Disk -ErrorAction Stop | Where-Object {
        [string]::Equals([string]$_.UniqueId, $uniqueId,
            [StringComparison]::OrdinalIgnoreCase)
    })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one disk with UniqueId '$uniqueId'; found $($matches.Count)."
    }
    $disk = $matches[0]
    if ($disk.Number -eq 0 -or $disk.IsBoot -or $disk.IsSystem) {
        throw "Disk $($disk.Number) is a boot/system disk or Disk 0; refusing to provision it."
    }
    if ($disk.IsOffline -or $disk.IsReadOnly) {
        throw "Disk $($disk.Number) is offline or read-only; refusing to change its state."
    }
    if ([string]$disk.PartitionStyle -ne 'RAW') {
        throw "Disk $($disk.Number) is already $($disk.PartitionStyle), so it is not a new empty disk."
    }
    if ([uint64]$disk.Size -lt 1GB) {
        throw "Disk $($disk.Number) is smaller than the 1 GiB test minimum."
    }
    $partitions = @(Get-CimInstance -Namespace 'root/Microsoft/Windows/Storage' -ClassName MSFT_Partition -Filter "DiskNumber=$($disk.Number)" -ErrorAction Stop)
    if ($partitions.Count -ne 0) {
        throw "Disk $($disk.Number) already has $($partitions.Count) partition(s)."
    }
    return $disk
}

function Assert-SameTarget([string]$uniqueId, [uint32]$number, [uint64]$size) {
    $disk = Assert-EmptyTarget $uniqueId
    if ($disk.Number -ne $number -or [uint64]$disk.Size -ne $size) {
        throw "Disk '$uniqueId' changed disk number or size since preview; refusing to provision."
    }
    return $disk
}

function Assert-OnlyAutomaticMsr([uint32]$number) {
    $parts = @(Get-CimInstance -Namespace 'root/Microsoft/Windows/Storage' -ClassName MSFT_Partition -Filter "DiskNumber=$number" -ErrorAction Stop)
    if ($parts.Count -gt 1 -or
        ($parts.Count -eq 1 -and [string]$parts[0].GptType -ne $msrGptType)) {
        throw "Disk $number has unexpected partition(s) after GPT initialization; refusing to add another."
    }
}

function Get-PreparedDisk([string]$uniqueId, [uint32]$number, [uint64]$size) {
    $disk = Get-Disk -Number $number -ErrorAction Stop
    if (-not [string]::Equals([string]$disk.UniqueId, $uniqueId,
            [StringComparison]::OrdinalIgnoreCase) -or
        [uint64]$disk.Size -ne $size -or
        [string]$disk.PartitionStyle -ne 'GPT' -or
        $disk.IsBoot -or $disk.IsSystem -or $disk.IsOffline -or $disk.IsReadOnly -or
        [string]::IsNullOrWhiteSpace([string]$disk.Guid)) {
        throw "Disk $number failed GPT identity or state verification after initialization."
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

if ([string]::Equals($FirstDiskUniqueId, $SecondDiskUniqueId,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'First and second disk unique IDs must be different.'
}

$first = Assert-EmptyTarget $FirstDiskUniqueId
$second = Assert-EmptyTarget $SecondDiskUniqueId
if ($first.Number -eq $second.Number) { throw 'Both IDs resolved to the same disk.' }

foreach ($disk in @($first, $second)) {
    $sizeGiB = [math]::Round([uint64]$disk.Size / 1GB, 2)
    "Disk $($disk.Number): $($disk.FriendlyName), $sizeGiB GiB, UniqueId $($disk.UniqueId), RAW, no partitions"
}

if (-not $Apply) {
    'Read-only preview complete. Re-run with -Apply in elevated PowerShell to create GPT partitions on exactly these disks.'
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '-Apply requires an elevated PowerShell session. No disk changes were made.'
}

# A fresh check immediately precedes every disk initialization. If any later
# command fails, stop; a partial GPT/partition state must be inspected manually.
$first = Assert-SameTarget $FirstDiskUniqueId $first.Number ([uint64]$first.Size)
$second = Assert-SameTarget $SecondDiskUniqueId $second.Number ([uint64]$second.Size)
$firstSize = [uint64]$first.Size
$secondSize = [uint64]$second.Size

try {
    $null = Initialize-Disk -Number $first.Number -PartitionStyle GPT -Confirm:$false -ErrorAction Stop
    $first = Get-PreparedDisk $FirstDiskUniqueId $first.Number $firstSize

    $second = Assert-SameTarget $SecondDiskUniqueId $second.Number ([uint64]$second.Size)
    $null = Initialize-Disk -Number $second.Number -PartitionStyle GPT -Confirm:$false -ErrorAction Stop
    $second = Get-PreparedDisk $SecondDiskUniqueId $second.Number $secondSize

    # Windows may create one Microsoft Reserved partition during Initialize-Disk.
    Assert-OnlyAutomaticMsr $first.Number
    $first = Get-PreparedDisk $FirstDiskUniqueId $first.Number $firstSize
    $firstPartitionSize = Get-AlignedPartitionSize $first
    $firstPartition = New-Partition -DiskNumber $first.Number -Size $firstPartitionSize -GptType $basicDataGptType -ErrorAction Stop
    if ([uint64]$firstPartition.Size -ne $firstPartitionSize) { throw "Disk $($first.Number) partition size differs from the aligned request." }
    $null = Set-Partition -DiskNumber $first.Number -PartitionNumber $firstPartition.PartitionNumber -NoDefaultDriveLetter $true -Confirm:$false -ErrorAction Stop

    Assert-OnlyAutomaticMsr $second.Number
    $second = Get-PreparedDisk $SecondDiskUniqueId $second.Number $secondSize
    $secondPartitionSize = Get-AlignedPartitionSize $second
    $secondPartition = New-Partition -DiskNumber $second.Number -Size $secondPartitionSize -GptType $basicDataGptType -ErrorAction Stop
    if ([uint64]$secondPartition.Size -ne $secondPartitionSize) { throw "Disk $($second.Number) partition size differs from the aligned request." }
    $null = Set-Partition -DiskNumber $second.Number -PartitionNumber $secondPartition.PartitionNumber -NoDefaultDriveLetter $true -Confirm:$false -ErrorAction Stop

    $result = @(
        @($first, $second) | ForEach-Object {
            $expectedSize = if ($_.Number -eq $first.Number) { $firstSize } else { $secondSize }
            $disk = Get-PreparedDisk ([string]$_.UniqueId) $_.Number $expectedSize
            $parts = @(Get-Partition -DiskNumber $disk.Number -ErrorAction Stop)
            $basicParts = @($parts | Where-Object { [string]$_.GptType -eq $basicDataGptType })
            $msrParts = @($parts | Where-Object { [string]$_.GptType -eq $msrGptType })
            if ($basicParts.Count -ne 1 -or $msrParts.Count -gt 1 -or
                $parts.Count -ne ($basicParts.Count + $msrParts.Count) -or
                [int][char]$basicParts[0].DriveLetter -ne 0 -or
                [uint64]$basicParts[0].Size % 4096 -ne 0) {
                throw "Disk $($disk.Number) has an unexpected partition layout after provisioning."
            }
            $volumePaths = @($basicParts[0].AccessPaths | Where-Object {
                $_ -and $_.StartsWith('\\?\Volume{', [StringComparison]::OrdinalIgnoreCase)
            })
            if ($volumePaths.Count -ne 1) {
                throw "Disk $($disk.Number) has no unique volume GUID path; inspect it before raw formatting."
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
    )
    $result | Format-List
    'GPT partitions are ready. No filesystem was formatted and no PoolFS metadata was written.'
} catch {
    Write-Warning "Provisioning stopped after a disk command failed: $($_.Exception.Message) Inspect both disk layouts before any retry; this script will refuse a partially provisioned disk."
    throw
}
