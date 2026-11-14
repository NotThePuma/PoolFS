# Read-only inventory for selecting future PoolFS test media. This script never
# initializes, partitions, formats, locks, or writes to a disk or volume.
[CmdletBinding()]
param([switch]$Json)

$ErrorActionPreference = 'Stop'

$pageFileDrives = @(
    Get-CimInstance Win32_PageFileUsage -ErrorAction Stop |
        ForEach-Object { if ($_.Name -match '^([A-Za-z]):\\') { $Matches[1].ToUpperInvariant() } }
)

$inventory = @(
    Get-Disk | Sort-Object Number | ForEach-Object {
        $disk = $_
        $partitionQueryError = $null
        try {
            $partitions = @(
                Get-Partition -DiskNumber $disk.Number -ErrorAction Stop |
                    Sort-Object PartitionNumber |
                    ForEach-Object {
                        $partition = $_
                        [pscustomobject]@{
                            PartitionNumber = $partition.PartitionNumber
                            SizeBytes = [uint64]$partition.Size
                            GptType = [string]$partition.GptType
                            DriveLetter = if ([int][char]$partition.DriveLetter -eq 0) { $null } else { [string]$partition.DriveLetter }
                            AccessPaths = @($partition.AccessPaths | Where-Object { $_ })
                            IsBoot = [bool]$partition.IsBoot
                            IsSystem = [bool]$partition.IsSystem
                        }
                    }
            )
        } catch {
            $partitions = @()
            $partitionQueryError = $_.Exception.Message
        }
        $hasPageFile = @($partitions | Where-Object {
            $_.DriveLetter -and $pageFileDrives -contains $_.DriveLetter
        }).Count -gt 0
        [pscustomobject]@{
            DiskNumber = $disk.Number
            FriendlyName = $disk.FriendlyName
            SerialNumber = $disk.SerialNumber
            UniqueId = $disk.UniqueId
            BusType = [string]$disk.BusType
            SizeBytes = [uint64]$disk.Size
            PartitionStyle = [string]$disk.PartitionStyle
            IsBoot = [bool]$disk.IsBoot
            IsSystem = [bool]$disk.IsSystem
            IsOffline = [bool]$disk.IsOffline
            IsReadOnly = [bool]$disk.IsReadOnly
            HasPageFile = $hasPageFile
            PartitionQueryError = $partitionQueryError
            Partitions = $partitions
        }
    }
)

if ($Json) {
    ConvertTo-Json -InputObject $inventory -Depth 6
    return
}

foreach ($disk in $inventory) {
    $sizeGiB = [math]::Round($disk.SizeBytes / 1GB, 2)
    "Disk $($disk.DiskNumber): $($disk.FriendlyName) | $sizeGiB GiB | $($disk.BusType) | $($disk.PartitionStyle)"
    "  Unique ID: $($disk.UniqueId) | Serial: $($disk.SerialNumber)"
    "  Boot=$($disk.IsBoot) System=$($disk.IsSystem) Pagefile=$($disk.HasPageFile) Offline=$($disk.IsOffline) ReadOnly=$($disk.IsReadOnly)"
    if ($disk.PartitionQueryError) { "  Partition query failed: $($disk.PartitionQueryError)" }
    if ($disk.Partitions.Count -eq 0) {
        '  Partitions: (none)'
    } else {
        foreach ($partition in $disk.Partitions) {
            $partitionGiB = [math]::Round($partition.SizeBytes / 1GB, 2)
            $letter = if ($partition.DriveLetter) { "$($partition.DriveLetter):" } else { '(none)' }
            "  Partition $($partition.PartitionNumber): $partitionGiB GiB | $($partition.GptType) | letter $letter | Boot=$($partition.IsBoot) System=$($partition.IsSystem)"
            foreach ($path in $partition.AccessPaths) { "    $path" }
        }
    }
}

if (@($inventory | Where-Object { -not $_.IsBoot -and -not $_.IsSystem }).Count -lt 2) {
    'Fewer than two non-boot, non-system disks are visible. No PoolFS raw pool can be created here.'
}

'Inventory only: non-boot/system status does not establish that a disk is safe to erase.'
