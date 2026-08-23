@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b 1
set "OPENISAC_REALTIME_OUT=measurement\cpp_realtime_benchmark"
if not exist "%OPENISAC_REALTIME_OUT%" mkdir "%OPENISAC_REALTIME_OUT%"
set "OPENISAC_REALTIME_FRAMES=200"
if not "%~1"=="" set "OPENISAC_REALTIME_FRAMES=%~1"
cpp_phy\build\ninja-vs2019\openisac_phy_realtime_benchmark.exe "%OPENISAC_REALTIME_OUT%\frames.csv" "%OPENISAC_REALTIME_FRAMES%"
if errorlevel 1 exit /b 1
set "OPENISAC_PLOT_PYTHON=python_phy\.venv\Scripts\python.exe"
if defined OPENISAC_PYTHON set "OPENISAC_PLOT_PYTHON=%OPENISAC_PYTHON%"
"%OPENISAC_PLOT_PYTHON%" cpp_phy\plot_realtime_benchmark.py "%OPENISAC_REALTIME_OUT%\frames.csv" --output-dir "%OPENISAC_REALTIME_OUT%"
exit /b %errorlevel%
