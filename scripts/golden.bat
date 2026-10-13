@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\golden.bat
rem
rem Build and run the M2 golden-image tests: headless offscreen Vulkan
rem rendering of fixed strings at sizes 12/16/32/64 x DPI scales 1.0/1.25/2.0,
rem compared per pixel against the M1 CPU reference rasterizer. Both render
rem paths are exercised: a classic render pass and core Vulkan 1.3 dynamic
rem rendering.
rem
rem Output: PPMs land in <build dir>\golden\ (<case>_gpu.ppm, <case>_cpu.ppm,
rem <case>_diff.ppm on failure). Tests skip (exit 77) when no usable Vulkan
rem device is available.
rem
rem   scripts\golden.bat                 build + run both render-path tests
rem   scripts\golden.bat -R golden_dyn   extra args pass to ctest
rem
rem Overrides: IMSLUG_BUILD_DIR / IMSLUG_BUILD_TYPE (see scripts\build.bat).
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_BUILD_DIR  set "IMSLUG_BUILD_DIR=build"
if not defined IMSLUG_BUILD_TYPE set "IMSLUG_BUILD_TYPE=Release"

echo [golden] build step
call "%~dp0build.bat" --target imslug_tests_golden
if errorlevel 1 (
    echo [golden] ERROR: build failed.>&2
    popd & exit /b 1
)

rem ctest lives next to the resolved cmake.exe (CMake >= 3.20 supports --test-dir).
set "CTEST_EXE=ctest"
if defined IMSLUG_CMAKE_RESOLVED for %%C in ("%IMSLUG_CMAKE_RESOLVED%") do if exist "%%~dpCctest.exe" set "CTEST_EXE=%%~dpCctest.exe"

set "CONFIG_ARG="
findstr /b /c:"CMAKE_GENERATOR:INTERNAL=Visual Studio" "%IMSLUG_BUILD_DIR%\CMakeCache.txt" >nul 2>nul
if not errorlevel 1 set "CONFIG_ARG=-C %IMSLUG_BUILD_TYPE%"

if "%~1"=="" (
    echo [golden] ctest --test-dir "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% -R golden --output-on-failure
    "%CTEST_EXE%" --test-dir "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% -R golden --output-on-failure
) else (
    echo [golden] ctest --test-dir "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% -R golden --output-on-failure %*
    "%CTEST_EXE%" --test-dir "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% -R golden --output-on-failure %*
)
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" echo [golden] FAILED with exit code %RESULT%.>&2
popd
exit /b %RESULT%
