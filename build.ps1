param(
    [string]$ZigPath,
    [string]$WinFspSdk,
    [string]$OutputDirectory = 'build',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory
} else {
    Join-Path $project $OutputDirectory
}
New-Item -ItemType Directory -Path $output -Force | Out-Null

if (-not $ZigPath) {
    $zig = Get-Command zig -ErrorAction SilentlyContinue
    if ($zig) { $ZigPath = $zig.Source }
}
if (-not $ZigPath) {
    $packages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    $candidate = Get-ChildItem -Path $packages -Filter 'zig.zig_*' -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem -Path $_.FullName -Filter 'zig.exe' -File -Recurse -ErrorAction SilentlyContinue } |
        Select-Object -First 1
    if ($candidate) { $ZigPath = $candidate.FullName }
}
if (-not $ZigPath -or -not (Test-Path -LiteralPath $ZigPath -PathType Leaf)) {
    throw 'Zig compiler not found. Pass -ZigPath with the path to zig.exe.'
}

if (-not $WinFspSdk) {
    $localSdk = Join-Path $project '.sdk\winfsp-2.1'
    if (Test-Path -LiteralPath (Join-Path $localSdk 'inc\winfsp\winfsp.h')) {
        $WinFspSdk = $localSdk
    }
}
if (-not $WinFspSdk) {
    $installed = 'C:\Program Files (x86)\WinFsp'
    if (Test-Path -LiteralPath (Join-Path $installed 'inc\winfsp\winfsp.h')) {
        $WinFspSdk = $installed
    }
}
if (-not $WinFspSdk -or
    -not (Test-Path -LiteralPath (Join-Path $WinFspSdk 'inc\winfsp\winfsp.h')) -or
    -not (Test-Path -LiteralPath (Join-Path $WinFspSdk 'lib\winfsp-x64.lib'))) {
    throw 'WinFsp developer SDK not found. Install the WinFsp Developer feature or pass -WinFspSdk to its inc/lib root.'
}

$optimization = if ($Configuration -eq 'Release') { '-O2' } else { '-O0' }
$partitionScript = Join-Path $project 'tools\gui-partition-ops.ps1'
$partitionHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $partitionScript).Hash.ToLowerInvariant()
$hashHeader = Join-Path $output 'gui_partition_script_hash.h'
[IO.File]::WriteAllText($hashHeader,
    "#pragma once`n#define POOLFS_PARTITION_SCRIPT_SHA256 `"$partitionHash`"`n",
    [Text.UTF8Encoding]::new($false))
$common = @('-std=c++20', '-target', 'x86_64-windows-gnu', '-fms-extensions',
            '-Wall', '-Wextra', '-Werror', $optimization,
            '-I', (Join-Path $project 'src'), '-I', $output)
$sdkInclude = Join-Path $WinFspSdk 'inc'
$sdkLibrary = Join-Path $WinFspSdk 'lib\winfsp-x64.lib'
$core = Join-Path $project 'src\core.cpp'
$crypto = Join-Path $project 'src\crypto.cpp'
$password = Join-Path $project 'src\password.cpp'
$mountConfig = Join-Path $project 'src\mount_config.cpp'
$mountStats = Join-Path $project 'src\mount_stats.cpp'
$guiInventory = Join-Path $project 'src\gui_inventory.cpp'
$guiBackend = Join-Path $project 'src\gui_backend.cpp'
$guiBridge = Join-Path $project 'src\gui_bridge.cpp'
$guiDialogs = Join-Path $project 'src\gui_dialogs.cpp'
$guiPartitions = Join-Path $project 'src\gui_partition_ops.cpp'
$guiMain = Join-Path $project 'src\gui_main.cpp'
$rawDevice = Join-Path $project 'src\raw_device.cpp'
$formatter = Join-Path $project 'src\format_main.cpp'
$mount = Join-Path $project 'src\winfsp_main.cpp'
$manager = Join-Path $project 'src\poolfsctl_main.cpp'
$v4CrashTests = Join-Path $project 'tests\v4_crash_tests.cpp'
$v6IntegrityTests = Join-Path $project 'tests\v6_integrity_tests.cpp'
$v6EncryptionTests = Join-Path $project 'tests\v6_encryption_tests.cpp'
$v6PortableMembersTests = Join-Path $project 'tests\v6_portable_members_tests.cpp'
$cryptoTests = Join-Path $project 'tests\crypto_tests.cpp'
$mountConfigTests = Join-Path $project 'tests\mount_config_tests.cpp'
$guiBackendTests = Join-Path $project 'tests\gui_backend_tests.cpp'
$guiPartitionTests = Join-Path $project 'tests\gui_partition_ops_tests.cpp'
$checker = Join-Path $project 'src\check_main.cpp'
$scrubberV6 = Join-Path $project 'src\scrub_v6_main.cpp'
$benchmark = Join-Path $project 'bench\bench_main.cpp'

