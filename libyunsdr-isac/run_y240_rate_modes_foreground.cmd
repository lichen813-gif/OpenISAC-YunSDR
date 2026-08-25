@echo off
setlocal

title Y240 Rate Tests - TX RX TXRX

set "RATE_EXE=%~dp0out\y240-sdk-26-01-00.1\bin\yunsdr_ss_rate.exe"
set "COMMON_ARGS=-a pcies:0.0 -s 15360000 -f 2400000000 -g 5 -G 60 -t 1000000 -T 0 -N 18432 -n 5"

if not exist "%RATE_EXE%" (
    echo ERROR: executable not found:
    echo   %RATE_EXE%
    pause
    exit /b 1
)

echo ============================================================
echo r=0: TX-only, TX channel 0, TX gain 60
echo ============================================================
"%RATE_EXE%" %COMMON_ARGS% -r 0 -c 0x0 -C 0x1
if errorlevel 1 goto :failed

timeout /t 2 /nobreak >nul
echo.
echo ============================================================
echo r=1: RX-only, RX channel 0, RX gain 5 dB
echo ============================================================
"%RATE_EXE%" %COMMON_ARGS% -r 1 -c 0x1 -C 0x0
if errorlevel 1 goto :failed

timeout /t 2 /nobreak >nul
echo.
echo ============================================================
echo r=2: simultaneous TX/RX, channel 0
echo ============================================================
"%RATE_EXE%" %COMMON_ARGS% -r 2 -c 0x1 -C 0x1
if errorlevel 1 goto :failed

echo.
echo All three rate tests completed successfully.
goto :done

:failed
echo.
echo ERROR: rate test failed with exit code %errorlevel%.

:done
echo.
echo Press any key to close this window.
pause >nul
endlocal
