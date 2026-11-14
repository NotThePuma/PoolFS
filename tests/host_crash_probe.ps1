param(
    [Parameter(Mandatory)]
    [ValidateSet('Prepare', 'WriteLoop', 'Verify')]
    [string]$Phase,
    [ValidatePattern('^[0-9a-fA-F]{32}$')]
    [string]$RunId,
    [string]$StateDirectory = 'work\host-crash',
    [string]$BinaryDirectory = 'site\dist\downloads',
    [ValidateRange(1, 10000000)]
    [int]$Count = 100000,
    [ValidateRange(0, 60000)]
    [int]$PauseMilliseconds = 200,
    [int]$ObservedAcknowledgedGeneration = -1
)

# This script never crashes, shuts down, or reboots Windows. A host-wide crash
# must be initiated separately by the user after Prepare and WriteLoop start.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path $PSScriptRoot -Parent
$oracle = Join-Path $PSScriptRoot 'power_cut_oracle.ps1'
$winfspBin = 'C:\Program Files (x86)\WinFsp\bin'
if (Test-Path -LiteralPath $winfspBin -PathType Container) {
    $env:PATH = $winfspBin + ';' + $env:PATH
}

function Get-AbsolutePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repo $Path))
}

function Get-ExpectedHash([int]$Generation) {
    $seed = [Text.Encoding]::UTF8.GetBytes("PoolFS power-cut generation $Generation")
    $digest = [Security.Cryptography.SHA256]::HashData($seed)
    $payload = New-Object byte[] 4096
    for ($offset = 0; $offset -lt $payload.Length; $offset += $digest.Length) {
        [Array]::Copy($digest, 0, $payload, $offset, $digest.Length)
    }
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload))
}

function Write-DurableText([string]$Path, [string]$Value, [bool]$CreateNew) {
    $mode = if ($CreateNew) { [IO.FileMode]::CreateNew } else { [IO.FileMode]::Append }
    $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
    $stream = [IO.FileStream]::new($Path, $mode, [IO.FileAccess]::Write,
        [IO.FileShare]::Read)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
}

