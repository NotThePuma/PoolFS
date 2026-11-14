# PoolFS Manager host-partition operations. Preview is read-only; -Apply repeats
# every guard immediately before using Windows Storage cmdlets. This script
# never writes PoolFS metadata or operates on direct raw PoolFS members.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('InitializeGpt', 'CreateAndFormat', 'Format', 'Remove', 'ChangeLetter')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [uint32]$DiskNumber,

    [Parameter(Mandatory = $true)]
    [uint64]$ExpectedDiskBytes,

    [string]$ExpectedDiskGuid = '',
    [uint32]$ExpectedPartitionNumber = 0,
    [string]$ExpectedPartitionGuid = '',
    [uint64]$ExpectedPartitionOffset = 0,
    [uint64]$ExpectedPartitionBytes = 0,
    [uint64]$SizeMiB = 0,
    [string]$DriveLetter = '',
    [ValidateSet('NTFS', 'exFAT', 'FAT32')]
    [string]$FileSystem = 'NTFS',
    [string]$Label = '',
    [string]$ExpectedDiskToken = '',
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$basicDataType = 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7'
$efiType = 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
$recoveryType = 'de94bba4-06d1-4d40-a16a-bfd50179d6ac'
$mib = [uint64]1048576

function Normalize-Guid([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    try { return ([guid]$Value).ToString('D').ToLowerInvariant() }
    catch { throw "Invalid GUID in selected disk or partition identity: '$Value'." }
}

function Get-PageFileLetters {
    $letters = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($class in @('Win32_PageFileUsage', 'Win32_PageFileSetting')) {
        # A failed query must not make a page-file disk appear safe.
        $items = @(Get-CimInstance -ClassName $class -ErrorAction Stop)
        foreach ($item in $items) {
            $path = [string]$item.Name
            if ([string]::IsNullOrWhiteSpace($path)) { continue }
            if ($path -notmatch '^([A-Za-z]):\\') {
                throw "Cannot identify the drive of configured page file '$path'."
            }
            $null = $letters.Add($Matches[1].ToUpperInvariant())
        }
    }
    return ,$letters
}

function Assert-SafeDisk {
    if ($ExpectedDiskBytes -eq 0) { throw 'Expected disk size is missing.' }
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ($disk.Number -ne $DiskNumber -or [uint64]$disk.Size -ne $ExpectedDiskBytes) {
        throw "Disk $DiskNumber number or size changed since selection."
    }
    if ($disk.Number -eq 0 -or $disk.IsBoot -or $disk.IsSystem) {
        throw "Disk $DiskNumber is Disk 0 or a boot/system disk; refusing partition changes."
    }
    if ($disk.IsOffline -or $disk.IsReadOnly) {
        throw "Disk $DiskNumber is offline or read-only."
    }
    if ($disk.IsClustered) {
        throw "Disk $DiskNumber is clustered; refusing local partition changes."
    }
    $uniqueId = [string]$disk.UniqueId
    if ([string]::IsNullOrWhiteSpace($uniqueId)) {
        throw "Disk $DiskNumber has no usable Windows UniqueId."
    }
    $token = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($uniqueId))
    if ($Apply -and (-not $ExpectedDiskToken -or
            -not [string]::Equals($ExpectedDiskToken, $token,
                [StringComparison]::Ordinal))) {
        throw "Disk $DiskNumber identity changed after preview; no changes were made."
    }

    if ($Action -eq 'InitializeGpt') {
        if ([string]$disk.PartitionStyle -ne 'RAW' -or $ExpectedDiskGuid) {
            throw "Disk $DiskNumber is not the selected RAW disk."
        }
    } else {
        if ([string]$disk.PartitionStyle -ne 'GPT' -or
            -not $ExpectedDiskGuid -or
            (Normalize-Guid ([string]$disk.Guid)) -ne
                (Normalize-Guid $ExpectedDiskGuid)) {
            throw "Disk $DiskNumber GPT identity changed since selection."
        }
    }

    $parts = @(Get-CimInstance -Namespace 'root/Microsoft/Windows/Storage' -ClassName MSFT_Partition -Filter "DiskNumber=$DiskNumber" -ErrorAction Stop)
    $pageLetters = Get-PageFileLetters
    foreach ($part in $parts) {
        $type = Normalize-Guid ([string]$part.GptType)
        if ($part.IsBoot -or $part.IsSystem -or $type -eq $efiType -or
            $type -eq $recoveryType) {
            throw "Disk $DiskNumber contains a protected boot, system, EFI, or recovery partition."
        }
        $letter = [string]$part.DriveLetter
        if ($letter.Length -eq 1 -and $pageLetters.Contains($letter)) {
            throw "Disk $DiskNumber contains a configured page file."
        }
    }
    if ($Action -eq 'InitializeGpt' -and $parts.Count -ne 0) {
        throw "RAW Disk $DiskNumber already has partition objects."
    }
    return [pscustomobject]@{ Disk = $disk; Parts = $parts; Token = $token }
}

