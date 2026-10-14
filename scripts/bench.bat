@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem imslug - scripts\bench.bat
rem
rem Benchmark harness (M4): builds imslug_tests_bench and runs it.
rem
rem   scripts\bench.bat              CPU pre-pass + GPU (headless) with a report
rem   scripts\bench.bat --cpu        CPU-side numbers only (no Vulkan device)
rem   scripts\bench.bat --gpu        GPU numbers only
rem
rem The markdown report is written to <build dir>\bench\bench_report.md; the
rem numbers are machine-specific (record the device name from the report when
rem quoting them). Budgets are not enforced (informational).
rem
rem Overrides: IMSLUG_BUILD_DIR (default: build), IMSLUG_BUILD_TYPE (default:
rem Release), IMSLUG_BENCH_ARGS (extra arguments), IMSLUG_CMAKE (cmake path).
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_BUILD_DIR set "IMSLUG_BUILD_DIR=build"
if not defined IMSLUG_REPORT_DIR set "IMSLUG_REPORT_DIR=%IMSLUG_BUILD_DIR%\bench"

rem --- build (reuses scripts\build.bat so the toolchain matches test runs) ---
call "%~dp0build.bat" --target imslug_tests_bench || (popd & exit /b 1)

set "BENCH=%IMSLUG_BUILD_DIR%\tests\imslug_tests_bench.exe"
if not exist "%BENCH%" (
    echo [bench] ERROR: "%BENCH%" not found after the build.>&2
    popd & exit /b 1
)

if not exist "%IMSLUG_REPORT_DIR%" mkdir "%IMSLUG_REPORT_DIR%"
set "REPORT=%IMSLUG_REPORT_DIR%\bench_report.md"

set "ARGS=%*"
if not defined ARGS set "ARGS=--all"
if defined IMSLUG_BENCH_ARGS set "ARGS=%ARGS% %IMSLUG_BENCH_ARGS%"

echo [bench] running "%BENCH%" %ARGS%
"%BENCH%" %ARGS% --report "%REPORT%"
set "RESULT=%errorlevel%"
echo [bench] report: "%REPORT%"
popd
endlocal & exit /b %RESULT%
