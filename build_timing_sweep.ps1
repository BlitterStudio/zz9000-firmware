# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
Build one release variant through the ordered placement-directive timing sweep.

.DESCRIPTION
Each directive is built from a clean Vivado project. The first build to pass
all gates is selected. If none pass, the attempt with the largest minimum
margin normalised to the four release floors is retained for diagnosis.
Build evidence is written under build/timing/<variant>/. A passing sweep also
updates the selected variant's qualification record without copying bitstreams.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Variant,
    [string]$BuildId,
    [string]$VivadoBat,
    [switch]$RasterIX
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSCommandPath))
Set-Location -LiteralPath $repoRoot
if (-not $BuildId) {
    $BuildId = (& git -C $repoRoot rev-parse --short=8 HEAD).Trim()
}
if ($BuildId -notmatch '^[0-9A-Fa-f]{8}$') {
    throw "BuildId must be exactly eight hexadecimal digits; got '$BuildId'."
}
$BuildId = $BuildId.ToLowerInvariant()

$directives = @('Default', 'Explore', 'ExtraTimingOpt', 'ExtraNetDelay_high', 'AltSpreadLogic_high')
$floors = [ordered]@{ PixelSetupNs = 0.100; PixelHoldNs = 0.000; OverallSetupNs = 0.100; I2sSetupNs = 0.100 }
# The hold gate is the 0.300 ns clock uncertainty, rather than a positive
# slack floor. Use it as the hold normalizer so comparison remains sensitive
# to a marginal runtime-pixel hold result without dividing by zero.
$normalizers = [ordered]@{ PixelSetupNs = 0.100; PixelHoldNs = 0.300; OverallSetupNs = 0.100; I2sSetupNs = 0.100 }
$variantMatrixPath = Join-Path $repoRoot 'bootimage_work\variants\variant-matrix.sh'

function Get-VariantDefinition {
    param([string]$Name)

    if (-not (Test-Path -LiteralPath $variantMatrixPath)) {
        throw "Missing canonical variant matrix '$variantMatrixPath'."
    }
    foreach ($line in Get-Content -LiteralPath $variantMatrixPath) {
        $match = [regex]::Match($line, "^\s*'(?<row>[^']+)'\s*$")
        if (-not $match.Success) { continue }
        $fields = $match.Groups['row'].Value -split '\|'
        # Rows carry an optional trailing firmware-flavor column (e.g. the
        # rasterix-only zorro3-aga-rix variant); tolerate 10 or 11 fields.
        if ($fields.Count -ne 10 -and $fields.Count -ne 11) {
            throw "Invalid canonical variant matrix row '$($match.Groups['row'].Value)'."
        }
        if ($fields[0] -ne $Name) { continue }
        return [pscustomobject][ordered]@{
            Name = $fields[0]; Bitstream = $fields[1]; Label = $fields[2]
            Zorro2 = [int]$fields[3]; Zorro3 = [int]$fields[4]; Zz9500 = [int]$fields[5]
            TwoMb = [int]$fields[6]; SuperDenise = [int]$fields[7]; Z3FastRam = [int]$fields[8]
            CaptureC28 = [int]$fields[9]
            Flavor = if ($fields.Count -eq 11) { $fields[10] } else { '' }
        }
    }
    throw "Unknown release variant '$Name' in canonical variant matrix."
}

function Get-DefineLine {
    param([string]$Name, [int]$Enabled, [string]$Comment = '')
    $prefix = if ($Enabled -eq 1) { '`define' } else { '//`define' }
    return '{0} {1}{2}' -f $prefix, $Name, $Comment
}

function Get-VariantBlock {
    param([pscustomobject]$Definition)
    return (@(
        '// ZORRO2/3 switch'
        Get-DefineLine 'ZORRO2' $Definition.Zorro2
        Get-DefineLine 'ZORRO3' $Definition.Zorro3
        ''
        '// use only together with ZORRO2:'
        Get-DefineLine 'VARIANT_ZZ9500' $Definition.Zz9500 '        // uses Denise adapter/A500 specific video capture'
        Get-DefineLine 'VARIANT_2MB' $Definition.TwoMb '           // uses only 2MB address space'
        Get-DefineLine 'VARIANT_SUPERDENISE' $Definition.SuperDenise '   // for A500+ and super denise'
        ''
        '//`define VARIANT_FW20'
        Get-DefineLine 'VARIANT_Z3_FASTRAM' $Definition.Z3FastRam
        '`define VARIANT_AUTOBOOT        // enable autoboot ROM'
        ''
        '`define C_S_AXI_DATA_WIDTH 32'
    ) -join "`n") + "`n"
}

function Get-Slack {
    param([string]$Log, [string]$Name)
    $match = [regex]::Match($Log, "$Name=([-+]?\d+(?:\.\d+)?)ns")
    if ($match.Success) { return [double]$match.Groups[1].Value }
    return $null
}

