@echo off
setlocal EnableExtensions
if "%~4"=="" (
  echo Usage: run_xsim.bat ROOT OUT SCENE LATENCY
  exit /b 2
)
set ROOT=%~1
set OUT=%~2
set SCENE=%~3
set LATENCY=%~4
if "%POLL_LIMIT%"=="" set POLL_LIMIT=500000
if "%TRACE_PIPE%"=="" set TRACE_PIPE=0
set OUT_FWD=%OUT:\=/%
if not exist "%OUT%" mkdir "%OUT%"
call D:\Xilinx\Vivado\2018.3\settings64.bat
if errorlevel 1 exit /b %errorlevel%
cd /d "%OUT%"
if exist xsim.dir rmdir /s /q xsim.dir
set SIMROOT=%OUT%\sim_rtl
call py -3 "%ROOT%\test\rix_xsim\prepare_xsim_sources.py" "%ROOT%" "%SIMROOT%"
if errorlevel 1 exit /b %errorlevel%
for %%F in ("%SIMROOT%\3rdParty\*.v") do call xvlog --relax -sv -d UNITTEST -i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis" "%%F"
if errorlevel 1 exit /b %errorlevel%
for %%F in ("%SIMROOT%\3rdParty\verilog-axi\*.v") do call xvlog --relax -sv -d UNITTEST -i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis" "%%F"
if errorlevel 1 exit /b %errorlevel%
for %%F in ("%SIMROOT%\3rdParty\verilog-axis\*.v") do call xvlog --relax -sv -d UNITTEST -i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis" "%%F"
if errorlevel 1 exit /b %errorlevel%
for %%F in ("%SIMROOT%\Float\rtl\float\*.v") do call xvlog --relax -sv -d UNITTEST -i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis" "%%F"
if errorlevel 1 exit /b %errorlevel%
for %%F in ("%SIMROOT%\RasterIX\*.v") do call xvlog --relax -sv -d UNITTEST -i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis" "%%F"
if errorlevel 1 exit /b %errorlevel%
call xvlog --relax -sv -d UNITTEST -i "%SIMROOT%\RasterIX" -i "%SIMROOT%\Float\rtl\float" -i "%SIMROOT%\3rdParty" -i "%SIMROOT%\3rdParty\verilog-axi" -i "%SIMROOT%\3rdParty\verilog-axis" "%ROOT%\rtl\zz9k_rix\zz9k_cmd_fetcher.v" "%ROOT%\rtl\zz9k_rix\zz9k_rasterix.v" "%ROOT%\test\rix_xsim\tb_zz9k_rasterix.sv"
if errorlevel 1 exit /b %errorlevel%
call xvlog --relax "%XILINX_VIVADO%\data\verilog\src\glbl.v"
if errorlevel 1 exit /b %errorlevel%
call xelab -debug typical -timescale 1ns/1ps -L xpm -L unisims_ver -top tb_zz9k_rasterix -top glbl -snapshot tb_zz9k_rasterix_sim
if errorlevel 1 exit /b %errorlevel%
call xsim tb_zz9k_rasterix_sim -runall --testplusarg "CMD=%OUT_FWD%/%SCENE%.hex" --testplusarg "TEXTURE=%OUT_FWD%/textures.hex" --testplusarg "PPM=%OUT_FWD%/%SCENE%.ppm" --testplusarg "SCENE=%SCENE%" --testplusarg "LATENCY=%LATENCY%" --testplusarg "POLL_LIMIT=%POLL_LIMIT%" --testplusarg "TRACE_PIPE=%TRACE_PIPE%"
exit /b %errorlevel%
