@echo off
setlocal

set "PROJECT_DIR=%~dp0"
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

set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJA_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "BUILD_VARIANT=%~1"
if not defined BUILD_VARIANT set "BUILD_VARIANT=ninja-vs2019"
set "BUILD_DIR=%PROJECT_DIR%build\%BUILD_VARIANT%"
set "BUILD_TEMP=%BUILD_DIR%\tmp"

if not exist "%BUILD_TEMP%" mkdir "%BUILD_TEMP%"
set "TEMP=%BUILD_TEMP%"
set "TMP=%BUILD_TEMP%"
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" -S "%PROJECT_DIR%." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" -DLIBYUNSDR_ISAC_ENABLE_HARDWARE=ON
if errorlevel 1 exit /b %errorlevel%

"%CMAKE_EXE%" --build "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

"%CTEST_EXE%" --test-dir "%BUILD_DIR%" --output-on-failure
exit /b %errorlevel%