function Assert-SelectedPartition($diskState) {
    if ($ExpectedPartitionNumber -eq 0 -or -not $ExpectedPartitionGuid -or
        $ExpectedPartitionBytes -eq 0) {
        throw 'Selected partition identity is incomplete.'
    }
    $matches = @($diskState.Parts | Where-Object {
        [uint32]$_.PartitionNumber -eq $ExpectedPartitionNumber
    })
    if ($matches.Count -ne 1) {
        throw "Partition $ExpectedPartitionNumber is no longer on Disk $DiskNumber."
    }
    $part = $matches[0]
    if ((Normalize-Guid ([string]$part.Guid)) -ne
            (Normalize-Guid $ExpectedPartitionGuid) -or
        [uint64]$part.Offset -ne $ExpectedPartitionOffset -or
        [uint64]$part.Size -ne $ExpectedPartitionBytes -or
        (Normalize-Guid ([string]$part.GptType)) -ne $basicDataType -or
        $part.IsBoot -or $part.IsSystem -or $part.IsHidden -or
        $part.IsReadOnly -or $part.IsOffline) {
        throw "Partition $ExpectedPartitionNumber identity, type, or state changed."
    }
    $selected = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $ExpectedPartitionNumber -ErrorAction Stop
    if ((Normalize-Guid ([string]$selected.Guid)) -ne
            (Normalize-Guid $ExpectedPartitionGuid) -or
        [uint64]$selected.Offset -ne $ExpectedPartitionOffset -or
        [uint64]$selected.Size -ne $ExpectedPartitionBytes -or
        (Normalize-Guid ([string]$selected.GptType)) -ne $basicDataType) {
        throw "Partition $ExpectedPartitionNumber changed while resolving its Storage object."
    }
    return $selected
}

function Assert-DriveLetter([bool]$MustBeUnused) {
    if ($DriveLetter -notmatch '^[A-Za-z]$') {
        throw 'A single A-Z drive letter is required.'
    }
    $letter = $DriveLetter.ToUpperInvariant()
    if ($MustBeUnused) {
        $claimed = @(Get-Partition -DriveLetter $letter -ErrorAction SilentlyContinue)
        if ($claimed.Count -gt 0 -or
            [IO.DriveInfo]::GetDrives().Name -contains "${letter}:\") {
            throw "Drive letter ${letter}: is already in use."
        }
    }
    return $letter
}

function Assert-FormatOptions {
    if ($Label.Length -gt 32 -or $Label -match '[\x00-\x1f\\/:*?"<>|]') {
        throw 'Volume label is too long or contains invalid characters.'
    }
    if ($FileSystem -eq 'FAT32' -and $Label.Length -gt 11) {
        throw 'FAT32 volume labels cannot exceed 11 characters.'
    }
}

function Assert-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Partition changes require PoolFS Manager to run as Administrator. No changes were made.'
    }
}

