param(
    [string]$BuilderScript
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuilderScript) {
    $BuilderScript = Join-Path $repoRoot 'build_bitstream.ps1'
}
$BuilderScript = (Resolve-Path -LiteralPath $BuilderScript).Path
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'zz9000-build-bitstream-test-' + [guid]::NewGuid().ToString('N')
)
$fakeRepo = Join-Path $tempRoot 'repo'
$stubLog = Join-Path $tempRoot 'vivado.log'
$stubProject = Join-Path $fakeRepo 'ZZ9000_proto'
$fakeVivado = Join-Path $tempRoot 'vivado.ps1'
$hadMarginalTiming = Test-Path Env:ZZ9000_ALLOW_NONRELEASE_TIMING
$previousMarginalTiming = $env:ZZ9000_ALLOW_NONRELEASE_TIMING

function Invoke-Build {
    param(
        [string]$ExpectedTiming,
        [switch]$AllowMarginalTiming,
        [string]$OutputBitstream
    )

    Remove-Item -LiteralPath $stubLog -Force -ErrorAction SilentlyContinue
    if ($AllowMarginalTiming) {
        & (Join-Path $fakeRepo 'build_bitstream.ps1') -VivadoBat $fakeVivado `
            -AllowMarginalTiming -OutputBitstream $OutputBitstream
    } else {
        & (Join-Path $fakeRepo 'build_bitstream.ps1') -VivadoBat $fakeVivado
    }
    $seen = @(Get-Content -LiteralPath $stubLog)
    if ($seen.Count -ne 2) {
        throw "Expected two Vivado calls, saw $($seen.Count)."
    }
    foreach ($value in $seen) {
        if ($value -ne $ExpectedTiming) {
            throw "Vivado saw '$value', expected '$ExpectedTiming'."
        }
    }
}

try {
    New-Item -ItemType Directory -Path $fakeRepo | Out-Null
    Copy-Item -LiteralPath $BuilderScript -Destination (Join-Path $fakeRepo 'build_bitstream.ps1')
    @'
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
)

if (Test-Path Env:ZZ9000_ALLOW_NONRELEASE_TIMING) {
    Add-Content -LiteralPath $env:ZZ9000_STUB_LOG -Value (
        'set=' + $env:ZZ9000_ALLOW_NONRELEASE_TIMING)
} else {
    Add-Content -LiteralPath $env:ZZ9000_STUB_LOG -Value 'unset'
}
if ($Arguments -contains 'build_run_synthesis.tcl') {
    $implementation = Join-Path $env:ZZ9000_STUB_PROJECT (
        'ZZ9000_proto.runs\impl_1')
    New-Item -ItemType Directory -Path $implementation -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $implementation 'zz9000_ps_wrapper.bit') `
        -Value 'fake'
}
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath $fakeVivado

    $env:ZZ9000_STUB_LOG = $stubLog
    $env:ZZ9000_STUB_PROJECT = $stubProject
    $env:ZZ9000_ALLOW_NONRELEASE_TIMING = 'inherited'
    Invoke-Build -ExpectedTiming 'unset'

    $outsideOutput = Join-Path $tempRoot 'marginal\zz9000_ps_wrapper.bit'
    Invoke-Build -ExpectedTiming 'set=1' -AllowMarginalTiming `
        -OutputBitstream $outsideOutput

    $refused = $false
    try {
        & (Join-Path $fakeRepo 'build_bitstream.ps1') -VivadoBat $fakeVivado `
            -AllowMarginalTiming 2>&1 | Out-Null
    } catch {
        if ($_.Exception.Message -match 'requires -OutputBitstream outside bootimage_work') {
            $refused = $true
        } else {
            throw
        }
    }
    if (-not $refused) {
        throw 'AllowMarginalTiming without an outside output path was accepted.'
    }

    Write-Host 'PASS build_bitstream inherited timing override regression'
} finally {
    if ($hadMarginalTiming) {
        $env:ZZ9000_ALLOW_NONRELEASE_TIMING = $previousMarginalTiming
    } else {
        Remove-Item Env:ZZ9000_ALLOW_NONRELEASE_TIMING -ErrorAction SilentlyContinue
    }
    Remove-Item Env:ZZ9000_STUB_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:ZZ9000_STUB_PROJECT -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
