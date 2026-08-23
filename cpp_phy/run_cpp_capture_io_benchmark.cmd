@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b 1
set "OPENISAC_CAPTURE_FRAMES=10000"
if not "%~1"=="" set "OPENISAC_CAPTURE_FRAMES=%~1"
cpp_phy\build\ninja-vs2019\openisac_phy_capture_io_benchmark.exe "%OPENISAC_CAPTURE_FRAMES%"
exit /b %errorlevel%
