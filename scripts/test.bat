@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\test.bat
rem
rem Build (via scripts\build.bat) and run the ctest suite.
rem Extra arguments pass through to ctest:
rem
rem   scripts\test.bat                  run everything
rem   scripts\test.bat -R smoke         run tests matching a regex
rem   scripts\test.bat --output-on-failure -V
rem
rem Overrides: IMSLUG_BUILD_DIR / IMSLUG_BUILD_TYPE (see scripts\build.bat).
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_BUILD_DIR  set "IMSLUG_BUILD_DIR=build"
if not defined IMSLUG_BUILD_TYPE set "IMSLUG_BUILD_TYPE=Release"

echo [test] build step
call "%~dp0build.bat"
if errorlevel 1 (
    echo [test] ERROR: build failed; not running ctest.>&2
    popd & exit /b 1
)

rem ctest lives next to the resolved cmake.exe (CMake >= 3.20 supports --test-dir).
set "CTEST_EXE=ctest"
if defined IMSLUG_CMAKE_RESOLVED for %%C in ("%IMSLUG_CMAKE_RESOLVED%") do if exist "%%~dpCctest.exe" set "CTEST_EXE=%%~dpCctest.exe"

set "CONFIG_ARG="
findstr /b /c:"CMAKE_GENERATOR:INTERNAL=Visual Studio" "%IMSLUG_BUILD_DIR%\CMakeCache.txt" >nul 2>nul
if not errorlevel 1 set "CONFIG_ARG=-C %IMSLUG_BUILD_TYPE%"

echo [test] ctest --test-dir "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% --output-on-failure %*
"%CTEST_EXE%" --test-dir "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% --output-on-failure %*
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" echo [test] FAILED with exit code %RESULT%.>&2
popd
exit /b %RESULT%
