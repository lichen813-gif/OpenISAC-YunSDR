@echo off
setlocal

set "SNR_DB=%~1"
if not defined SNR_DB set "SNR_DB=40"
set "TIMING_OFFSET=%~2"
if not defined TIMING_OFFSET set "TIMING_OFFSET=20"
set "TDL_TAPS=%~3"
if not defined TDL_TAPS set "TDL_TAPS=0:0:0+3:-4:45+9:-8:-80"
set "CFO_HZ=%~4"
if not defined CFO_HZ set "CFO_HZ=300"
set "SFO_PPM=%~5"
if not defined SFO_PPM set "SFO_PPM=20"
set "RANDOM_SEED=%~6"
if not defined RANDOM_SEED set "RANDOM_SEED=49239"
set "OUTPUT_DIR=%~dp0..\measurement\cpp_windows_plot"

call "%~dp0build_windows_vs2019.cmd"
if errorlevel 1 exit /b 1

"%~dp0build\ninja-vs2019\openisac_phy_diagnostics.exe" "%OUTPUT_DIR%" "%SNR_DB%" "%TIMING_OFFSET%" "%TDL_TAPS%" "%CFO_HZ%" "%SFO_PPM%" "%RANDOM_SEED%"
if errorlevel 1 exit /b 1

set "PYTHON_EXE="
if defined OPENISAC_PYTHON set "PYTHON_EXE=%OPENISAC_PYTHON%"
if not defined PYTHON_EXE if exist "%~dp0..\python_phy\.venv\Scripts\python.exe" set "PYTHON_EXE=%~dp0..\python_phy\.venv\Scripts\python.exe"
if not defined PYTHON_EXE for /f "delims=" %%i in ('where python 2^>nul') do if not defined PYTHON_EXE set "PYTHON_EXE=%%i"

if not defined PYTHON_EXE (
    echo ERROR: Python was not found.
    echo Set OPENISAC_PYTHON to the full path of python.exe and run this script again.
    exit /b 1
)
if not exist "%PYTHON_EXE%" (
    echo ERROR: Python does not exist at "%PYTHON_EXE%".
    exit /b 1
)

set "MPLCONFIGDIR=%OUTPUT_DIR%\matplotlib-cache"
if not exist "%MPLCONFIGDIR%" mkdir "%MPLCONFIGDIR%"
"%PYTHON_EXE%" -c "import matplotlib" >nul 2>nul
if errorlevel 1 (
    echo ERROR: Matplotlib is not installed in "%PYTHON_EXE%".
    echo Install it with: "%PYTHON_EXE%" -m pip install matplotlib
    exit /b 1
)

"%PYTHON_EXE%" "%~dp0plot_cpp_results.py" "%OUTPUT_DIR%"
if errorlevel 1 exit /b 1

echo.
echo C++ calculation and Python plotting completed.
echo Constellation: %OUTPUT_DIR%\constellation.png
echo Time waveform: %OUTPUT_DIR%\time_waveform.png
echo Channel and synchronization: %OUTPUT_DIR%\channel_synchronization.png
exit /b 0
