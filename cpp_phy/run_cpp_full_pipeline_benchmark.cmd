@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b 1
set "OPENISAC_FULL_PIPELINE_OUT=measurement\cpp_full_pipeline_benchmark"
if not exist "%OPENISAC_FULL_PIPELINE_OUT%" mkdir "%OPENISAC_FULL_PIPELINE_OUT%"
set "OPENISAC_FULL_PIPELINE_FRAMES=1000"
if not "%~1"=="" set "OPENISAC_FULL_PIPELINE_FRAMES=%~1"
cpp_phy\build\ninja-vs2019\openisac_phy_full_pipeline_benchmark.exe "%OPENISAC_FULL_PIPELINE_OUT%\frames.csv" "%OPENISAC_FULL_PIPELINE_FRAMES%"
if errorlevel 1 exit /b 1
set "OPENISAC_PLOT_PYTHON=python_phy\.venv\Scripts\python.exe"
if defined OPENISAC_PYTHON set "OPENISAC_PLOT_PYTHON=%OPENISAC_PYTHON%"
"%OPENISAC_PLOT_PYTHON%" cpp_phy\plot_full_pipeline_benchmark.py "%OPENISAC_FULL_PIPELINE_OUT%\frames.csv" --output-dir "%OPENISAC_FULL_PIPELINE_OUT%"
exit /b %errorlevel%
