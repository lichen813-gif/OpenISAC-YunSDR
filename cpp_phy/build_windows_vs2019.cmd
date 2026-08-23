@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2019 C++ Desktop Development.
    exit /b 1
)

set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -version [16.0^,17.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo ERROR: Visual Studio 2019 with the C++ toolset was not found.
    exit /b 1
)

set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJA_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "BUILD_DIR=%~dp0build\ninja-vs2019"
set "OPENISAC_CLEAN_OPTION="
if /I "%~1"=="clean" set "OPENISAC_CLEAN_OPTION=--clean-first"

if not exist "%CMAKE_EXE%" (
    echo ERROR: Visual Studio bundled CMake was not found.
    exit /b 1
)
if not exist "%NINJA_EXE%" (
    echo ERROR: Visual Studio bundled Ninja was not found.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%"
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" --build "%BUILD_DIR%" %OPENISAC_CLEAN_OPTION%
if errorlevel 1 exit /b 1

"%CTEST_EXE%" --test-dir "%BUILD_DIR%" --output-on-failure
if errorlevel 1 exit /b 1

echo.
echo Build and tests passed. Running the detector/demapper benchmark...
"%BUILD_DIR%\openisac_phy_bench.exe"
exit /b %ERRORLEVEL%