function Assert-NoPoolFsMount {
    # A host partition may hold a mounted member image. Keep all partition
    # mutations unavailable until every PoolFS mount process has exited.
    $active = @(Get-CimInstance -ClassName Win32_Process -Filter "Name='poolfs-mount.exe'" -ErrorAction Stop)
    if ($active.Count -ne 0) {
        throw 'Unmount all PoolFS drives before changing host partitions.'
    }
}

# Read-only preview and apply use the same checks. Apply re-runs them in a fresh
# PowerShell process with the preview's UniqueId token to detect target swaps.
Assert-NoPoolFsMount
$state = Assert-SafeDisk
$partition = $null
$letter = ''
switch ($Action) {
    'InitializeGpt' {
        if ([uint64]$state.Disk.Size -lt 1GB) {
            throw 'A new GPT disk must be at least 1 GiB.'
        }
        $description = "Initialize RAW Disk $DiskNumber ($([math]::Round($state.Disk.Size / 1GB, 2)) GiB) as GPT"
    }
    'CreateAndFormat' {
        Assert-FormatOptions
        $letter = Assert-DriveLetter $true
        if ($SizeMiB -gt [uint64]::MaxValue / $mib) {
            throw 'Requested partition size is too large.'
        }
        if ($SizeMiB -ne 0 -and $SizeMiB -lt 512) {
            throw 'New PoolFS host partitions must be at least 512 MiB.'
        }
        $free = [uint64]$state.Disk.LargestFreeExtent
        if ($free -lt 512MB -or ($SizeMiB -ne 0 -and
                $SizeMiB * $mib -gt $free)) {
            throw "Disk $DiskNumber does not have enough unallocated space."
        }
        if ($FileSystem -eq 'FAT32' -and
            (($SizeMiB -eq 0 -and $free -gt 32GB) -or
             ($SizeMiB -ne 0 -and $SizeMiB * $mib -gt 32GB))) {
            throw 'PoolFS Manager limits FAT32 partition formatting to 32 GiB; choose NTFS or exFAT for larger members.'
        }
        $sizeText = if ($SizeMiB -eq 0) { 'maximum available' } else { "$SizeMiB MiB" }
        $labelText = if ($Label) { $Label } else { '(empty)' }
        $description = "Create $sizeText GPT basic-data partition on Disk $DiskNumber, assign ${letter}:, quick-format $FileSystem, label '$labelText'"
        if ($FileSystem -eq 'FAT32') { $description += '; FAT32 files cannot exceed 4 GiB' }
    }
    'Format' {
        Assert-FormatOptions
        $partition = Assert-SelectedPartition $state
        if ($FileSystem -eq 'FAT32' -and [uint64]$partition.Size -gt 32GB) {
            throw 'PoolFS Manager limits FAT32 partition formatting to 32 GiB; choose NTFS or exFAT for larger members.'
        }
        $labelText = if ($Label) { $Label } else { '(empty)' }
        $description = "ERASE and quick-format Disk $DiskNumber Partition $ExpectedPartitionNumber ($([math]::Round($partition.Size / 1GB, 2)) GiB) as $FileSystem, label '$labelText'"
        if ($FileSystem -eq 'FAT32') { $description += '; FAT32 files cannot exceed 4 GiB' }
    }
    'Remove' {
        $partition = Assert-SelectedPartition $state
        $description = "DELETE Disk $DiskNumber Partition $ExpectedPartitionNumber ($([math]::Round($partition.Size / 1GB, 2)) GiB) and its volume"
    }
    'ChangeLetter' {
        $partition = Assert-SelectedPartition $state
        $letter = Assert-DriveLetter $true
        $description = "Change Disk $DiskNumber Partition $ExpectedPartitionNumber to drive ${letter}:"
    }
}

