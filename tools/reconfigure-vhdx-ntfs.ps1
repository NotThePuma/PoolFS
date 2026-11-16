# Add one NTFS host partition in unused space on each disposable VHDX disk.
# The existing three-member raw PoolFS pool and each MSR are preserved.
# Dry run by default; -Apply requires the raw P: mount to be stopped.
[CmdletBinding()]
param([switch]$Apply)

$ErrorActionPreference = 'Stop'
$basicData = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$msrType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
$targets = @(
    [pscustomobject]@{
        Number = 1
        UniqueId = '60022480AB8EFFB1ADF43DD1FF8F83A9'
        DiskGuid = '{82f01061-6453-40fe-82b4-a3204208f02f}'
        DevicePath = '\\?\scsi#disk&ven_msft&prod_virtual_disk#5&1adfedf9&0&000001#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}'
        DiskBytes = [uint64]10737418240
        Msr = [pscustomobject]@{ Number=1; Guid='{72ce6dee-9830-4da1-a1be-89f650ee22b4}'; Offset=[uint64]17408; Size=[uint64]16759808 }
        Raw = @(
            [pscustomobject]@{ Number=2; Guid='{4f42f97a-f0c1-4b12-82ae-0a9b72f92c08}'; Offset=[uint64]536870912; Size=[uint64]4294967296 },
            [pscustomobject]@{ Number=3; Guid='{2ffed969-cbbc-464e-b835-3e8544623e70}'; Offset=[uint64]4831838208; Size=[uint64]4294967296 }
        )
        New = [pscustomobject]@{ Offset=[uint64]9126805504; Size=[uint64](1GB); Letter='T'; Label='PoolFS-Host1' }
    },
    [pscustomobject]@{
        Number = 2
        UniqueId = '60022480542A978B5E84E922C041AF43'
        DiskGuid = '{b0644b17-4e34-4604-a371-d2900c0dfbb7}'
        DevicePath = '\\?\scsi#disk&ven_msft&prod_virtual_disk#5&1adfedf9&0&000002#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}'
        DiskBytes = [uint64]8589934592
        Msr = [pscustomobject]@{ Number=1; Guid='{4ea1d2c9-13f0-4813-989f-1d1a1e5fb428}'; Offset=[uint64]17408; Size=[uint64]16759808 }
        Raw = @(
            [pscustomobject]@{ Number=2; Guid='{d746f67b-f9b0-4777-959c-7b8bb19b724e}'; Offset=[uint64]536870912; Size=[uint64]6442450944 }
        )
        New = [pscustomobject]@{ Offset=[uint64]6979321856; Size=[uint64](1GB); Letter='U'; Label='PoolFS-Host2' }
    }
)

