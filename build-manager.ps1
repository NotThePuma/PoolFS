param(
    [string]$PythonPath,
    [string]$OutputDirectory = 'build'
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory
} else {
    Join-Path $project $OutputDirectory
}
$output = [IO.Path]::GetFullPath($output)
$entry = Join-Path $project 'manager\app.py'
if (-not (Test-Path -LiteralPath $entry -PathType Leaf)) {
    throw 'The Python/Qt manager entry point is missing.'
}

if (-not $PythonPath) {
    $venv = Join-Path $project '.venv\Scripts\python.exe'
    $local = Join-Path $project '.sdk\pyenv\Scripts\python.exe'
    if (Test-Path -LiteralPath $venv -PathType Leaf) {
        $PythonPath = $venv
    } elseif (Test-Path -LiteralPath $local -PathType Leaf) {
        $PythonPath = $local
    } else {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($python) { $PythonPath = $python.Source }
    }
}
if (-not $PythonPath -or -not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
    throw 'Python is required to build the Qt manager. Pass -PythonPath.'
}

& $PythonPath -c 'import PySide6, PyInstaller'
if ($LASTEXITCODE -ne 0) {
    throw 'Install requirements-manager.txt in the selected Python environment.'
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
$work = Join-Path $project 'work\pyinstaller'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$originalPath = $env:PATH
$windows = if ($env:SystemRoot) { $env:SystemRoot } else { 'C:\Windows' }
$env:PATH = @(
    (Join-Path $windows 'System32'), $windows,
    (Join-Path $windows 'System32\Wbem')
) -join ';'
try {
    # Keep unrelated applications' DLLs on PATH out of the frozen Qt bundle.
    & $PythonPath -m PyInstaller --noconfirm --clean --onefile --windowed `
        --name poolfs-manager --distpath $output --workpath $work `
        --specpath $work $entry
    $packageExit = $LASTEXITCODE
} finally {
    $env:PATH = $originalPath
}
if ($packageExit -ne 0) {
    throw "Qt manager packaging failed with exit code $packageExit."
}
Write-Host "Built Python/Qt PoolFS Manager in $output"
