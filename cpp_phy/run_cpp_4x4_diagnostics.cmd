@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b %errorlevel%

if "%~1"=="" (
    cpp_phy\build\ninja-vs2019\openisac_phy_4x4_diagnostics.exe --frames 10 --snr 45 --modulation 64QAM --pilot-spacing 2 --tx-correlation 0.2 --rx-correlation 0.2 --seed 311383 --check
) else (
    cpp_phy\build\ninja-vs2019\openisac_phy_4x4_diagnostics.exe %*
)
if errorlevel 1 exit /b %errorlevel%
set "OPENISAC_4X4_PYTHON=python_phy\.venv\Scripts\python.exe"
if defined OPENISAC_PYTHON set "OPENISAC_4X4_PYTHON=%OPENISAC_PYTHON%"
if exist "%OPENISAC_4X4_PYTHON%" (
    "%OPENISAC_4X4_PYTHON%" cpp_phy\plot_4x4_diagnostics.py measurement\cpp_4x4_diagnostics
)
exit /b %errorlevel%
