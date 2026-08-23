@echo off
setlocal

call "%~dp0build_windows_vs2019.cmd"
if errorlevel 1 exit /b 1

set "OUTPUT_DIR=%~dp0..\measurement\cpp_sensing_plot"
set "MPLCONFIGDIR=%OUTPUT_DIR%\matplotlib-cache"
set "BATCHES=%~1"
if not defined BATCHES set "BATCHES=20"

"%~dp0build\ninja-vs2019\openisac_phy_sensing_benchmark.exe" %BATCHES% "%OUTPUT_DIR%"
if errorlevel 1 exit /b 1

set "PYTHON_EXE=%OPENISAC_PYTHON%"
if not defined PYTHON_EXE if exist "%~dp0..\python_phy\.venv\Scripts\python.exe" set "PYTHON_EXE=%~dp0..\python_phy\.venv\Scripts\python.exe"
if not defined PYTHON_EXE set "PYTHON_EXE=python.exe"

"%PYTHON_EXE%" "%~dp0plot_sensing.py" "%OUTPUT_DIR%"
if errorlevel 1 exit /b 1

echo Sensing plots written to %OUTPUT_DIR%
exit /b 0