$diskIdentity = if ($Action -eq 'InitializeGpt') {
    "RAW UniqueId '$([string]$state.Disk.UniqueId)', size $([uint64]$state.Disk.Size) bytes"
} else {
    "GPT disk GUID '$([string]$state.Disk.Guid)', size $([uint64]$state.Disk.Size) bytes"
}
$partIdentity = ''
if ($partition) {
    $oldLetter = [string]$partition.DriveLetter
    if ($oldLetter -notmatch '^[A-Za-z]$') { $oldLetter = '(none)' }
    else { $oldLetter = $oldLetter.ToUpperInvariant() + ':' }
    $partIdentity = "; partition GUID '$([string]$partition.Guid)', offset $([uint64]$partition.Offset) bytes, size $([uint64]$partition.Size) bytes, current letter $oldLetter"
}
"POOLFS_DISK_TOKEN=$($state.Token)"
"PREVIEW: $description; $diskIdentity$partIdentity"
if (-not $Apply) { return }

Assert-Elevated
Assert-NoPoolFsMount
# Resolve target once more immediately before the Storage mutation. This
# catches disk hot-plug and partition-number reuse during the preview dialog.
$state = Assert-SafeDisk
if ($Action -in @('Format', 'Remove', 'ChangeLetter')) {
    $partition = Assert-SelectedPartition $state
}
if ($Action -eq 'CreateAndFormat') {
    $free = [uint64]$state.Disk.LargestFreeExtent
    if ($free -lt 512MB -or ($SizeMiB -ne 0 -and
            $SizeMiB * $mib -gt $free) -or
        ($FileSystem -eq 'FAT32' -and $SizeMiB -eq 0 -and $free -gt 32GB)) {
        throw "Disk $DiskNumber free extent changed after preview; no partition was created."
    }
}
if ($Action -in @('CreateAndFormat', 'ChangeLetter')) {
    $letter = Assert-DriveLetter $true
}

switch ($Action) {
    'InitializeGpt' {
        $null = Initialize-Disk -Number $DiskNumber -PartitionStyle GPT -Confirm:$false -ErrorAction Stop
        "DONE: Initialized Disk $DiskNumber as GPT."
    }
    'CreateAndFormat' {
        $parameters = @{
            DiskNumber = $DiskNumber
            DriveLetter = [char]$letter
            GptType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
            ErrorAction = 'Stop'
        }
        if ($SizeMiB -eq 0) { $parameters.UseMaximumSize = $true }
        else { $parameters.Size = [uint64]($SizeMiB * $mib) }
        $created = New-Partition @parameters
        try {
            $format = @{
                Partition = $created
                FileSystem = $FileSystem
                Force = $true
                Confirm = $false
                ErrorAction = 'Stop'
            }
            if ($Label) { $format.NewFileSystemLabel = $Label }
            $null = Format-Volume @format
        } catch {
            throw "Partition $($created.PartitionNumber) was created on Disk $DiskNumber, but formatting failed: $($_.Exception.Message) Inspect the new partition before retrying; it was not deleted."
        }
        "DONE: Created and formatted Disk $DiskNumber Partition $($created.PartitionNumber) at ${letter}: as $FileSystem."
    }
    'Format' {
        $format = @{
            Partition = $partition
            FileSystem = $FileSystem
            Force = $true
            Confirm = $false
            ErrorAction = 'Stop'
        }
        if ($Label) { $format.NewFileSystemLabel = $Label }
        $null = Format-Volume @format
        "DONE: Formatted Disk $DiskNumber Partition $ExpectedPartitionNumber as $FileSystem."
    }
    'Remove' {
        $null = Remove-Partition -InputObject $partition -Confirm:$false -ErrorAction Stop
        "DONE: Removed Disk $DiskNumber Partition $ExpectedPartitionNumber."
    }
    'ChangeLetter' {
        $null = Set-Partition -InputObject $partition -NewDriveLetter ([char]$letter) -Confirm:$false -ErrorAction Stop
        "DONE: Assigned ${letter}: to Disk $DiskNumber Partition $ExpectedPartitionNumber."
    }
}
