param(
    [string]$BinaryDirectory = 'build',
    [string]$OutputDirectory = 'build',
    [string]$InnoSetupPath,
    [ValidatePattern('^\d{1,4}\.\d{1,4}\.\d{1,4}(\.\d{1,4})?$')]
    [string]$Version = '6.0.1'
)

$ErrorActionPreference = 'Stop'
$payload = [IO.Path]::GetFullPath($BinaryDirectory, $PSScriptRoot)
$output = [IO.Path]::GetFullPath($OutputDirectory, $PSScriptRoot)

if (-not $InnoSetupPath) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    $candidates = @(
        $command.Source
        (Join-Path $PSScriptRoot '.sdk\inno-setup\compiler\ISCC.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
        (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 7\ISCC.exe')
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe')
    )
    $InnoSetupPath = $candidates | Where-Object {
        $_ -and (Test-Path -LiteralPath $_ -PathType Leaf)
    } | Select-Object -First 1
}
if (-not $InnoSetupPath -or -not (Test-Path -LiteralPath $InnoSetupPath -PathType Leaf)) {
    throw 'Inno Setup compiler not found. Install Inno Setup 6.7 or newer or pass -InnoSetupPath to ISCC.exe.'
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
$script = Join-Path $PSScriptRoot 'installer\poolfs.iss'
& $InnoSetupPath '/Qp' "/DPayloadDir=$payload" "/DPoolFSVersion=$Version" "/O$output" $script
if ($LASTEXITCODE -ne 0) { throw "PoolFS installer compilation failed with exit code $LASTEXITCODE." }

$setup = Join-Path $output 'poolfs-setup-x64.exe'
if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) { throw "Missing installer: $setup" }
Write-Host "Built PoolFS $Version installer in $setup"
