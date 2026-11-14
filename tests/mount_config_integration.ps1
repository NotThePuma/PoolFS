param(
    [string]$BinaryDirectory = 'build'
)

# Uses only fresh member images under the repository's ignored work folder.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$binaryDir = if ([IO.Path]::IsPathRooted($BinaryDirectory)) {
    [IO.Path]::GetFullPath($BinaryDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $BinaryDirectory))
}
$formatter = Join-Path $binaryDir 'poolfs-format.exe'
$controller = Join-Path $binaryDir 'poolfsctl.exe'
$mounter = Join-Path $binaryDir 'poolfs-mount.exe'
foreach ($binary in @($formatter, $controller, $mounter)) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Missing test binary: $binary"
    }
}

$usedLetters = @([IO.DriveInfo]::GetDrives() |
    ForEach-Object { $_.Name.Substring(0, 1).ToUpperInvariant() })
$drive = $null
foreach ($candidate in [char[]]'ZYXWVUTSRQPONMLKJIHGFED') {
    $letter = [string]$candidate
    $root = "${letter}:\"
    if ($usedLetters -contains $letter -or
        (Get-PSDrive -Name $letter -ErrorAction SilentlyContinue) -or
        (Test-Path -LiteralPath $root)) {
        continue
    }
    $null = @(& $controller status "${letter}:" 2>&1)
    if ($LASTEXITCODE -ne 1) { continue }
    $drive = "${letter}:"
    break
}
if (-not $drive) { throw 'No unused drive letter is available for this test.' }
$driveRoot = $drive + '\'

function Invoke-Success([string]$Label, [string]$Program, [string[]]$Arguments) {
    $output = @(& $Program @Arguments 2>&1) -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0) {
        throw "$Label exited $LASTEXITCODE.`n$output"
    }
    return $output
}

function Invoke-Failure([string]$Label, [string]$Program, [string[]]$Arguments) {
    $output = @(& $Program @Arguments 2>&1) -join [Environment]::NewLine
    if ($LASTEXITCODE -eq 0) {
        throw "$Label unexpectedly succeeded.`n$output"
    }
    return $output
}

function Write-Config([string]$Path, [string[]]$Members,
                      [bool]$RequireEncrypted = $false) {
    $document = [ordered]@{
        schema_version = 1
        drive = $drive
        require_encrypted = $RequireEncrypted
        members = $Members
    } | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText($Path, $document,
        [Text.UTF8Encoding]::new($false))
}

function Assert-Unmounted {
    $null = Invoke-Failure 'unmounted status' $controller @('status', $drive)
    if (Test-Path -LiteralPath $driveRoot) {
        throw "$driveRoot is still mounted"
    }
}

$winfspBin = 'C:\Program Files (x86)\WinFsp\bin'
if (Test-Path -LiteralPath $winfspBin -PathType Container) {
    $env:PATH = $winfspBin + ';' + $env:PATH
}

