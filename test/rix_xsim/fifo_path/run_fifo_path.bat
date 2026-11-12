@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem Real command-ingress replay: GP1 -> ps7_0_axi_periph -> axi_fifo_mm_s ->
rem RasterIX EF, with the interconnect and FIFO generated from
rem zz9000_project.tcl by fifo_path.tcl.
rem
rem   run_fifo_path.bat build ROOT WORK
rem       Generate the block design, export it, and compile and elaborate it
rem       with the RasterIX EF wrapper and tb_zz9k_rasterix (FIFO_PATH).
rem   run_fifo_path.bat run WORK OUT SCENE LATENCY TRANSPORT
rem       Replay OUT\SCENE.hex / .fifo inline, or the generated memfetch
rem       envelope and DDR files for TRANSPORT=memfetch.  Writes
rem       OUT\SCENE_fifo-TRANSPORT.ppm (except pl330-early).
rem
rem WORK must be an absolute path without spaces.
setlocal EnableExtensions
if /I "%~1"=="build" if not "%~3"=="" goto build
if /I "%~1"=="run" if not "%~6"=="" goto run
echo Usage: run_fifo_path.bat build ROOT WORK
echo        run_fifo_path.bat run WORK OUT SCENE LATENCY TRANSPORT
exit /b 2

:build
set ROOT=%~2
set WORK=%~3
set VIVADO=D:\Xilinx\Vivado\2018.3\bin\vivado.bat
if not exist "%VIVADO%" (
  echo Vivado 2018.3 not found at "%VIVADO%"
  exit /b 1
)
for /L %%A in (1,1,3) do (
  if exist "%WORK%" rmdir /s /q "%WORK%"
  mkdir "%WORK%"
  cd /d "%WORK%"
  call "%VIVADO%" -mode batch -nojournal -log bd.log -source "%ROOT%\test\rix_xsim\fifo_path\fifo_path.tcl" -tclargs "%ROOT%" "%WORK%\bd"
  if not errorlevel 1 goto generated
)
exit /b 1

:generated
set XSIMDIR=%WORK%\bd\export\rix_fifo_path\xsim
if not exist "%XSIMDIR%\vlog.prj" (
  echo fifo_path.tcl did not export "%XSIMDIR%"
  exit /b 1
)
cd /d "%XSIMDIR%"
call D:\Xilinx\Vivado\2018.3\settings64.bat
if errorlevel 1 exit /b %errorlevel%
call xvlog --relax -prj vlog.prj
if errorlevel 1 exit /b %errorlevel%
call xvhdl --relax -prj vhdl.prj
if errorlevel 1 exit /b %errorlevel%
set SIMROOT=%WORK%\sim_rtl
call py -3 "%ROOT%\test\rix_xsim\prepare_xsim_sources.py" "%ROOT%" "%SIMROOT%"
if errorlevel 1 exit /b %errorlevel%
set INCS=-i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis"
for %%D in (3rdParty 3rdParty\verilog-axi 3rdParty\verilog-axis Float\rtl\float RasterIX) do (
  for %%F in ("%SIMROOT%\%%D\*.v") do (
    call xvlog --relax -sv -d UNITTEST %INCS% "%%F"
    if errorlevel 1 exit /b 1
  )
)
call xvlog --relax "%WORK%\bd\export\rix_fifo_path_wrapper.v"
if errorlevel 1 exit /b %errorlevel%
call xvlog --relax -sv -d UNITTEST -d RASTERIX_VARIANT_EF -d FIFO_PATH %INCS% "%ROOT%\rtl\zz9k_rix\zz9k_rasterix.v" "%ROOT%\test\rix_xsim\tb_zz9k_rasterix.sv"
if errorlevel 1 exit /b %errorlevel%
call xelab --relax -debug typical -timescale 1ns/1ps -L work -L xil_defaultlib -L xpm -L axi_lite_ipif_v3_0_4 -L lib_pkg_v1_0_2 -L fifo_generator_v13_2_3 -L lib_fifo_v1_0_12 -L axi_fifo_mm_s_v4_2_0 -L generic_baseblocks_v2_1_0 -L axi_infrastructure_v1_1_0 -L axi_register_slice_v2_1_18 -L axi_data_fifo_v2_1_17 -L axi_crossbar_v2_1_19 -L axi_protocol_converter_v2_1_18 -L axi_clock_converter_v2_1_17 -L unisims_ver -L unimacro_ver -L secureip -top work.tb_zz9k_rasterix -top xil_defaultlib.glbl -snapshot tb_fifo_path
exit /b %errorlevel%

:run
set WORK=%~2
set OUT=%~3
set SCENE=%~4
set LATENCY=%~5
set TRANSPORT=%~6
if "%POLL_LIMIT%"=="" set POLL_LIMIT=2000000
set OUT_FWD=%OUT:\=/%
rem transport "irq" replays over mmio with FIFO_IRQ=1: the directed
rem gpu_cmd_fifo interrupt check (IER masking, the 1024-word TFPE
rem threshold and the W1C clear) instead of a frame comparison.  The
rem log keeps the requested transport name.
set FIFO_IRQ_ARG=
set SIM_TRANSPORT=%TRANSPORT%
if /I "%TRANSPORT%"=="irq" (
  set SIM_TRANSPORT=mmio
  set FIFO_IRQ_ARG=--testplusarg "FIFO_IRQ=1"
)
set CMD_NAME=%SCENE%.hex
set FIFO_NAME=%SCENE%.fifo
set CMD_DDR_ARG=
if /I "%TRANSPORT%"=="memfetch" (
  set CMD_NAME=%SCENE%_memfetch.hex
  set FIFO_NAME=%SCENE%_memfetch.fifo
  set CMD_DDR_ARG=--testplusarg "CMD_DDR=%OUT_FWD%/%SCENE%_memfetch.ddr"
)
call D:\Xilinx\Vivado\2018.3\settings64.bat
if errorlevel 1 exit /b %errorlevel%
cd /d "%WORK%\bd\export\rix_fifo_path\xsim"
call xsim tb_fifo_path -runall -log "%OUT%\%SCENE%_fifo-%TRANSPORT%.xsim.log" --testplusarg "CMD=%OUT_FWD%/%CMD_NAME%" --testplusarg "FIFO=%OUT_FWD%/%FIFO_NAME%" --testplusarg "FIFO_TRANSPORT=%SIM_TRANSPORT%" %CMD_DDR_ARG% %FIFO_IRQ_ARG% --testplusarg "TEXTURE=%OUT_FWD%/textures.hex" --testplusarg "PPM=%OUT_FWD%/%SCENE%_fifo-%TRANSPORT%.ppm" --testplusarg "SCENE=%SCENE%" --testplusarg "LATENCY=%LATENCY%" --testplusarg "POLL_LIMIT=%POLL_LIMIT%" --testplusarg "TRACE_PIPE=0"
exit /b %errorlevel%
