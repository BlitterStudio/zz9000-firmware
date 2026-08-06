# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
Rebuild the default ZZ9000 FPGA bitstream with Windows Vivado 2018.3.

.DESCRIPTION
Regenerates the Vivado project from zz9000_project.tcl, runs synthesis,
implementation, and write_bitstream, then copies the produced
zz9000_ps_wrapper.bit into bootimage_work/.

.PARAMETER VivadoBat
Path to vivado.bat. If omitted, the script checks VIVADO_BAT, VIVADO_DIR,
D:\Xilinx\Vivado\2018.3\bin\vivado.bat, then
C:\Xilinx\Vivado\2018.3\bin\vivado.bat.

.PARAMETER NoAutoboot
Build a diagnostic bitstream that does not advertise the Zorro autoboot ROM.
#>

[CmdletBinding()]
param(
    [string]$VivadoBat,
    [switch]$NoAutoboot
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

function Resolve-VivadoBat {
    param([string]$RequestedPath)

    $candidates = @()
    if ($RequestedPath) {
        $candidates += $RequestedPath
    }
    if ($env:VIVADO_BAT) {
        $candidates += $env:VIVADO_BAT
    }
    if ($env:VIVADO_DIR) {
        if ([System.IO.Path]::GetFileName($env:VIVADO_DIR) -ieq 'vivado.bat') {
            $candidates += $env:VIVADO_DIR
        } else {
            $candidates += (Join-Path $env:VIVADO_DIR 'bin\vivado.bat')
        }
    }
    $candidates += 'D:\Xilinx\Vivado\2018.3\bin\vivado.bat'
    $candidates += 'C:\Xilinx\Vivado\2018.3\bin\vivado.bat'

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw 'Vivado 2018.3 not found. Pass -VivadoBat or set VIVADO_BAT/VIVADO_DIR.'
}

function Invoke-Checked {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Wait-ForVivadoExit {
    # A previous batch iteration's Vivado can outlive the process we waited
    # on. Deleting the project while it is still alive is worse than losing
    # the delete: it can recreate files underneath us, and the next
    # regeneration then merges fresh output with stale block-design state.
    # That produces a project whose wrapper is missing every ZORRO_* port and
    # fails elaboration with ~86 errors, which looks like an RTL bug and is
    # not one. Wait for the field to be clear first.
    for ($i = 0; $i -lt 60; $i++) {
        $running = @(Get-Process -Name 'vivado' -ErrorAction SilentlyContinue)
        if ($running.Count -eq 0) {
            return
        }
        if ($i -eq 0) {
            Write-Host '[bitstream] waiting for a previous Vivado to exit'
        }
        Start-Sleep -Seconds 2
    }
    throw 'A Vivado process is still running after 120s; refusing to delete the project underneath it.'
}

function Remove-GeneratedProject {
    param([string]$Path)

    Wait-ForVivadoExit

    # Even after Vivado exits, handles under the run directories linger for a
    # moment, and on-access virus scanning of the freshly written bitstream
    # extends that window. Back-to-back variant builds delete the project the
    # instant the previous run returns, which loses that race and fails the
    # whole batch several variants in. Retry before giving up.
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        } catch [System.IO.IOException], [System.UnauthorizedAccessException] {
            if ($attempt -eq 10) {
                throw
            }
            Write-Host "[bitstream] project still locked, retrying ($attempt/9)"
            [System.GC]::Collect()
            Start-Sleep -Seconds 3
            continue
        }

        # Remove-Item can report success having left entries behind. A
        # partially deleted project is exactly the state that corrupts the
        # next regeneration, so confirm rather than assume.
        if (-not (Test-Path -LiteralPath $Path)) {
            return
        }
        if ($attempt -eq 10) {
            throw "Generated project still present after removal: $Path"
        }
        Write-Host "[bitstream] project partially removed, retrying ($attempt/9)"
        Start-Sleep -Seconds 3
    }
}

$scriptDir = Split-Path -Parent $PSCommandPath
$repoRoot = [System.IO.Path]::GetFullPath($scriptDir)
Set-Location -LiteralPath $repoRoot

$vivado = Resolve-VivadoBat -RequestedPath $VivadoBat
Write-Host "[bitstream] Vivado: $vivado"
if ($NoAutoboot) {
    Write-Host '[bitstream] autoboot ROM: disabled'
}

$projectArgs = @('--origin_dir', '.')
if ($NoAutoboot) {
    $projectArgs += '--no-autoboot'
}

$projectDir = Join-Path $repoRoot 'ZZ9000_proto'
if (Test-Path -LiteralPath $projectDir) {
    $resolvedProject = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $projectDir).Path)
    $rootPrefix = $repoRoot.TrimEnd('\') + '\'
    if (-not $resolvedProject.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete generated project outside repo: $resolvedProject"
    }

    Write-Host '[bitstream] removing old generated project'
    Remove-GeneratedProject -Path $resolvedProject
}

Write-Host '[bitstream] regenerating project from zz9000_project.tcl'
$vivadoProjectArgs = @(
    '-mode', 'batch',
    '-source', 'zz9000_project.tcl',
    '-tclargs'
) + $projectArgs
Invoke-Checked -FilePath $vivado -Arguments $vivadoProjectArgs

Write-Host '[bitstream] running synthesis + implementation + write_bitstream'
Invoke-Checked -FilePath $vivado -Arguments @(
    '-mode', 'batch',
    '-source', 'build_run_synthesis.tcl'
)

$bitstream = Get-ChildItem -LiteralPath $projectDir -Recurse -Filter 'zz9000_ps_wrapper.bit' |
    Where-Object { $_.FullName -match '[\\/]impl_1[\\/]' } |
    Select-Object -First 1

if (-not $bitstream) {
    throw 'Bitstream not produced; check ZZ9000_proto/ZZ9000_proto.runs/impl_1/runme.log.'
}

$output = Join-Path $repoRoot 'bootimage_work\zz9000_ps_wrapper.bit'
Copy-Item -LiteralPath $bitstream.FullName -Destination $output -Force

Write-Host "[bitstream] done: $output"
Write-Host '[bitstream] NB: commit bootimage_work/zz9000_ps_wrapper.bit so CI picks up HDL changes.'