function Copy-Reports {
    param([string]$Destination)
    $reports = Join-Path $snapshotRoot 'ZZ9000_proto\timing_reports'
    if (Test-Path -LiteralPath $reports) {
        Copy-Item -LiteralPath $reports -Destination (Join-Path $Destination 'reports') -Recurse -Force
    }
}

function Get-Sha256 {
    param([string]$Path)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha256.ComputeHash([IO.File]::ReadAllBytes($Path))) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

function Update-QualificationRecord {
    param(
        [pscustomobject]$Definition,
        [string]$SelectedBitstream,
        [pscustomobject]$SelectedAttempt
    )

    $recordPath = Join-Path $repoRoot 'bootimage_work\variants\timing-qualification.json'
    $record = if (Test-Path -LiteralPath $recordPath) {
        Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    } else {
        [pscustomobject][ordered]@{ schema_version = 1; variants = [pscustomobject]@{} }
    }
    if ($record.schema_version -ne 1) {
        throw "Unsupported timing qualification schema in '$recordPath'."
    }
    if (-not $record.PSObject.Properties['variants']) {
        $record | Add-Member -NotePropertyName variants -NotePropertyValue ([pscustomobject]@{})
    }

    $sourceRevision = (& git -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not determine the source revision for the timing qualification.'
    }
    $artifactName = if ($RasterIX) {
        'bootimage_work/variants/zz9000_ps_wrapper-zorro3-aga-rix.bit'
    } else {
        $Definition.Bitstream
    }
    $recordName = if ($RasterIX) { "$($Definition.Name)-rix" } else { $Definition.Name }
    $qualification = [pscustomobject][ordered]@{
        bitstream = $artifactName
        sha256 = Get-Sha256 $SelectedBitstream
        source_revision = $sourceRevision
        build_id = $BuildId
        placement_directive = $SelectedAttempt.Directive
        pixel_slack_ns = [ordered]@{ setup = $SelectedAttempt.PixelSetupNs; hold = $SelectedAttempt.PixelHoldNs }
        overall_slack_ns = [ordered]@{ setup = $SelectedAttempt.OverallSetupNs; hold = $SelectedAttempt.OverallHoldNs }
        i2s_slack_ns = [ordered]@{ setup = $SelectedAttempt.I2sSetupNs }
        sii9022_slack_ns = [ordered]@{ setup = $SelectedAttempt.Sii9022SetupNs; hold = $SelectedAttempt.Sii9022HoldNs }
        gate_passed = $true
    }
    $record.variants | Add-Member -NotePropertyName $recordName -NotePropertyValue $qualification -Force
    [IO.File]::WriteAllText(
        $recordPath,
        (($record | ConvertTo-Json -Depth 8) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false)
    )
}

