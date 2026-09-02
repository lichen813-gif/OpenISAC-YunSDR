@echo off
setlocal

set "PROJECT_DIR=%~dp0"
set "DEFAULT_SOURCE=%PROJECT_DIR%..\import\vendor-y240-26-01-00.1\source"
set "SOURCE_DIR=%~1"
if "%SOURCE_DIR%"=="" set "SOURCE_DIR=%DEFAULT_SOURCE%"
set "LIBUSB_DIR=%PROJECT_DIR%..\import\libusb-1.0.23"
set "BUILD_DIR=%PROJECT_DIR%build\vendor-y240-26-01-00.1-pcies-usb-vs2019"
set "BUILD_TEMP=%BUILD_DIR%\tmp"
set "OUTPUT_DIR=%PROJECT_DIR%out\y240-sdk-26-01-00.1"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    exit /b 2
)

set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -version [16.0^,17.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo ERROR: Visual Studio 2019 C++ toolset was not found.
    exit /b 2
)
set "CMAKE_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CMAKE_BIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%CMAKE_BIN%;%PATH%"

if exist "%SOURCE_DIR%\CMakeLists.txt" goto source_found
if /I not "%SOURCE_DIR%"=="%DEFAULT_SOURCE%" goto source_missing
echo YunSDR source is not staged; checking the ignored import directory...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_DIR%prepare_vendor_y240_sdk.ps1"
if errorlevel 1 exit /b 3
if exist "%SOURCE_DIR%\CMakeLists.txt" goto source_found

:source_missing
echo ERROR: libyunsdr source was not found:
echo %SOURCE_DIR%
echo.
echo The vendor SDK is intentionally not stored in public Git.
echo See README_zh.md and docs\Y240_VENDOR_HARDWARE_TEST.md.
exit /b 3

:source_found
if exist "%LIBUSB_DIR%\MS64\dll\libusb-1.0.lib" goto libusb_found
echo MSVC x64 libusb was not found:
echo %LIBUSB_DIR%
echo Obtain the libusb 1.0.23 Windows binary package and extract it so that
echo import\libusb-1.0.23\MS64\dll\libusb-1.0.lib exists.
exit /b 4

:libusb_found

if not exist "%BUILD_TEMP%" mkdir "%BUILD_TEMP%"
set "TEMP=%BUILD_TEMP%"
set "TMP=%BUILD_TEMP%"

"%CMAKE_EXE%" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 16 2019" -A x64 ^
    -DENABLE_PCIE=OFF ^
    -DENABLE_PCIEX=ON ^
    -DENABLE_PCIES=ON ^
    -DENABLE_USB3=ON ^
    -DLIBUSB_PATH="%LIBUSB_DIR%" ^
    -DENABLE_SFP=OFF ^
    -DENABLE_FIRMWARE=ON ^
    -DENABLE_EXAMPLE=OFF ^
    -DENABLE_PYTHON=OFF ^
    -DENABLE_COMLABS=OFF ^
    -DENABLE_NUMA=OFF
if errorlevel 1 exit /b %errorlevel%

findstr /R /C:"^LIBPCIES_LIBRARIES:FILEPATH=.*libpcies.lib$" "%BUILD_DIR%\CMakeCache.txt" >nul
if not errorlevel 1 goto libpcies_found
echo LIBPCIES was not enabled. Refusing to create a PCIES build.
exit /b 4

:libpcies_found
findstr /R /C:"^LIBFIRMWARE_LIBRARIES:FILEPATH=.*libfirmware.lib$" "%BUILD_DIR%\CMakeCache.txt" >nul
if not errorlevel 1 goto libfirmware_found
echo LIBFIRMWARE was not enabled. Refusing to create a PCIES build.
exit /b 5

:libfirmware_found
findstr /R /C:"^usb_LIBRARY:FILEPATH=.*libusb-1.0.lib$" "%BUILD_DIR%\CMakeCache.txt" >nul
if not errorlevel 1 goto libusb_cache_found
echo libusb was not enabled. Refusing to create the compatible build.
exit /b 6

:libusb_cache_found

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target yunsdr_ss yunsdr_ss_static yunsdr_ss_txrx_multiport yunsdr_ss_rate -- /m
if errorlevel 1 exit /b %errorlevel%

copy /Y "%BUILD_DIR%\lib\Release\libyunsdr_ss.dll" "%BUILD_DIR%\tests\Release\libyunsdr_ss.dll" >nul
if not exist "%OUTPUT_DIR%\bin" mkdir "%OUTPUT_DIR%\bin"
if not exist "%OUTPUT_DIR%\lib" mkdir "%OUTPUT_DIR%\lib"
if not exist "%OUTPUT_DIR%\include" mkdir "%OUTPUT_DIR%\include"
copy /Y "%BUILD_DIR%\lib\Release\libyunsdr_ss.dll" "%OUTPUT_DIR%\bin\libyunsdr_ss.dll" >nul
copy /Y "%LIBUSB_DIR%\MS64\dll\libusb-1.0.dll" "%OUTPUT_DIR%\bin\libusb-1.0.dll" >nul
copy /Y "%BUILD_DIR%\tests\Release\yunsdr_ss_txrx_multiport.exe" "%OUTPUT_DIR%\bin\yunsdr_ss_txrx_multiport.exe" >nul
copy /Y "%BUILD_DIR%\tests\Release\yunsdr_ss_rate.exe" "%OUTPUT_DIR%\bin\yunsdr_ss_rate.exe" >nul
copy /Y "%BUILD_DIR%\lib\Release\yunsdr_ss.lib" "%OUTPUT_DIR%\lib\yunsdr_ss.lib" >nul
copy /Y "%BUILD_DIR%\lib\Release\libyunsdr_ss.lib" "%OUTPUT_DIR%\lib\libyunsdr_ss.lib" >nul
copy /Y "%SOURCE_DIR%\lib\libpcies.lib" "%OUTPUT_DIR%\lib\libpcies.lib" >nul
copy /Y "%SOURCE_DIR%\lib\libfirmware.lib" "%OUTPUT_DIR%\lib\libfirmware.lib" >nul
copy /Y "%SOURCE_DIR%\src\yunsdr_ss\include\yunsdr_api_ss.h" "%OUTPUT_DIR%\include\yunsdr_api_ss.h" >nul
exit /b %errorlevel%