function Invoke-Tool([string]$Label, [string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

function Get-Acknowledgements([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ACK log is missing: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $lastNewline = [Array]::LastIndexOf($bytes, [byte]10)
    if ($lastNewline -lt 0) { throw 'ACK log has no complete record.' }
    $complete = [Text.Encoding]::UTF8.GetString($bytes, 0, $lastNewline + 1)
    $lines = @($complete.Substring(0, $complete.Length - 1).Split([char]10))
    if ($lines.Count -eq 0) { throw 'ACK log has no complete record.' }
    for ($generation = 0; $generation -lt $lines.Count; $generation++) {
        $line = $lines[$generation]
        if ($line -cnotmatch '^ACK generation=([0-9]+) sha256=([0-9A-F]{64})$') {
            throw "Invalid complete ACK record $generation."
        }
        if ([int]$Matches[1] -ne $generation -or
            $Matches[2] -cne (Get-ExpectedHash $generation)) {
            throw "ACK record $generation does not match its expected generation and hash."
        }
    }
    return [pscustomobject]@{
        Generation = $lines.Count - 1
        IncompleteTail = $lastNewline -ne ($bytes.Length - 1)
    }
}

function Assert-Member([string]$Path, [long]$ExpectedBytes) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Member image is missing after reboot: $Path. Do not reformat it."
    }
    $actual = (Get-Item -LiteralPath $Path).Length
    if ($actual -ne $ExpectedBytes) {
        throw "Member image has changed length: $Path ($actual instead of $ExpectedBytes bytes)."
    }
}

$stateRoot = Get-AbsolutePath $StateDirectory
$stateVolume = [IO.Path]::GetPathRoot($stateRoot)
if (-not $stateVolume -or $stateVolume -in @('T:\', 'U:\', 'X:\')) {
    throw 'State and ACK files must be outside T:, U:, and the PoolFS mount X:.'
}
if ($ObservedAcknowledgedGeneration -lt -1) {
    throw 'ObservedAcknowledgedGeneration must be -1 or a nonnegative generation.'
}
if (-not $RunId) {
    if ($Phase -ne 'Prepare') { throw 'WriteLoop and Verify require the RunId printed by Prepare.' }
    $RunId = [Guid]::NewGuid().ToString('N')
}
$RunId = $RunId.ToLowerInvariant()
$first = "T:\PoolFS\host-crash-$RunId-a.img"
$second = "U:\PoolFS\host-crash-$RunId-b.img"
$poolFile = 'X:\host-crash-probe.bin'
$statePath = Join-Path $stateRoot "$RunId.json"
$ackPath = Join-Path $stateRoot "$RunId.acks"

if ($Phase -eq 'Prepare') {
    if (-not (Test-Path -LiteralPath 'T:\PoolFS' -PathType Container) -or
        -not (Test-Path -LiteralPath 'U:\PoolFS' -PathType Container)) {
        throw 'T:\PoolFS and U:\PoolFS must both be attached before preparation.'
    }
    if (Test-Path -LiteralPath 'X:\') { throw 'X: is already in use.' }
    foreach ($path in @($first, $second, $statePath, $ackPath)) {
        if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing path: $path" }
    }
    $binaryRoot = Get-AbsolutePath $BinaryDirectory
    foreach ($name in @('poolfs-format.exe', 'poolfsctl.exe', 'poolfs-check.exe',
                         'poolfs-scrub.exe', 'poolfs-mount.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot $name) -PathType Leaf)) {
            throw "Missing binary: $(Join-Path $binaryRoot $name)"
        }
    }
    $firstFree = (Get-PSDrive -Name T -PSProvider FileSystem).Free
    $secondFree = (Get-PSDrive -Name U -PSProvider FileSystem).Free
    if ($firstFree -lt (384MB + 64MB) -or $secondFree -lt (512MB + 64MB)) {
        throw 'T: or U: lacks room for a fresh member image and 64 MiB headroom.'
    }
    [void][IO.Directory]::CreateDirectory($stateRoot)
    $existingMounts = @()
    try {
        $existingMounts = @(Get-CimInstance Win32_Process -Filter "Name='poolfs-mount.exe'" |
            Select-Object ProcessId, ExecutablePath, CommandLine)
    } catch {
        Write-Warning 'Could not capture currently running PoolFS mount commands.'
    }
    $state = [ordered]@{
        Schema = 1
        RunId = $RunId
        CreatedUtc = [DateTime]::UtcNow.ToString('o')
        Drive = 'X:'
        FirstMember = $first
        SecondMember = $second
        PoolFile = $poolFile
        AckLog = $ackPath
        BinaryDirectory = $binaryRoot
        ExistingMounts = $existingMounts
    }
    Write-DurableText $statePath (($state | ConvertTo-Json -Depth 5) + "`n") $true
    $mounted = $false
    try {
        Invoke-Tool 'format' (Join-Path $binaryRoot 'poolfs-format.exe') @(
            '--sized', '384', $first, '512', $second)
        Invoke-Tool 'mount' (Join-Path $binaryRoot 'poolfsctl.exe') @(
            'mount', 'X:', $first, $second)
        $mounted = $true
        if (-not (Test-Path -LiteralPath 'X:\')) { throw 'X: did not become visible.' }
        $init = @(& $oracle -Phase Initialize -PoolFile $poolFile)
        if ($init.Count -ne 1 -or
            $init[0] -cnotmatch '^INIT generation=0 sha256=([0-9A-F]{64})$' -or
            $Matches[1] -cne (Get-ExpectedHash 0)) {
            throw 'Oracle initialization produced an unexpected generation or hash.'
        }
        Write-DurableText $ackPath "ACK generation=0 sha256=$(Get-ExpectedHash 0)`n" $true
        Write-Output "PREPARED run_id=$RunId"
        Write-Output "STATE $statePath"
        Write-Output "MEMBER_A $first"
        Write-Output "MEMBER_B $second"
        Write-Output "ACK_LOG $ackPath"
        Write-Output 'X: is mounted. Start WriteLoop in a separate PowerShell window.'
    } catch {
        if ($mounted) {
            try { Invoke-Tool 'unmount after setup failure' (Join-Path $binaryRoot 'poolfsctl.exe') @('unmount', 'X:') }
            catch { Write-Warning "Could not unmount X: after setup failure: $_" }
        }
        throw
    }
    return
}

if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
    throw "Run state is missing: $statePath"
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($state.Schema -ne 1 -or $state.RunId -cne $RunId -or $state.Drive -cne 'X:' -or
    $state.FirstMember -cne $first -or $state.SecondMember -cne $second -or
    $state.PoolFile -cne $poolFile -or $state.AckLog -cne $ackPath) {
    throw 'Run state does not match the exact T:/U: images and X: mount for this RunId.'
}
$binaryRoot = [string]$state.BinaryDirectory
foreach ($name in @('poolfsctl.exe', 'poolfs-check.exe', 'poolfs-scrub.exe', 'poolfs-mount.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot $name) -PathType Leaf)) {
        throw "Missing binary after reboot: $(Join-Path $binaryRoot $name)"
    }
}
$ack = $null
try {
    $ack = Get-Acknowledgements $ackPath
} catch {
    if ($Phase -ne 'Verify' -or $ObservedAcknowledgedGeneration -lt 0) { throw }
    Write-Warning "ACK log could not be validated; using externally observed ACK: $_"
}

if ($Phase -eq 'WriteLoop') {
    Assert-Member $first 402653184
    Assert-Member $second 536870912
    if (-not (Test-Path -LiteralPath 'X:\' -PathType Container) -or
        -not (Test-Path -LiteralPath $poolFile -PathType Leaf)) {
        throw 'X: and its probe file must still be mounted from Prepare.'
    }
    if ($ack.IncompleteTail) {
        throw 'ACK log has an incomplete tail. Verify this run before writing again.'
    }
    if ($ack.Generation -ge [int]::MaxValue - $Count) {
        throw 'Generation would exceed the supported range.'
    }
    $next = $ack.Generation + 1
    & $oracle -Phase WriteLoop -PoolFile $poolFile -StartGeneration $next `
        -Count $Count -PauseMilliseconds $PauseMilliseconds | ForEach-Object {
        $line = [string]$_
        if ($line -cnotmatch '^ACK generation=([0-9]+) sha256=([0-9A-F]{64})$') {
            throw "Unexpected oracle output: $line"
        }
        $generation = [int]$Matches[1]
        $hash = [string]$Matches[2]
        if ($generation -ne $next -or $hash -cne (Get-ExpectedHash $generation)) {
            throw "Oracle generation $generation did not match expected generation $next."
        }
        Write-DurableText $ackPath "$line`n" $false
        Write-Output $line
        $next++
    }
    return
}

# Verification runs after reboot, when the old WinFsp mount process is gone.
Assert-Member $first 402653184
Assert-Member $second 536870912
if (Test-Path -LiteralPath 'X:\') {
    throw 'X: is already mounted. Unmount the test pool before Verify; this phase remounts it itself.'
}
$generation = if ($ObservedAcknowledgedGeneration -ge 0) {
    $ObservedAcknowledgedGeneration
} else { $ack.Generation }
if ($ObservedAcknowledgedGeneration -ge 0) {
    Write-Output "Using externally observed ACK generation $generation."
} else {
    Write-Output "Using last complete C: ACK log generation $generation."
}
if ($ack -and $ack.IncompleteTail) {
    Write-Warning 'Ignoring an incomplete final ACK log record.'
}
$verificationError = $null
$mounted = $false
try {
    Invoke-Tool 'remount' (Join-Path $binaryRoot 'poolfsctl.exe') @(
        'mount', 'X:', $first, $second)
    $mounted = $true
    & $oracle -Phase Verify -PoolFile $poolFile -AcknowledgedGeneration $generation
} catch {
    $verificationError = $_
} finally {
    if ($mounted) {
        Invoke-Tool 'unmount' (Join-Path $binaryRoot 'poolfsctl.exe') @('unmount', 'X:')
    }
}
if (Test-Path -LiteralPath 'X:\') { throw 'X: is still mounted; refusing offline checks.' }
Invoke-Tool 'offline scrub' (Join-Path $binaryRoot 'poolfs-scrub.exe') @($first, $second)
Invoke-Tool 'offline check' (Join-Path $binaryRoot 'poolfs-check.exe') @($first, $second)
if ($verificationError) { throw $verificationError }
Write-Output "HOST_CRASH_PROBE_CLEAN run_id=$RunId"
Write-Output "Images and run state were retained: $first ; $second ; $statePath"
