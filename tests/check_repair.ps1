param(
    [string]$BinaryDirectory = 'build',
    [switch]$MixedSize
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
$checker = Join-Path $binaryDir 'poolfs-check.exe'
$crashTests = Join-Path $binaryDir 'poolfs-v6-crash-tests.exe'
$scrubber = Join-Path $binaryDir 'poolfs-scrub.exe'
$required = @($formatter, $checker, $crashTests, $scrubber)
foreach ($binary in $required) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Required test binary is missing: $binary"
    }
}
if (Test-Path Env:POOLFS_TEST_V4_CRASH_AT) {
    throw 'Unset POOLFS_TEST_V4_CRASH_AT before running this test.'
}
if (Test-Path Env:POOLFS_CHECK_TEST_EXIT_AFTER_FIRST_ZERO) {
    throw 'Unset POOLFS_CHECK_TEST_EXIT_AFTER_FIRST_ZERO before running this test.'
}

function Invoke-Expected([string]$Label, [string]$Binary, [string[]]$Arguments,
                         [int]$ExpectedExit) {
    $lines = @(& $Binary @Arguments 2>&1)
    $code = $LASTEXITCODE
    if ($code -ne $ExpectedExit) {
        throw "$Label exited $code instead of $ExpectedExit.`n$($lines -join [Environment]::NewLine)"
    }
    return ($lines -join [Environment]::NewLine)
}

function Flip-Byte([string]$Path, [long]$Offset) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $stream.Position = $Offset
        $value = $stream.ReadByte()
        if ($value -lt 0) { throw "Cannot read journal byte at $Offset in $Path" }
        $stream.Position = $Offset
        $stream.WriteByte([byte]($value -bxor 1))
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Copy-Block([string]$Source, [string]$Target, [long]$Offset) {
    $sourceStream = [IO.FileStream]::new($Source, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $targetStream = [IO.FileStream]::new($Target, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $block = New-Object byte[] 4096
        $sourceStream.Position = $Offset
        $read = 0
        while ($read -lt $block.Length) {
            $count = $sourceStream.Read($block, $read, $block.Length - $read)
            if ($count -eq 0) { throw "Short journal block in $Source" }
            $read += $count
        }
        $targetStream.Position = $Offset
        $targetStream.Write($block, 0, $block.Length)
        $targetStream.Flush($true)
    } finally {
        $targetStream.Dispose()
        $sourceStream.Dispose()
    }
}

$testDir = Join-Path ([IO.Path]::GetTempPath()) ('poolfs-check-repair-' + [Guid]::NewGuid().ToString('N'))
$testDir = [IO.Path]::GetFullPath($testDir)
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $testDir.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to create fixture outside the temp directory.'
}
New-Item -ItemType Directory -Path $testDir -ErrorAction Stop | Out-Null
$members = @(
    (Join-Path $testDir 'a.img'),
    (Join-Path $testDir 'b.img'),
    (Join-Path $testDir 'c.img')
)
$sidecars = @($members | ForEach-Object { $_ + '.poolfs-check-undo' })
$staged = @($sidecars | ForEach-Object { $_ + '.tmp' })
$damaged = @('damaged-a.img', 'damaged-b.img', 'damaged-c.img') |
    ForEach-Object { Join-Path $testDir $_ }
$divergent = @('divergent-a.img', 'divergent-b.img', 'divergent-c.img') |
    ForEach-Object { Join-Path $testDir $_ }
$foreign = @('foreign-a.img', 'foreign-b.img', 'foreign-c.img') |
    ForEach-Object { Join-Path $testDir $_ }
$negativeSidecars = @($damaged + $divergent + $foreign |
    ForEach-Object { $_ + '.poolfs-check-undo' })
$negativeStaged = @($negativeSidecars | ForEach-Object { $_ + '.tmp' })

