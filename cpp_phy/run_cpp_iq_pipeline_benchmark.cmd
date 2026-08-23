@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b 1
set "OPENISAC_IQ_PIPELINE_OUT=measurement\cpp_iq_pipeline_benchmark"
if not exist "%OPENISAC_IQ_PIPELINE_OUT%" mkdir "%OPENISAC_IQ_PIPELINE_OUT%"
set "OPENISAC_IQ_PIPELINE_FRAMES=1000"
if not "%~1"=="" set "OPENISAC_IQ_PIPELINE_FRAMES=%~1"
set "OPENISAC_IQ_PIPELINE_CORPUS=256"
if not "%~2"=="" set "OPENISAC_IQ_PIPELINE_CORPUS=%~2"
set "OPENISAC_IQ_PIPELINE_SCHEDULING=normal"
if not "%~3"=="" set "OPENISAC_IQ_PIPELINE_SCHEDULING=%~3"
cpp_phy\build\ninja-vs2019\openisac_phy_iq_pipeline_benchmark.exe "%OPENISAC_IQ_PIPELINE_OUT%\frames.csv" "%OPENISAC_IQ_PIPELINE_FRAMES%" "%OPENISAC_IQ_PIPELINE_CORPUS%" "%OPENISAC_IQ_PIPELINE_SCHEDULING%"
if errorlevel 1 exit /b 1
set "OPENISAC_PLOT_PYTHON=python_phy\.venv\Scripts\python.exe"
if defined OPENISAC_PYTHON set "OPENISAC_PLOT_PYTHON=%OPENISAC_PYTHON%"
"%OPENISAC_PLOT_PYTHON%" cpp_phy\plot_iq_pipeline_benchmark.py "%OPENISAC_IQ_PIPELINE_OUT%\frames.csv" --output-dir "%OPENISAC_IQ_PIPELINE_OUT%"
exit /b %errorlevel%
