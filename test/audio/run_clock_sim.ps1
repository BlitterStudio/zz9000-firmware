param(
    [string]$VivadoRoot = 'D:\Xilinx\Vivado\2018.3'
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$rtl = Join-Path $repoRoot 'ZZ9000_proto.srcs\sources_1\new\audio_clock.v'
$testbench = Join-Path $PSScriptRoot 'audio_clock_tb.sv'
$receiverTestbench = Join-Path $PSScriptRoot 'audio_clock_i2s_rx_tb.sv'
$receiverWrapper = Join-Path $repoRoot (
    'ZZ9000_proto\ZZ9000_proto.srcs\sources_1\bd\zz9000_ps\ip\' +
    'zz9000_ps_i2s_receiver_0_0\synth\zz9000_ps_i2s_receiver_0_0.sv'
)
$receiverImplementation = Join-Path $repoRoot (
    'ZZ9000_proto\ZZ9000_proto.srcs\sources_1\bd\zz9000_ps\ipshared\' +
    '7590\hdl\i2s_receiver_v1_0_rfs.sv'
)
$vivadoGlobal = Join-Path $VivadoRoot 'data\verilog\src\glbl.v'
$xvlog = Join-Path $VivadoRoot 'bin\xvlog.bat'
$xelab = Join-Path $VivadoRoot 'bin\xelab.bat'
$xsim = Join-Path $VivadoRoot 'bin\xsim.bat'
$work = Join-Path ([System.IO.Path]::GetTempPath()) (
    'zz9000-audio-clock-sim-' + [guid]::NewGuid().ToString('N')
)

foreach ($tool in @($xvlog, $xelab, $xsim)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Vivado simulator tool not found: $tool"
    }
}

New-Item -ItemType Directory -Path $work | Out-Null

try {
    Push-Location $work

    & $xvlog -sv $rtl $testbench
    if ($LASTEXITCODE -ne 0) {
        throw "xvlog failed with exit code $LASTEXITCODE"
    }

    & $xelab audio_clock_tb -s audio_clock_tb_sim
    if ($LASTEXITCODE -ne 0) {
        throw "xelab failed with exit code $LASTEXITCODE"
    }

    $simOutput = & $xsim audio_clock_tb_sim -runall 2>&1
    $simOutput | ForEach-Object { Write-Output $_ }
    $simText = $simOutput -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0 -or
        $simText -match '(?m)^(Fatal|Error):' -or
        $simText -notmatch 'audio_clock fixed TDM8 slot-0/1 bridge PASS') {
        throw "xsim failed with exit code $LASTEXITCODE"
    }

    & $xvlog -sv $rtl $receiverImplementation $receiverWrapper `
        $receiverTestbench $vivadoGlobal
    if ($LASTEXITCODE -ne 0) {
        throw "receiver integration xvlog failed with exit code $LASTEXITCODE"
    }

    & $xelab -L xpm audio_clock_i2s_rx_tb glbl `
        -s audio_clock_i2s_rx_tb_sim
    if ($LASTEXITCODE -ne 0) {
        throw "receiver integration xelab failed with exit code $LASTEXITCODE"
    }

    $receiverOutput = & $xsim audio_clock_i2s_rx_tb_sim -runall 2>&1
    $receiverOutput | ForEach-Object { Write-Output $_ }
    $receiverText = $receiverOutput -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0 -or
        $receiverText -match '(?m)^(Fatal|Error):' -or
        $receiverText -notmatch (
            'audio_clock \+ Xilinx I2S receiver fixed TDM PASS')) {
        throw "receiver integration xsim failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
    Remove-Item -LiteralPath $work -Recurse -Force
}