function Same([string]$a, [string]$b) {
    return [string]::Equals($a, $b, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PUnmounted {
    if ((Get-PSDrive -Name P -ErrorAction SilentlyContinue) -or
        @(Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='P:'" -ErrorAction Stop).Count -ne 0) {
        throw 'P: is still mounted. Stop the exact raw PoolFS mount before applying.'
    }
}

function Assert-LetterFree([string]$letter) {
    if ((Get-PSDrive -Name $letter -ErrorAction SilentlyContinue) -or
        @(Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($letter):'" -ErrorAction Stop).Count -ne 0 -or
        @(Get-Partition -DriveLetter ([char]$letter) -ErrorAction SilentlyContinue).Count -ne 0 -or
        (Test-Path "${letter}:\" -ErrorAction SilentlyContinue)) {
        throw "Host drive letter ${letter}: is in use."
    }
}

function Resolve-PageFileDisk([string]$path, [object[]]$partitions) {
    if ($path -match '(?i)Volume\{([0-9a-f-]{36})\}\\') {
        $fragment = "Volume{$($Matches[1])}\"
        $matches = @($partitions | Where-Object {
            $part = $_
            @($part.AccessPaths | Where-Object {
                $_ -and $_.IndexOf($fragment, [StringComparison]::OrdinalIgnoreCase) -ge 0
            }).Count -gt 0
        })
    } elseif ($path -match '(?i)([A-Z]):\\') {
        $letter = $Matches[1]
        $matches = @($partitions | Where-Object { Same ([string]$_.DriveLetter) $letter })
    } else {
        throw "Cannot locate page-file path '$path' on a disk."
    }
    if ($matches.Count -ne 1) { throw "Cannot uniquely resolve page-file path '$path'." }
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
        if ($number -eq 1 -or $number -eq 2) { throw "Page file '$path' is on target Disk $number." }
    }
}

function Assert-Disk($spec) {
    if ($spec.Number -notin @(1,2)) { throw 'Only the two recorded VHDX disks are allowed.' }
    $disk = Get-Disk -Number $spec.Number -ErrorAction Stop
    if (-not (Same ([string]$disk.UniqueId) $spec.UniqueId) -or
        -not (Same ([string]$disk.Guid) $spec.DiskGuid) -or
        -not (Same ([string]$disk.Path) $spec.DevicePath) -or
        [uint64]$disk.Size -ne $spec.DiskBytes -or
        [string]$disk.FriendlyName -ne 'Msft Virtual Disk' -or
        [string]$disk.PartitionStyle -ne 'GPT' -or
        $disk.IsBoot -or $disk.IsSystem -or $disk.IsOffline -or $disk.IsReadOnly) {
        throw "Disk $($spec.Number) identity or safety state changed."
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
        throw "Disk $diskNumber partition $($expected.Number) changed."
    }
}

function Assert-RawLayout($spec, $newPart = $null) {
    $null = Assert-Disk $spec
    $parts = @(Get-Partition -DiskNumber $spec.Number -ErrorAction Stop)
    if ($parts.Count -ne (1 + $spec.Raw.Count + [int][bool]$newPart)) {
        throw "Disk $($spec.Number) partition count changed."
    }
    $msr = @($parts | Where-Object { $_.PartitionNumber -eq 1 })
    if ($msr.Count -ne 1) { throw "Disk $($spec.Number) MSR is missing." }
    Assert-Partition $msr[0] $spec.Msr $msrType $spec.Number
    if (@($msr[0].AccessPaths | Where-Object { $_ }).Count -ne 0) { throw 'MSR has an access path.' }
    foreach ($expected in $spec.Raw) {
        $found = @($parts | Where-Object { $_.PartitionNumber -eq $expected.Number })
        if ($found.Count -ne 1) { throw "Disk $($spec.Number) raw PoolFS partition is missing." }
        Assert-Partition $found[0] $expected $basicData $spec.Number
        $paths = @($found[0].AccessPaths | Where-Object { $_ })
        if ($paths.Count -ne 1 -or $paths[0] -notmatch '^\\\\\?\\Volume\{[0-9a-f-]{36}\}\\$') {
            throw "Disk $($spec.Number) raw PoolFS partition access paths changed."
        }
    }
    if ($newPart) {
        $added = @($parts | Where-Object { $_.Guid -eq $newPart.Guid })
        if ($added.Count -ne 1 -or [uint64]$added[0].Offset -ne $spec.New.Offset -or
            [uint64]$added[0].Size -ne $spec.New.Size -or
            -not (Same ([string]$added[0].DriveLetter) $spec.New.Letter)) {
            throw "Disk $($spec.Number) new host partition changed."
        }
    }
}

function Assert-NewGeometry($spec) {
    $lastRawEnd = ($spec.Raw | Measure-Object -Property Offset -Maximum).Maximum
    $lastRaw = @($spec.Raw | Where-Object { $_.Offset -eq $lastRawEnd })[0]
    if ($spec.New.Offset -ne [uint64]($lastRaw.Offset + $lastRaw.Size) -or
        $spec.New.Size -ne 1GB -or $spec.New.Offset % 4096 -ne 0 -or
        $spec.New.Offset + $spec.New.Size -gt $spec.DiskBytes - 1MB) {
        throw "Disk $($spec.Number) planned NTFS partition is outside the verified free tail."
    }
}

function New-NtfsHost($spec) {
    Assert-PUnmounted
    Assert-NoTargetPageFile
    Assert-LetterFree $spec.New.Letter
    Assert-RawLayout $spec
    Assert-NewGeometry $spec
    $created = New-Partition -DiskNumber $spec.Number -Offset $spec.New.Offset -Size $spec.New.Size -DriveLetter ([char]$spec.New.Letter) -GptType $basicData -ErrorAction Stop
    $part = $null
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        $part = Get-Partition -DiskNumber $spec.Number -PartitionNumber $created.PartitionNumber -ErrorAction Stop
        if (-not [string]::IsNullOrWhiteSpace([string]$part.Guid) -and
            (Same ([string]$part.DriveLetter) $spec.New.Letter)) { break }
        Start-Sleep -Milliseconds 200
    }
    if ($part.GptType -ne $basicData -or [uint64]$part.Offset -ne $spec.New.Offset -or
        [uint64]$part.Size -ne $spec.New.Size -or
        -not (Same ([string]$part.DriveLetter) $spec.New.Letter) -or
        [string]::IsNullOrWhiteSpace([string]$part.Guid)) {
        throw "Disk $($spec.Number) new partition failed immediate verification."
    }
    Assert-RawLayout $spec $part
    $null = Format-Volume -Partition $part -FileSystem NTFS -NewFileSystemLabel $spec.New.Label -AllocationUnitSize 4096 -Force -Confirm:$false -ErrorAction Stop
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        $part = Get-Partition -DiskNumber $spec.Number -PartitionNumber $created.PartitionNumber -ErrorAction Stop
        try { $vol = Get-Volume -Partition $part -ErrorAction Stop }
        catch { $vol = $null }
        $paths = @($part.AccessPaths | Where-Object { $_ -match '^\\\\\?\\Volume\{[0-9a-f-]{36}\}\\$' })
        if ($vol -and $vol.FileSystem -eq 'NTFS' -and $paths.Count -eq 1) { break }
        Start-Sleep -Milliseconds 200
    }
    Assert-RawLayout $spec $part
    if ($vol.FileSystem -ne 'NTFS' -or $vol.FileSystemLabel -ne $spec.New.Label -or
        -not (Same ([string]$vol.DriveLetter) $spec.New.Letter) -or $paths.Count -ne 1) {
        throw "Disk $($spec.Number) NTFS format or volume path verification failed."
    }
    return [pscustomobject]@{
        DiskNumber = [uint32]$spec.Number
        DiskUniqueId = $spec.UniqueId
        DiskGuid = $spec.DiskGuid
        PartitionNumber = [uint32]$part.PartitionNumber
        PartitionId = [string]$part.Guid
        OffsetBytes = [uint64]$part.Offset
        PartitionBytes = [uint64]$part.Size
        DriveLetter = [string]$vol.DriveLetter
        FileSystem = [string]$vol.FileSystem
        FileSystemLabel = [string]$vol.FileSystemLabel
        VolumeGuidPath = [string]$paths[0]
        RootPath = "$($spec.New.Letter):\"
    }
}

foreach ($spec in $targets) {
    Assert-RawLayout $spec
    Assert-NewGeometry $spec
    Assert-LetterFree $spec.New.Letter
}
Assert-NoTargetPageFile

if (-not $Apply) {
    $mounted = [bool]((Get-PSDrive -Name P -ErrorAction SilentlyContinue) -or
        @(Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='P:'" -ErrorAction Stop).Count -ne 0)
    $plan = @($targets | ForEach-Object {
        $spec = $_
        [pscustomobject]@{
            DiskNumber = $spec.Number
            DiskUniqueId = $spec.UniqueId
            DiskGuid = $spec.DiskGuid
            PreserveRawPartitionIds = @($spec.Raw | ForEach-Object { $_.Guid })
            PreserveMsrPartitionId = $spec.Msr.Guid
            NewOffsetBytes = $spec.New.Offset
            NewPartitionBytes = $spec.New.Size
            NewDriveLetter = $spec.New.Letter
            NewFileSystem = 'NTFS'
            NewLabel = $spec.New.Label
        }
    })
    [pscustomobject]@{ Status='DryRun'; PIsMounted=$mounted; ReadyToApply=(-not $mounted); Plan=$plan } | ConvertTo-Json -Depth 7
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '-Apply requires elevated PowerShell.'
}
Assert-PUnmounted

# Any failure leaves a partial layout for manual inspection; this one-time
# helper deliberately refuses a second run once a host partition exists.
try {
    $created = @()
    foreach ($spec in $targets) { $created += New-NtfsHost $spec }
    [pscustomobject]@{ Status='Applied'; HostVolumes=$created } | ConvertTo-Json -Depth 7
} catch {
    throw "NTFS host setup stopped. Inspect Disk 1 and Disk 2 before any retry: $($_.Exception.Message)"
}