try {
    $formatArgs = if ($MixedSize) {
        @('--sized', '384', $members[0], '512', $members[1], '640', $members[2])
    } else {
        @('384') + $members
    }
    $null = Invoke-Expected 'format' $formatter $formatArgs 0
    if ($MixedSize) {
        $actual = @($members | ForEach-Object { (Get-Item -LiteralPath $_).Length })
        if ($actual[0] -ne 384MB -or $actual[1] -ne 512MB -or
            $actual[2] -ne 640MB) {
            throw "Mixed-size recovery fixture has unexpected lengths: $($actual -join ', ')"
        }
    }
    $env:POOLFS_TEST_V4_CRASH_AT = 'after:6'
    try {
        $null = Invoke-Expected 'three-member interrupted commit' $crashTests (@('--create-on-members') + $members) 86
    } finally {
        Remove-Item Env:POOLFS_TEST_V4_CRASH_AT -ErrorAction SilentlyContinue
    }

    $before = Invoke-Expected 'read-only interrupted-tail inspection' $checker $members 2
    if ($before -notmatch 'status: INTERRUPTED_FINAL_COMMIT' -or
        $before -notmatch 'journal\[0\]: records=1' -or
        $before -notmatch 'journal\[1\]: records=1' -or
        $before -notmatch 'journal\[2\]: records=0') {
        throw "Unexpected interrupted-tail report.`n$before"
    }

    $env:POOLFS_CHECK_TEST_EXIT_AFTER_FIRST_ZERO = '1'
    try {
        $null = Invoke-Expected 'interrupted checker repair' $checker (@('--repair-final-tail') + $members) 89
    } finally {
        Remove-Item Env:POOLFS_CHECK_TEST_EXIT_AFTER_FIRST_ZERO -ErrorAction SilentlyContinue
    }
    foreach ($sidecar in $sidecars[0..1]) {
        $file = Get-Item -LiteralPath $sidecar -ErrorAction Stop
        if ($file.PSIsContainer -or $file.Length -ne 8192) {
            throw "Expected durable 8192-byte undo sidecar: $sidecar"
        }
    }
    if (Test-Path -LiteralPath $sidecars[2]) {
        throw 'The member without an extra journal record has an undo sidecar.'
    }
    $pending = Invoke-Expected 'pending-repair inspection' $checker $members 3
    if ($pending -notmatch 'status: PENDING_REPAIR') {
        throw "Expected pending-repair report.`n$pending"
    }
    $guard = Invoke-Expected 'pending-repair mount guard' $crashTests (@('--create-on-members') + $members) 1
    if ($guard -notmatch 'repair is incomplete|pending repair sidecar') {
        throw "Mount did not reject pending repair sidecars.`n$guard"
    }
    $scrubGuard = Invoke-Expected 'pending-repair scrub guard' $scrubber $members 2
    if ($scrubGuard -notmatch 'status: CORRUPT' -or
        $scrubGuard -notmatch 'pending repair sidecar') {
        throw "Scrub did not reject pending repair sidecars.`n$scrubGuard"
    }

    $resumed = Invoke-Expected 'repair retry' $checker (@('--repair-final-tail') + $members) 0
    if ($resumed -notmatch 'recovery: restored 2 original journal record' -or
        $resumed -notmatch 'status: REPAIRED') {
        throw "Repair did not restore then complete.`n$resumed"
    }
    foreach ($path in $sidecars + $staged) {
        if (Test-Path -LiteralPath $path) {
            throw "Repair left a sidecar: $path"
        }
    }
    $clean = Invoke-Expected 'final read-only inspection' $checker $members 0
    if ($clean -notmatch 'status: CLEAN' -or
        $clean -notmatch 'journal_common_records: 0') {
        throw "Unexpected final pool state.`n$clean"
    }
    $null = Invoke-Expected 'writable reopen and data write' $crashTests (@('--write-after-repair') + $members) 0
    $scrub = Invoke-Expected 'scrub after repair' $scrubber $members 0
    if ($scrub -notmatch '1 file\(s\) scanned' -or
        $scrub -notmatch 'status: CLEAN') {
        throw "Repaired data did not pass scrub.`n$scrub"
    }

    # A damaged acknowledged journal record must never be treated as an
    # interrupted final commit. The first record starts at two 4 KiB headers.
    $null = Invoke-Expected 'damaged fixture format' $formatter (@('384') + $damaged) 0
    $null = Invoke-Expected 'damaged fixture commit' $crashTests (@('--create-on-members') + $damaged) 0
    Flip-Byte $damaged[0] (2 * 4096 + 40)
    $bad = Invoke-Expected 'damaged journal inspection' $checker $damaged 3
    if ($bad -notmatch 'status: CORRUPT') {
        throw "Damaged journal was not rejected.`n$bad"
    }
    $badRepair = Invoke-Expected 'damaged journal repair refusal' $checker (@('--repair-final-tail') + $damaged) 3
    if ($badRepair -notmatch 'status: CORRUPT') {
        throw "Damaged journal repair was not refused.`n$badRepair"
    }

    # Swap one complete, independently valid record from another test pool.
    # The member headers remain a matching set, but the journals diverge.
    $null = Invoke-Expected 'divergent fixture format' $formatter (@('384') + $divergent) 0
    $null = Invoke-Expected 'foreign fixture format' $formatter (@('384') + $foreign) 0
    $null = Invoke-Expected 'divergent fixture commit' $crashTests (@('--create-on-members') + $divergent) 0
    $null = Invoke-Expected 'foreign fixture commit' $crashTests (@('--create-different-on-members') + $foreign) 0
    Copy-Block $foreign[0] $divergent[0] (2 * 4096)
    $split = Invoke-Expected 'divergent journal inspection' $checker $divergent 3
    if ($split -notmatch 'status: CORRUPT') {
        throw "Divergent journal was not rejected.`n$split"
    }
    $splitRepair = Invoke-Expected 'divergent journal repair refusal' $checker (@('--repair-final-tail') + $divergent) 3
    if ($splitRepair -notmatch 'status: CORRUPT') {
        throw "Divergent journal repair was not refused.`n$splitRepair"
    }
    foreach ($path in $negativeSidecars + $negativeStaged) {
        if (Test-Path -LiteralPath $path) {
            throw "Refused repair created an undo sidecar: $path"
        }
    }
    $sizeLabel = if ($MixedSize) { 'mixed-size ' } else { '' }
    Write-Host "poolfs-check v6 ${sizeLabel}sidecar restart test passed (three members)."
} finally {
    Remove-Item Env:POOLFS_TEST_V4_CRASH_AT -ErrorAction SilentlyContinue
    Remove-Item Env:POOLFS_CHECK_TEST_EXIT_AFTER_FIRST_ZERO -ErrorAction SilentlyContinue
    foreach ($path in $members + $sidecars + $staged + $damaged + $divergent + $foreign +
            $negativeSidecars + $negativeStaged) {
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path
            if ($item.PSIsContainer -or
                [IO.Path]::GetFullPath($item.DirectoryName) -ne $testDir) {
                throw "Refusing to clean an unexpected fixture path: $path"
            }
            Remove-Item -LiteralPath $path
        }
    }
    if (Test-Path -LiteralPath $testDir) {
        Remove-Item -LiteralPath $testDir # Fails if an unexpected file remains.
    }
}