function Write-Checksums {
    param([string]$Directory)
    $checksumPath = Join-Path $Directory 'SHA256SUMS'
    $lines = Get-ChildItem -LiteralPath $Directory -Recurse -File |
        Where-Object { $_.FullName -ne $checksumPath } |
        ForEach-Object {
            $relative = $_.FullName.Substring($Directory.Length).TrimStart('\', '/') -replace '\\', '/'
            '{0}  {1}' -f (Get-Sha256 $_.FullName), $relative
        } | Sort-Object
    [IO.File]::WriteAllLines($checksumPath, [string[]]$lines, [Text.UTF8Encoding]::new($false))
}

function Write-AggregateSummary {
    param([string]$TimingRoot)
    $summaries = Get-ChildItem -LiteralPath $TimingRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { $summary = Join-Path $_.FullName 'sweep-summary.json'; if (Test-Path -LiteralPath $summary) { Get-Content -LiteralPath $summary -Raw | ConvertFrom-Json } } |
        Sort-Object Variant
    $lines = @('# Timing placement sweep', '', '| Variant | Chosen directive | Attempts | Pixel setup / hold (ns) | Overall / I2S setup (ns) | SiI9022 setup / hold (ns) | Gate result |', '| --- | --- | --- | --- | --- | --- | --- |')
    foreach ($summary in $summaries) {
        $selected = $summary.SelectedAttempt
        $directive = if ($summary.ChosenDirective) { $summary.ChosenDirective } else { 'none' }
        $attempts = [string]::Join(', ', @($summary.Attempts | ForEach-Object { $_.Directive }))
        $pixel = if ($selected) { '{0:N3} / {1:N3}' -f $selected.PixelSetupNs, $selected.PixelHoldNs } else { 'n/a' }
        $overall = if ($selected) { '{0:N3} / {1:N3}' -f $selected.OverallSetupNs, $selected.I2sSetupNs } else { 'n/a' }
        $sii = if ($selected) { '{0:N3} / {1:N3}' -f $selected.Sii9022SetupNs, $selected.Sii9022HoldNs } else { 'n/a' }
        $gate = if ($summary.Passed) { 'PASS' } else { 'FAIL (best retained for analysis)' }
        $lines += "| $($summary.Variant) | $directive | $attempts | $pixel | $overall | $sii | $gate |"
    }
    [IO.File]::WriteAllLines((Join-Path $TimingRoot 'timing-sweep-summary.md'), [string[]]$lines, [Text.UTF8Encoding]::new($false))
}

$variantDefinition = Get-VariantDefinition $Variant
if ($RasterIX -and $Variant -ne 'zorro3-aga' -and $Variant -ne 'zorro3-aga-rix') {
    throw 'RasterIX timing qualification is supported only for zorro3-aga and zorro3-aga-rix.'
}

$timingRoot = Join-Path $repoRoot 'build\timing'
$stageDir = Join-Path $timingRoot $Variant
$attemptsDir = Join-Path $stageDir 'attempts'
# Variant defines must never be written into the checked-out branch. The
# generated project itself also belongs in this disposable source worktree.
$snapshotRoot = Join-Path ([IO.Path]::GetTempPath()) "zz9000-timing-$Variant-$BuildId"
$snapshotAdded = $false
if (Test-Path -LiteralPath $snapshotRoot) {
    & git -C $repoRoot worktree remove --force $snapshotRoot
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $snapshotRoot)) {
        throw "Could not remove stale sweep source worktree '$snapshotRoot'."
    }
}
& git -C $repoRoot worktree add --detach $snapshotRoot HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not create isolated sweep source worktree '$snapshotRoot'."
}
$snapshotAdded = $true
$sourcePath = Join-Path $snapshotRoot 'mntzorro.v'
$dcpPath = Join-Path $snapshotRoot 'ZZ9000_proto\ZZ9000_proto.runs\impl_1\zz9000_ps_wrapper_postroute_physopt.dcp'
Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $attemptsDir -Force | Out-Null

$sourceBytes = [IO.File]::ReadAllBytes($sourcePath)
$sourceText = [Text.Encoding]::UTF8.GetString($sourceBytes)
$variantPattern = '(?ms)^// ZORRO2/3 switch\r?\n.*?^`define C_S_AXI_DATA_WIDTH 32\r?\n'
if (-not [regex]::IsMatch($sourceText, $variantPattern)) { throw 'Could not locate the variant define block in mntzorro.v.' }
$updatedSource = [regex]::Replace($sourceText, $variantPattern, (Get-VariantBlock $variantDefinition), 1)
$oldDirective = $env:ZZ9000_PLACE_DIRECTIVE
$oldBuildId = $env:VCAP_DIAG_BUILD_ID
$oldProcessors = $env:NUMBER_OF_PROCESSORS
$attempts = @()
$chosenAttempt = $null
$bestAttempt = $null

