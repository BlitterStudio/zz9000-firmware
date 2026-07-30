param(
    [Parameter(Mandatory = $true)]
    [string]$ResultLog,

    [Parameter(Mandatory = $true)]
    [string]$Evidence
)

$ErrorActionPreference = "Stop"

function Require-Match {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Description
    )

    $match = [regex]::Match(
        $Text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if (-not $match.Success) {
        throw "Missing $Description in $ResultLog"
    }
    return $match
}

function Require-EvidenceNumber {
    param(
        [object]$Object,
        [string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "Missing evidence field '$Name' in $Evidence"
    }
    if ($property.Value -isnot [ValueType]) {
        throw "Evidence field '$Name' must be numeric"
    }
    return [double]$property.Value
}

if (-not (Test-Path -LiteralPath $ResultLog -PathType Leaf)) {
    throw "Result log not found: $ResultLog"
}
if (-not (Test-Path -LiteralPath $Evidence -PathType Leaf)) {
    throw "Evidence file not found: $Evidence"
}

$text = Get-Content -LiteralPath $ResultLog -Raw
$evidenceObject = Get-Content -LiteralPath $Evidence -Raw |
    ConvertFrom-Json

$summary = Require-Match $text (
    'zzplay:\s+(\d+)\s+decoded,\s+(\d+)\s+presented,\s+' +
    '(\d+)\s+discarded frames,\s+(\d+)\s+audio frames played,\s+' +
    '(\d+)\s+underruns') "playback summary"
$drift = Require-Match $text (
    'zzplay:\s+A/V drift current\s+(-?\d+)\s+ms,\s+max\s+' +
    '(\d+)\s+ms,\s+\d+\s+hold polls,\s+(\d+)\s+late frames') "A/V drift summary"
$average = Require-Match $text (
    'zzplay:\s+average\s+([0-9]+(?:\.[0-9]+)?)\s+fps playback,\s+' +
    '([0-9]+(?:\.[0-9]+)?)\s+fps decode-call') "average FPS summary"
$wall = Require-Match $text (
    'zzplay:\s+profile wall\s+(\d+)\s+ms') "wall-time profile"

$decoded = [int64]$summary.Groups[1].Value
$presented = [int64]$summary.Groups[2].Value
$discarded = [int64]$summary.Groups[3].Value
$audioFrames = [int64]$summary.Groups[4].Value
$underruns = [int64]$summary.Groups[5].Value

if ($decoded -le 0) {
    throw "Decoded frame count must be positive"
}
if ($presented + $discarded -gt $decoded) {
    throw "Presented plus discarded frames exceed decoded frames"
}

$result = [ordered]@{
    presented_frame_ratio = [double]$presented / [double]$decoded
    decoded_frames = $decoded
    video_complete = [int](
        $text -notmatch '\*\*\*Break' -and
        $decoded -ge 4405)
    formatter_tests_pass = [int](Require-EvidenceNumber `
        $evidenceObject "formatter_tests_pass")
    timing_met = [int](Require-EvidenceNumber `
        $evidenceObject "timing_met")
    exact_size_regression_pass = [int](Require-EvidenceNumber `
        $evidenceObject "exact_size_regression_pass")
    visual_correctness_pass = [int](Require-EvidenceNumber `
        $evidenceObject "visual_correctness_pass")
    audio_frames_played = $audioFrames
    audio_underruns = $underruns
    presented_frames = $presented
    discarded_frames = $discarded
    late_frames = [int64]$drift.Groups[3].Value
    average_playback_fps = [double]$average.Groups[1].Value
    average_decode_fps = [double]$average.Groups[2].Value
    current_drift_ms = [int64]$drift.Groups[1].Value
    max_drift_ms = [int64]$drift.Groups[2].Value
    wall_ms = [int64]$wall.Groups[1].Value
    formatter_setup_wns_ns = Require-EvidenceNumber `
        $evidenceObject "formatter_setup_wns_ns"
    formatter_hold_whs_ns = Require-EvidenceNumber `
        $evidenceObject "formatter_hold_whs_ns"
}

$result | ConvertTo-Json -Compress
