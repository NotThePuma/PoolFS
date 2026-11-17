param(
    [string]$BinaryDirectory = 'build',
    [ValidatePattern('^[A-Za-z]:$')]
    [string]$Drive = 'V:'
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path $PSScriptRoot -Parent
$binaryDir = if ([IO.Path]::IsPathRooted($BinaryDirectory)) {
    [IO.Path]::GetFullPath($BinaryDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $BinaryDirectory))
}
$formatter = Join-Path $binaryDir 'poolfs-format.exe'
$controller = Join-Path $binaryDir 'poolfsctl.exe'
$scrubber = Join-Path $binaryDir 'poolfs-scrub.exe'
$checker = Join-Path $binaryDir 'poolfs-check.exe'
foreach ($binary in @($formatter, $controller, $scrubber, $checker)) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Missing test binary: $binary"
    }
}
$driveRoot = $Drive.ToUpperInvariant() + '\'
if (Test-Path -LiteralPath $driveRoot) {
    throw "Test drive is occupied: $Drive"
}
$winfspBin = 'C:\Program Files (x86)\WinFsp\bin'
if (Test-Path -LiteralPath $winfspBin -PathType Container) {
    $env:PATH = $winfspBin + ';' + $env:PATH
}

function Invoke-Expected([string]$Label, [string]$Program,
                         [string[]]$Arguments, [int]$ExpectedCode) {
    $output = @(& $Program @Arguments 2>&1) -join [Environment]::NewLine
    if ($LASTEXITCODE -ne $ExpectedCode) {
        throw "$Label exited $LASTEXITCODE instead of $ExpectedCode.`n$output"
    }
    return $output
}

$tempName = 'poolfsctl-' + [Guid]::NewGuid().ToString('N')
$tempRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) $tempName))
$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $tempRoot.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to create a fixture outside the temp directory.'
}
$firstDir = Join-Path $tempRoot 'first'
$secondDir = Join-Path $tempRoot 'second'
$first = Join-Path $firstDir 'member.img'
$second = Join-Path $secondDir 'member.img'
$passed = $false

try {
    New-Item -ItemType Directory -Path $firstDir, $secondDir -Force | Out-Null
    $null = Invoke-Expected 'format' $formatter @('--sized', '384', $first,
        '512', $second) 0
    $healthConfig = Join-Path $tempRoot 'health.json'
    @{schema_version=1; name='Health test'; drive=$Drive;
      require_encrypted=$false; members=@($first, $second)} |
        ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $healthConfig -Encoding utf8
    $health = Invoke-Expected 'complete pool health' $controller @(
        'health-config', $healthConfig, '--json') 0 | ConvertFrom-Json
    if (-not $health.mountable -or $health.members.Count -ne 2 -or
        @($health.members | Where-Object status -ne 'ready').Count -ne 0) {
        throw 'Health report did not recognize the complete pool.'
    }
    $reorderedConfig = Join-Path $tempRoot 'health-reordered.json'
    @{schema_version=1; name='Health test'; drive=$Drive;
      require_encrypted=$false; members=@($second, $first)} |
        ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $reorderedConfig -Encoding utf8
    $reordered = Invoke-Expected 'reordered pool health' $controller @(
        'health-config', $reorderedConfig, '--json') 0 | ConvertFrom-Json
    if (-not $reordered.mountable) {
        throw 'Health report rejected a complete pool in a different config order.'
    }
    $missingConfig = Join-Path $tempRoot 'health-missing.json'
    @{schema_version=1; name='Health test'; drive=$Drive;
      require_encrypted=$false; members=@($first, (Join-Path $secondDir 'missing.img'))} |
        ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $missingConfig -Encoding utf8
    $unhealthy = Invoke-Expected 'missing pool health' $controller @(
        'health-config', $missingConfig, '--json') 2 | ConvertFrom-Json
    if ($unhealthy.mountable -or $unhealthy.members[1].status -ne 'missing') {
        throw 'Health report did not identify the missing member.'
    }
    $missing = Invoke-Expected 'missing member discovery' $controller @('discover', $first) 2
    if ($missing -notmatch 'member\[1\]: MISSING') {
        throw "Discovery did not report the absent member.`n$missing"
    }
    $complete = Invoke-Expected 'complete discovery' $controller @('discover', $first, $secondDir) 0
    if ($complete -notmatch [Regex]::Escape($first) -or
        $complete -notmatch [Regex]::Escape($second)) {
        throw "Discovery did not return both members.`n$complete"
    }
    $mounted = Invoke-Expected 'managed mount' $controller @('mount', $Drive, $first, $secondDir) 0
    if ($mounted -notmatch 'mounted at') { throw "Mount was not confirmed.`n$mounted" }
    $null = Invoke-Expected 'mounted status' $controller @('status', $Drive) 0
    if (-not (Test-Path -LiteralPath $driveRoot)) {
        throw "Managed mount is not visible at $Drive"
    }
    $live = Invoke-Expected 'mounted pool health' $controller @(
        'health-config', $reorderedConfig, '--json') 0 | ConvertFrom-Json
    if (-not $live.mounted -or $live.mountable -or
        @($live.members | Where-Object status -ne 'mounted').Count -ne 0) {
        throw 'Health did not identify the active pool and both of its members.'
    }
    $liveMissing = Invoke-Expected 'different config on mounted drive' $controller @(
        'health-config', $missingConfig, '--json') 2 | ConvertFrom-Json
    if ($liveMissing.mounted -or $liveMissing.members[0].status -ne 'mounted' -or
        $liveMissing.members[1].status -ne 'missing') {
        throw 'Health treated an incomplete config as the mounted pool.'
    }
    Set-Content -LiteralPath (Join-Path $driveRoot 'control.txt') -Value 'poolfsctl'
    $null = Invoke-Expected 'managed unmount' $controller @('unmount', $Drive) 0
    if (Test-Path -LiteralPath $driveRoot) { throw "$Drive stayed mounted" }
    $null = Invoke-Expected 'unmounted status' $controller @('status', $Drive) 1
    $offline = Invoke-Expected 'unmounted pool health' $controller @(
        'health-config', $healthConfig, '--json') 0 | ConvertFrom-Json
    if ($offline.mounted -or -not $offline.mountable) {
        throw 'Health did not clear mounted status after unmount.'
    }
    $null = Invoke-Expected 'remount' $controller @('mount', $Drive, $first, $secondDir) 0
    $content = Get-Content -LiteralPath (Join-Path $driveRoot 'control.txt') -Raw
    if ($content.Trim() -ne 'poolfsctl') { throw 'File changed after managed remount' }
    $null = Invoke-Expected 'final unmount' $controller @('unmount', $Drive) 0
    $scrub = Invoke-Expected 'offline scrub' $scrubber @($first, $second) 0
    $check = Invoke-Expected 'offline check' $checker @($first, $second) 0
    if ($scrub -notmatch 'status: CLEAN' -or $check -notmatch 'status: CLEAN') {
        throw 'Managed pool did not pass offline checks'
    }
    $passed = $true
    Write-Host 'poolfsctl discovery, mount, status, unmount and remount test passed.'
} finally {
    $status = @(& $controller status $Drive 2>&1)
    if ($LASTEXITCODE -eq 0) {
        $null = @(& $controller unmount $Drive 2>&1)
    }
    if ($passed -and -not (Test-Path -LiteralPath $driveRoot) -and
        (Test-Path -LiteralPath $tempRoot -PathType Container)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
