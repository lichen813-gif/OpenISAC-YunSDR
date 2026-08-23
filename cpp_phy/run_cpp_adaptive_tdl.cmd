@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b 1
set "OPENISAC_ADAPTIVE_OUT=measurement\cpp_adaptive_tdl"
if not exist "%OPENISAC_ADAPTIVE_OUT%" mkdir "%OPENISAC_ADAPTIVE_OUT%"
set "OPENISAC_ADAPTIVE_FRAMES=100"
if not "%~1"=="" set "OPENISAC_ADAPTIVE_FRAMES=%~1"
cpp_phy\build\ninja-vs2019\openisac_phy_adaptive_regression.exe "%OPENISAC_ADAPTIVE_OUT%\frames.csv" "%OPENISAC_ADAPTIVE_FRAMES%"
if errorlevel 1 exit /b 1
set "OPENISAC_PLOT_PYTHON=python_phy\.venv\Scripts\python.exe"
if defined OPENISAC_PYTHON set "OPENISAC_PLOT_PYTHON=%OPENISAC_PYTHON%"
"%OPENISAC_PLOT_PYTHON%" cpp_phy\plot_adaptive_tdl.py "%OPENISAC_ADAPTIVE_OUT%\frames.csv" --output-dir "%OPENISAC_ADAPTIVE_OUT%"
exit /b %errorlevel%