try {
    [IO.File]::WriteAllBytes($sourcePath, [Text.UTF8Encoding]::new($false).GetBytes($updatedSource))
    $env:VCAP_DIAG_BUILD_ID = $BuildId
    $env:NUMBER_OF_PROCESSORS = '4'
    foreach ($directive in $directives) {
        $attemptDir = Join-Path $attemptsDir $directive
        New-Item -ItemType Directory -Path $attemptDir -Force | Out-Null
        $bitstream = Join-Path $attemptDir 'zz9000_ps_wrapper.bit'
        $logPath = Join-Path $attemptDir 'build.log'
        $env:ZZ9000_PLACE_DIRECTIVE = $directive
        $builderArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $snapshotRoot 'build_bitstream.ps1'), '-OutputBitstream', $bitstream)
        if ($VivadoBat) { $builderArgs += @('-VivadoBat', $VivadoBat) }
        if ($variantDefinition.CaptureC28 -eq 1) { $builderArgs += '-CaptureC28' }
        if ($RasterIX) { $builderArgs += '-RasterIX' }
        Write-Host "[timing-sweep] ${Variant}: $directive"
        $savedErrorActionPreference = $ErrorActionPreference
        try {
            # A timing-floor failure is an expected sweep result, not a driver
            # failure. Preserve the nonzero child status and continue to the
            # next directive after capturing its log and reports.
            $ErrorActionPreference = 'Continue'
            & powershell.exe @builderArgs 2>&1 | Tee-Object -FilePath $logPath
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        Copy-Reports -Destination $attemptDir
        if (Test-Path -LiteralPath $dcpPath) { Copy-Item -LiteralPath $dcpPath -Destination (Join-Path $attemptDir 'postroute-physopt.dcp') -Force }
        $log = Get-Content -LiteralPath $logPath -Raw
        $attempt = [pscustomobject][ordered]@{
            Directive = $directive; ExitCode = $exitCode
            PixelSetupNs = Get-Slack $log 'runtime_150mhz_setup_slack'; PixelHoldNs = Get-Slack $log 'runtime_150mhz_hold_slack'
            OverallSetupNs = Get-Slack $log 'overall_setup_slack'; OverallHoldNs = Get-Slack $log 'overall_hold_slack'; I2sSetupNs = Get-Slack $log 'i2s_setup_slack'
            Sii9022SetupNs = Get-Slack $log 'sii9022_output_setup_slack'; Sii9022HoldNs = Get-Slack $log 'sii9022_output_hold_slack'
            GatePassed = ($exitCode -eq 0 -and $log -match 'TIMING_GATE: PASS - runtime pixel, overall, and i2s setup floors met')
            GateLines = @($log -split "`r?`n" | Where-Object { $_ -match 'TIMING_GATE:|Timing floor failed|Timing failed in' })
        }
        if ($null -ne $attempt.PixelSetupNs -and $null -ne $attempt.PixelHoldNs -and $null -ne $attempt.OverallSetupNs -and $null -ne $attempt.I2sSetupNs) {
            $margins = @($attempt.PixelSetupNs / $normalizers.PixelSetupNs; $attempt.PixelHoldNs / $normalizers.PixelHoldNs; $attempt.OverallSetupNs / $normalizers.OverallSetupNs; $attempt.I2sSetupNs / $normalizers.I2sSetupNs)
            $attempt | Add-Member -NotePropertyName NormalizedMinimumMargin -NotePropertyValue (($margins | Measure-Object -Minimum).Minimum)
        } else { $attempt | Add-Member -NotePropertyName NormalizedMinimumMargin -NotePropertyValue $null }
        $attempts += $attempt
        if ($null -eq $bestAttempt -or ($null -ne $attempt.NormalizedMinimumMargin -and ($null -eq $bestAttempt.NormalizedMinimumMargin -or $attempt.NormalizedMinimumMargin -gt $bestAttempt.NormalizedMinimumMargin))) { $bestAttempt = $attempt }
        if ($attempt.GatePassed) { $chosenAttempt = $attempt; break }
    }
} finally {
    [IO.File]::WriteAllBytes($sourcePath, $sourceBytes)
    if ($snapshotAdded) {
        & git -C $repoRoot worktree remove --force $snapshotRoot
    }
    $env:ZZ9000_PLACE_DIRECTIVE = $oldDirective
    $env:VCAP_DIAG_BUILD_ID = $oldBuildId
    $env:NUMBER_OF_PROCESSORS = $oldProcessors
}

$selectedAttempt = if ($chosenAttempt) { $chosenAttempt } else { $bestAttempt }
if ($selectedAttempt) {
    $selectedDir = Join-Path $attemptsDir $selectedAttempt.Directive
    if ($chosenAttempt) { Copy-Item -LiteralPath (Join-Path $selectedDir 'zz9000_ps_wrapper.bit') -Destination (Join-Path $stageDir 'zz9000_ps_wrapper.bit') -Force }
    if (Test-Path -LiteralPath (Join-Path $selectedDir 'reports')) { Copy-Item -LiteralPath (Join-Path $selectedDir 'reports') -Destination (Join-Path $stageDir 'reports') -Recurse -Force }
    if (-not $chosenAttempt -and (Test-Path -LiteralPath (Join-Path $selectedDir 'postroute-physopt.dcp'))) { Copy-Item -LiteralPath (Join-Path $selectedDir 'postroute-physopt.dcp') -Destination (Join-Path $stageDir 'best-postroute-physopt.dcp') -Force }
}
$summary = [pscustomobject][ordered]@{ Variant = $Variant; BuildId = $BuildId; Directives = $directives; FloorsNs = $floors; Passed = ($null -ne $chosenAttempt); ChosenDirective = if ($chosenAttempt) { $chosenAttempt.Directive } else { $null }; SelectedAttempt = $selectedAttempt; Attempts = $attempts }
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stageDir 'sweep-summary.json') -Encoding utf8
Write-Checksums -Directory $stageDir
Write-AggregateSummary -TimingRoot $timingRoot
if ($chosenAttempt) {
    Update-QualificationRecord -Definition $variantDefinition -SelectedBitstream (Join-Path $stageDir 'zz9000_ps_wrapper.bit') -SelectedAttempt $chosenAttempt
}
if (-not $chosenAttempt) {
    $best = if ($bestAttempt) { "$($bestAttempt.Directive), normalized minimum margin $($bestAttempt.NormalizedMinimumMargin)" } else { 'no gate values extracted' }
    throw "No placement directive passed for $Variant; retained $best under $stageDir. See its final post-route physical-opt reports."
}
Write-Host "[timing-sweep] PASS: $Variant with $($chosenAttempt.Directive) (build ID $BuildId)"
