@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\gen_asset.bat
rem
rem Regenerates the committed packed test asset
rem (tests\assets\Roboto-ASCII.imslug) with the imslug_gen CLI, plus the two
rem generated headers the shared example app embeds as its default fonts
rem (example\common\example_app.cpp):
rem   example\common\imslug_example_asset.h  (the packed asset itself)
rem   example\common\imslug_example_font.h   (the raw TTF for the stock
rem                                             ImGui atlas font)
rem All three are generated artifacts: never edit them by hand. The round-trip
rem test (imslug_packed_roundtrip) and the CLI freshness test
rem (imslug_asset_fresh, which also byte-compares both example headers) compare
rem against them and fail when stale, so regenerate + commit the files together
rem with any generator/format/version change.
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

echo [gen_asset] %GEN% font tests\fonts\Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests\assets\Roboto-ASCII.imslug --emit-header example\common\imslug_example_asset.h --header-symbol imslug_example_asset
"%GEN%" font tests\fonts\Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests\assets\Roboto-ASCII.imslug --emit-header example\common\imslug_example_asset.h --header-symbol imslug_example_asset
if errorlevel 1 (
    echo [gen_asset] FAILED: asset + asset header.>&2
    popd & exit /b 1
)

echo [gen_asset] %GEN% embed tests\fonts\Roboto-Regular.ttf --emit-header example\common\imslug_example_font.h --header-symbol imslug_example_font
"%GEN%" embed tests\fonts\Roboto-Regular.ttf --emit-header example\common\imslug_example_font.h --header-symbol imslug_example_font
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" echo [gen_asset] FAILED with exit code %RESULT%.>&2
popd
exit /b %RESULT%
