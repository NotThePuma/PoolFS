param(
    [Parameter(Mandatory)]
    [ValidateSet('Initialize', 'WriteLoop', 'Verify')]
    [string]$Phase,
    [Parameter(Mandatory)]
    [string]$PoolFile,
    [int]$StartGeneration = 1,
    [int]$Count = 100000,
    [int]$AcknowledgedGeneration = -1,
    [int]$PauseMilliseconds = 200
)

# Run inside a disposable VM. A controller outside the VM records ACK lines
# and can turn the VM off without a guest shutdown. Verification accepts the
# last acknowledged write or the one write that could have committed before
# its ACK was observed. Scrub and poolfs-check must also pass after unmount.
$ErrorActionPreference = 'Stop'
if ($StartGeneration -lt 1 -or $Count -lt 1 -or $PauseMilliseconds -lt 0 -or
    $AcknowledgedGeneration -lt -1) {
    throw 'Invalid generation, count, or pause.'
}
$PoolFile = [IO.Path]::GetFullPath($PoolFile)
if (-not (Test-Path -LiteralPath ([IO.Path]::GetPathRoot($PoolFile)))) {
    throw 'The PoolFS drive is not mounted.'
}

function Get-Payload([int]$Generation) {
    $seed = [Text.Encoding]::UTF8.GetBytes("PoolFS power-cut generation $Generation")
    $digest = [Security.Cryptography.SHA256]::HashData($seed)
    $payload = New-Object byte[] 4096
    for ($offset = 0; $offset -lt $payload.Length; $offset += $digest.Length) {
        [Array]::Copy($digest, 0, $payload, $offset, $digest.Length)
    }
    return ,$payload
}

function Get-Hash([byte[]]$Payload) {
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Payload))
}

switch ($Phase) {
    'Initialize' {
        if (Test-Path -LiteralPath $PoolFile) {
            throw 'Initialize refuses to overwrite an existing probe file.'
        }
        $payload = Get-Payload 0
        $stream = [IO.FileStream]::new($PoolFile, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $stream.Write($payload, 0, $payload.Length)
            $stream.Flush($true)
        } finally { $stream.Dispose() }
        Write-Output "INIT generation=0 sha256=$(Get-Hash $payload)"
    }
    'WriteLoop' {
        if (-not (Test-Path -LiteralPath $PoolFile -PathType Leaf) -or
            (Get-Item -LiteralPath $PoolFile).Length -ne 4096) {
            throw 'WriteLoop requires a 4096-byte file created by Initialize.'
        }
        for ($generation = $StartGeneration;
             $generation -lt $StartGeneration + $Count; $generation++) {
            $payload = Get-Payload $generation
            $stream = [IO.FileStream]::new($PoolFile, [IO.FileMode]::Open,
                [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $stream.Position = 0
                $stream.Write($payload, 0, $payload.Length)
                $stream.Flush($true)
            } finally { $stream.Dispose() }
            Write-Output "ACK generation=$generation sha256=$(Get-Hash $payload)"
            [Console]::Out.Flush()
            if ($PauseMilliseconds) { Start-Sleep -Milliseconds $PauseMilliseconds }
        }
    }
    'Verify' {
        if ($AcknowledgedGeneration -lt 0) {
            throw 'Verify requires -AcknowledgedGeneration from the external ACK log.'
        }
        $actual = [IO.File]::ReadAllBytes($PoolFile)
        if ($actual.Length -ne 4096) { throw "Recovered probe length is $($actual.Length), expected 4096." }
        $hash = Get-Hash $actual
        $ackHash = Get-Hash (Get-Payload $AcknowledgedGeneration)
        $nextHash = Get-Hash (Get-Payload ($AcknowledgedGeneration + 1))
        if ($hash -ne $ackHash -and $hash -ne $nextHash) {
            throw "Recovered probe is neither generation $AcknowledgedGeneration nor the next complete write: $hash"
        }
        $found = if ($hash -eq $ackHash) { $AcknowledgedGeneration } else { $AcknowledgedGeneration + 1 }
        Write-Output "RECOVERED generation=$found sha256=$hash"
    }
}
