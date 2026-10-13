@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\smoke_imgui_master.bat
rem
rem ImGui master smoke job (PLAN 8 / M3 brief 6, risk R1: internal-API drift).
rem Configures a separate build directory against a *fetched Dear ImGui master*
rem instead of the pinned release tag and runs the full test suite. The suite is
rem exactly the code that touches imgui_internal.h (the custom ImFontLoader, the
rem baked glyph contract, the pre-pass), so a red run means the internal API
rem moved.
rem
rem   scripts\smoke_imgui_master.bat               master, build dir build-master
rem   set IMSLUG_SMOKE_IMGUI_TAG=v1.93.0   scripts\smoke_imgui_master.bat
rem
rem Overrides: IMSLUG_SMOKE_IMGUI_TAG (default: master)
rem            IMSLUG_SMOKE_BUILD_DIR (default: build-master)
rem            IMSLUG_BUILD_TYPE      (default: Release)
rem
rem Runs on demand (and daily in CI). Not part of scripts\test.bat: it needs
rem network (FetchContent) and a fresh configure.
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_SMOKE_IMGUI_TAG set "IMSLUG_SMOKE_IMGUI_TAG=master"
if not defined IMSLUG_SMOKE_BUILD_DIR set "IMSLUG_SMOKE_BUILD_DIR=build-master"

echo [smoke] Dear ImGui "%IMSLUG_SMOKE_IMGUI_TAG%" -^> "%IMSLUG_SMOKE_BUILD_DIR%" (drift check, not a release gate)
set "IMSLUG_RECONFIGURE=1"
set "IMSLUG_BUILD_DIR=%IMSLUG_SMOKE_BUILD_DIR%"
set "IMSLUG_CMAKE_DEFINES=-DIMSLUG_IMGUI_TAG=%IMSLUG_SMOKE_IMGUI_TAG%"
call "%~dp0test.bat" %*
set "RESULT=%errorlevel%"
popd
endlocal & exit /b %RESULT%