function Invoke-Build([string[]]$Arguments) {
    & $ZigPath c++ @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Zig C++ build failed with exit code $LASTEXITCODE." }
}

Invoke-Build ($common + @($core, $crypto, $password, $rawDevice, $formatter, '-lbcrypt', '-ladvapi32',
    '-o', (Join-Path $output 'poolfs-format.exe')))
Invoke-Build ($common + @('-isystem', $sdkInclude, $core, $crypto, $password, $mountConfig, $mountStats, $rawDevice, $mount, $sdkLibrary,
    '-lbcrypt', '-ladvapi32', '-lshell32', '-o', (Join-Path $output 'poolfs-mount.exe')))
Invoke-Build ($common + @($manager, $password, $mountConfig, $mountStats, '-lshell32',
    '-o', (Join-Path $output 'poolfsctl.exe')))
Invoke-Build ($common + @($core, $crypto, $password, $mountConfig, $rawDevice,
    $guiInventory, $guiBackend, $guiDialogs, $guiPartitions, $guiMain,
    '-lbcrypt', '-ladvapi32',
    '-lcomctl32', '-lcomdlg32', '-lshell32', '-lole32', '-lgdi32',
    '-Wl,--subsystem,windows',
    '-o', (Join-Path $output 'poolfs-manager-classic.exe')))
Invoke-Build ($common + @($core, $crypto, $password, $mountConfig, $rawDevice,
    $guiInventory, $guiBackend, $guiPartitions, $guiBridge, $mountStats,
    '-shared', '-lbcrypt', '-ladvapi32', '-lshell32', '-lole32',
    '-o', (Join-Path $output 'poolfs-gui-native.dll')))
Invoke-Build ($common + @('-DPOOLFS_CRASH_V6', $core, $crypto, $rawDevice, $v4CrashTests,
    '-lbcrypt', '-ladvapi32', '-o', (Join-Path $output 'poolfs-v6-crash-tests.exe')))
Invoke-Build ($common + @('-DPOOLFS_CRASH_V6_ENCRYPTED', $core, $crypto, $rawDevice, $v4CrashTests,
    '-lbcrypt', '-ladvapi32', '-o', (Join-Path $output 'poolfs-v6-encrypted-crash-tests.exe')))
Invoke-Build ($common + @('-municode', $core, $crypto, $password, $rawDevice, $checker, '-lbcrypt', '-ladvapi32',
    '-o', (Join-Path $output 'poolfs-check.exe')))
Invoke-Build ($common + @($core, $crypto, $password, $rawDevice, $scrubberV6, '-lbcrypt', '-ladvapi32',
    '-o', (Join-Path $output 'poolfs-scrub.exe')))
Invoke-Build ($common + @($core, $crypto, $rawDevice, $v6IntegrityTests, '-lbcrypt', '-ladvapi32',
    '-o', (Join-Path $output 'poolfs-v6-integrity-tests.exe')))
Invoke-Build ($common + @($core, $crypto, $rawDevice, $v6EncryptionTests, '-lbcrypt', '-ladvapi32',
    '-o', (Join-Path $output 'poolfs-v6-encryption-tests.exe')))
Invoke-Build ($common + @($core, $crypto, $rawDevice, $v6PortableMembersTests, '-lbcrypt', '-ladvapi32',
    '-o', (Join-Path $output 'poolfs-v6-portable-members-tests.exe')))
Invoke-Build ($common + @($crypto, $cryptoTests, '-lbcrypt',
    '-o', (Join-Path $output 'poolfs-crypto-tests.exe')))
Invoke-Build ($common + @($mountConfig, $mountConfigTests,
    '-o', (Join-Path $output 'poolfs-mount-config-tests.exe')))
Invoke-Build ($common + @($core, $crypto, $password, $mountConfig, $rawDevice,
    $guiBackend, $guiBackendTests, '-lbcrypt', '-ladvapi32', '-lshell32',
    '-o', (Join-Path $output 'poolfs-gui-backend-tests.exe')))
Invoke-Build ($common + @($guiInventory, $guiPartitions, $guiPartitionTests, '-ladvapi32', '-lbcrypt',
    '-o', (Join-Path $output 'poolfs-gui-partition-tests.exe')))
Invoke-Build ($common + @($benchmark, '-lshell32', '-o',
    (Join-Path $output 'poolfs-bench.exe')))

Copy-Item -LiteralPath $partitionScript -Destination (Join-Path $output 'poolfs-partitions.ps1') -Force

Write-Host "Built PoolFS binaries in $output"
