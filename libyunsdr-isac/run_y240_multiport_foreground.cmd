@echo off
setlocal

title Y240 SISO Multiport TX RX Loopback

set "MULTIPORT_EXE=%~dp0out\y240-sdk-26-01-00.1\bin\yunsdr_ss_txrx_multiport.exe"
set "RESULT_DIR=%~dp0out\hardware-tests\20260825-y240\multiport-siso"

if not exist "%MULTIPORT_EXE%" (
    echo ERROR: executable not found:
    echo   %MULTIPORT_EXE%
    pause
    exit /b 1
)

if not exist "%RESULT_DIR%" mkdir "%RESULT_DIR%"
cd /d "%RESULT_DIR%"

echo ============================================================
echo Y240 SISO multiport wired-loopback test
echo Device:       pcies:0.0
echo Center freq:  2400 MHz
echo Sample rate:  15.36 Msps
echo TX gain:      60
echo RX gain:      5 dB
echo TX/RX mask:   channel 0 ^(0x1^)
echo Tone offset:  1 MHz
echo Frame length: 30720 samples
echo ============================================================
echo.

"%MULTIPORT_EXE%" -a pcies:0.0 -c 0x1 -C 0x1 -s 15360000 -f 2400000000 -g 5 -G 60 -t 1000000 -T 0 -N 30720
set "TEST_EXIT=%errorlevel%"

echo.
if "%TEST_EXIT%"=="0" (
    echo Multiport test completed successfully.
    echo Output directory:
    echo   %RESULT_DIR%
) else (
    echo ERROR: multiport test failed with exit code %TEST_EXIT%.
)

echo.
echo Press any key to close this window.
pause >nul
endlocal
exit /b %TEST_EXIT%
