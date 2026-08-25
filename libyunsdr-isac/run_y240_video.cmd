@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 VIDEO_FILE [siso^|mimo2^|stbc] [qpsk^|16qam^|64qam^|256qam] [fdm^|dmrs] [FREQUENCY_MHZ] [TX_GAIN] [RX_GAIN] [VIDEO_KBIT_S]
    echo Example: %~nx0 "C:\path\to\video.mp4" siso 64qam fdm 1500 60 20 1000
    exit /b 2
)

set "MODE=%~2"
if not defined MODE set "MODE=siso"
set "MODULATION=%~3"
if not defined MODULATION set "MODULATION=64qam"
set "PILOT=%~4"
if not defined PILOT set "PILOT=fdm"
set "FREQUENCY=%~5"
if not defined FREQUENCY set "FREQUENCY=1500"
set "TX_GAIN=%~6"
if not defined TX_GAIN set "TX_GAIN=60"
set "RX_GAIN=%~7"
if not defined RX_GAIN set "RX_GAIN=20"
set "BITRATE=%~8"
if not defined BITRATE set "BITRATE=1000"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_y240_video.ps1" -VideoFile "%~1" -Mode "%MODE%" -Modulation "%MODULATION%" -Pilot "%PILOT%" -FrequencyMHz "%FREQUENCY%" -TxGain "%TX_GAIN%" -RxGain "%RX_GAIN%" -VideoBitrateKbps "%BITRATE%"
exit /b %ERRORLEVEL%