$id = [Guid]::NewGuid().ToString('N')
$workRoot = [IO.Path]::GetFullPath((Join-Path $repo 'work'))
$testName = "mount-config-integration-$id"
$testRoot = [IO.Path]::GetFullPath((Join-Path $workRoot $testName))
$workPrefix = $workRoot.TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($workPrefix,
        [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($testRoot) -ne $testName -or
    (Test-Path -LiteralPath $testRoot)) {
    throw 'Refusing to create the test fixture outside its unique work folder.'
}

$single = Join-Path $testRoot 'single\member.img'
$singleConfig = Join-Path $testRoot 'single\config\mount.json'
$encryptedRequiredConfig = Join-Path $testRoot 'single\config\encrypted-required.json'
$first = Join-Path $testRoot 'two\members\member-a.img'
$second = Join-Path $testRoot 'two\members\member-b.img'
$twoConfig = Join-Path $testRoot 'two\config\mount.json'
$missingConfig = Join-Path $testRoot 'two\config\missing.json'
$mountedByTest = $false
$passed = $false

try {
    New-Item -ItemType Directory -Path (Split-Path $singleConfig -Parent),
        (Split-Path $twoConfig -Parent), (Split-Path $first -Parent) -Force |
        Out-Null

    $null = Invoke-Success 'format one-member v6 pool' $formatter @('384', $single)
    Write-Config $singleConfig @('..\member.img')
    Write-Config $encryptedRequiredConfig @('..\member.img') $true
    $null = Invoke-Failure 'plaintext rejected by encrypted policy' $controller @(
        'discover-config', $encryptedRequiredConfig)
    $null = Invoke-Failure 'plaintext mount rejected by encrypted policy' $controller @(
        'mount-config', $encryptedRequiredConfig)
    Assert-Unmounted
    $one = Invoke-Success 'discover one-member config' $controller @(
        'discover-config', $singleConfig)
    if ($one -notmatch 'member\[0\]:' -or
        $one -notmatch [Regex]::Escape($single)) {
        throw "One-member discovery did not report its image.`n$one"
    }
    $oneMount = Invoke-Success 'mount one-member config' $controller @(
        'mount-config', $singleConfig)
    $mountedByTest = $true
    if ($oneMount -notmatch 'mounted at' -or
        -not (Test-Path -LiteralPath $driveRoot)) {
        throw "One-member config did not mount on $drive.`n$oneMount"
    }
    [IO.File]::WriteAllText((Join-Path $driveRoot 'single.txt'), 'single')
    $null = Invoke-Success 'unmount one-member config' $controller @(
        'unmount', $drive)
    $mountedByTest = $false
    Assert-Unmounted

    $null = Invoke-Success 'format two-member v6 pool' $formatter @(
        '--sized', '384', $first, '384', $second)
    Write-Config $twoConfig @('..\members\member-a.img',
        '..\members\member-b.img')
    Write-Config $missingConfig @('..\members\member-a.img',
        '..\missing\member-b.img')

    # The ordinary seed-folder search can find member-b. The config must not
    # use that result when its explicit member-b path is absent.
    $fallback = Invoke-Success 'baseline seed discovery' $controller @(
        'discover', $first)
    if ($fallback -notmatch [Regex]::Escape($second)) {
        throw "Valid second member was not discoverable.`n$fallback"
    }
    $null = Invoke-Failure 'missing-path config discovery' $controller @(
        'discover-config', $missingConfig)
    $null = Invoke-Failure 'missing-path config mount' $controller @(
        'mount-config', $missingConfig)
    Assert-Unmounted

    $two = Invoke-Success 'discover two-member config' $controller @(
        'discover-config', $twoConfig)
    if ($two -notmatch [Regex]::Escape($first) -or
        $two -notmatch [Regex]::Escape($second)) {
        throw "Two-member discovery did not report both images.`n$two"
    }
    $twoMount = Invoke-Success 'mount two-member config' $controller @(
        'mount-config', $twoConfig)
    $mountedByTest = $true
    if ($twoMount -notmatch 'mounted at' -or
        -not (Test-Path -LiteralPath $driveRoot)) {
        throw "Two-member config did not mount on $drive.`n$twoMount"
    }
    [IO.File]::WriteAllText((Join-Path $driveRoot 'two.txt'), 'two')
    if ([IO.File]::ReadAllText((Join-Path $driveRoot 'two.txt')) -ne 'two') {
        throw 'Two-member mounted readback failed.'
    }
    $null = Invoke-Success 'unmount two-member config' $controller @(
        'unmount', $drive)
    $mountedByTest = $false
    Assert-Unmounted
    $passed = $true
    Write-Host "Mount-config integration passed on $drive (one and two members, encryption policy, and strict missing-path refusal)."
} finally {
    $null = @(& $controller status $drive 2>&1)
    if ($mountedByTest -or $LASTEXITCODE -eq 0) {
        try { $null = Invoke-Success 'cleanup unmount' $controller @('unmount', $drive) }
        catch { Write-Warning "Could not unmount the test pool on $drive`: $_" }
    }
    if ($passed -and -not (Test-Path -LiteralPath $driveRoot) -and
        (Test-Path -LiteralPath $testRoot -PathType Container)) {
        $resolved = [IO.Path]::GetFullPath($testRoot)
        $attributes = (Get-Item -LiteralPath $resolved).Attributes
        if ($resolved.StartsWith($workPrefix,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolved) -eq $testName -and
            -not ($attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        } else {
            Write-Warning "Leaving test fixture at $testRoot because its cleanup path changed."
        }
    }
}
