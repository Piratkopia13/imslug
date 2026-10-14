@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\gen_asset.bat
rem
rem Regenerates the committed packed test asset
rem (tests\assets\Roboto-ASCII.imslug) with the imslug_gen CLI. The asset is a
rem generated artifact: never edit it by hand. The round-trip test
rem (imslug_packed_roundtrip) and the CLI freshness test (imslug_asset_fresh)
rem compare against it and fail when it is stale, so regenerate + commit the
rem file together with any generator/format/version change.
rem
rem Overrides (same as scripts\build.bat): IMSLUG_BUILD_DIR, IMSLUG_BUILD_TYPE,
rem IMSLUG_CMAKE, IMSLUG_RECONFIGURE, IMSLUG_CMAKE_DEFINES.
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_BUILD_DIR  set "IMSLUG_BUILD_DIR=build"
if not defined IMSLUG_BUILD_TYPE set "IMSLUG_BUILD_TYPE=Release"

echo [gen_asset] build step (imslug_gen)
call "%~dp0build.bat" --target imslug_gen
if errorlevel 1 (
    echo [gen_asset] ERROR: build failed.>&2
    popd & exit /b 1
)

set "GEN=%IMSLUG_BUILD_DIR%\imslug_gen.exe"
if not exist "%GEN%" set "GEN=%IMSLUG_BUILD_DIR%\%IMSLUG_BUILD_TYPE%\imslug_gen.exe"
if not exist "%GEN%" (
    echo [gen_asset] ERROR: imslug_gen not found in "%IMSLUG_BUILD_DIR%".>&2
    popd & exit /b 1
)

echo [gen_asset] %GEN% font tests\fonts\Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests\assets\Roboto-ASCII.imslug
"%GEN%" font tests\fonts\Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests\assets\Roboto-ASCII.imslug
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" echo [gen_asset] FAILED with exit code %RESULT%.>&2
popd
exit /b %RESULT%
