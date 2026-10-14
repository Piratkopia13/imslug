@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\build_web.bat
rem
rem Build the browser demo (WebGPU backend + Emscripten example) and assemble
rem the static site GitHub Pages serves: <build dir>\site\ with index.html +
rem index.js + index.wasm + .nojekyll. Canonical entry point for humans, CI and
rem .opencode/commands - see AGENTS.md ("Commands").
rem
rem   scripts\build_web.bat
rem
rem Requirements:
rem   * emsdk >= 4.0.10 (CI pins 5.0.1): set EMSDK to the emsdk root, install it
rem     at %USERPROFILE%\emsdk, or have emcmake/em++ on PATH (CI: the
rem     emscripten-core/setup-emsdk action). emsdk_env.bat is sourced when found.
rem   * cmake >= 3.21 resolved like scripts\build.bat (IMSLUG_CMAKE, PATH, or
rem     Visual Studio) and ninja (Visual Studio ships both).
rem   * A native build (scripts\build.bat) to have run once: the demo embeds the
rem     fetched CJK subset with the NATIVE imslug_gen (a cross-compiled binary
rem     cannot run during the web build). Without it the demo still builds
rem     (embedded Roboto fonts only) and the CJK scene shows its "font not
rem     embedded" message.
rem
rem Overrides:
rem   IMSLUG_BUILD_DIR=<dir>         web build directory   (default: build-web)
rem   IMSLUG_BUILD_TYPE=<cfg>        single-config config  (default: Release)
rem   IMSLUG_NATIVE_BUILD_DIR=<dir>  native build with imslug_gen.exe (default: build)
rem   IMSLUG_CMAKE=<path>            cmake to use
rem   IMSLUG_RECONFIGURE=1           force a configure pass
rem   IMSLUG_CMAKE_DEFINES="-D..."   extra configure-time options
rem   IMSLUG_SKIP_EMSDK_ENV=1        do not source emsdk_env.bat
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_BUILD_DIR  set "IMSLUG_BUILD_DIR=build-web"
if not defined IMSLUG_BUILD_TYPE set "IMSLUG_BUILD_TYPE=Release"
if not defined IMSLUG_NATIVE_BUILD_DIR set "IMSLUG_NATIVE_BUILD_DIR=build"

rem --- emsdk environment ------------------------------------------------------
rem Flat lines (not a parenthesized block): %EMSDK% must be read after it is
rem set, and cmd expands %VAR% at parse time inside blocks.
if "%IMSLUG_SKIP_EMSDK_ENV%"=="1" goto :emsdk_env_done
if not defined EMSDK if exist "%USERPROFILE%\emsdk\emsdk_env.bat" set "EMSDK=%USERPROFILE%\emsdk"
if not defined EMSDK goto :emsdk_env_done
if not exist "%EMSDK%\emsdk_env.bat" goto :emsdk_env_done
echo [build_web] sourcing "%EMSDK%\emsdk_env.bat"
call "%EMSDK%\emsdk_env.bat" >nul 2>&1
:emsdk_env_done

set "EMCMAKE="
for /f "delims=" %%C in ('where emcmake 2^>nul') do if not defined EMCMAKE set "EMCMAKE=%%C"
if not defined EMCMAKE (
    echo [build_web] ERROR: emcmake not found.>&2
    echo [build_web]   - install emsdk ^(^>= 4.0.10^) and set EMSDK to its root, or let it>&2
    echo [build_web]     live at "%USERPROFILE%\emsdk" ^(emsdk_env.bat is sourced when found^), or>&2
    echo [build_web]   - put emcmake on PATH ^(CI: emscripten-core/setup-emsdk^).>&2
    popd & exit /b 1
)

rem --- resolve cmake: IMSLUG_CMAKE -> PATH -> Visual Studio --------------------
set "CMAKE_EXE="
if defined IMSLUG_CMAKE (
    if exist "%IMSLUG_CMAKE%" ( set "CMAKE_EXE=%IMSLUG_CMAKE%" ) else (
        echo [build_web] ERROR: IMSLUG_CMAKE="%IMSLUG_CMAKE%" does not exist>&2
        popd & exit /b 1
    )
)
if not defined CMAKE_EXE for /f "delims=" %%C in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%C"

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%V"
if not defined CMAKE_EXE if defined VSINSTALL set "CMAKE_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE_EXE (
    echo [build_web] ERROR: cmake not found.>&2
    echo [build_web]   - put cmake on PATH, set IMSLUG_CMAKE, or install the>&2
    echo [build_web]     "C++ CMake tools for Windows" component in Visual Studio.>&2
    popd & exit /b 1
)

set "NINJA_EXE="
for /f "delims=" %%N in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%N"
if not defined NINJA_EXE if defined VSINSTALL set "NINJA_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if defined NINJA_EXE if not exist "%NINJA_EXE%" set "NINJA_EXE="
if not defined NINJA_EXE (
    echo [build_web] ERROR: ninja not found ^(needed by the Emscripten configure/build^).>&2
    echo [build_web]   - put ninja on PATH, or install the Visual Studio CMake component.>&2
    popd & exit /b 1
)

rem --- optional CJK subset embed (native imslug_gen) --------------------------
set "CJK_HEADER="
set "GEN=%IMSLUG_NATIVE_BUILD_DIR%\imslug_gen.exe"
if not exist "%GEN%" set "GEN=%IMSLUG_NATIVE_BUILD_DIR%\%IMSLUG_BUILD_TYPE%\imslug_gen.exe"
set "CJK_FONT=tests\fonts\cjk\NotoSansSC-subset.ttf"
if exist "%GEN%" if exist "%CJK_FONT%" (
    echo [build_web] embedding the CJK subset with "%GEN%"
    if not exist "%IMSLUG_BUILD_DIR%\include" mkdir "%IMSLUG_BUILD_DIR%\include"
    "%GEN%" embed "%CJK_FONT%" --emit-header "%IMSLUG_BUILD_DIR%\include\imslug_example_cjk.h" --header-symbol imslug_example_cjk
    if errorlevel 1 (
        echo [build_web] ERROR: imslug_gen embed failed.>&2
        popd & exit /b 1
    )
    set "CJK_HEADER=%IMSLUG_BUILD_DIR%\include\imslug_example_cjk.h"
) else (
    echo [build_web] CJK subset or native imslug_gen missing - building without embedded CJK
    echo [build_web]   ^(fetch once: scripts\fetch_cjk_font.bat; native tool: scripts\build.bat^)
)

rem --- configure only when needed --------------------------------------------
set "DEFINES=-DIMSLUG_BUILD_EXAMPLES=ON -DIMSLUG_BACKEND_VULKAN=OFF -DIMSLUG_BUILD_TESTS=OFF -DIMSLUG_BUILD_C_EXAMPLE=OFF -DIMSLUG_BUILD_TOOLS=OFF"
if defined CJK_HEADER set "DEFINES=%DEFINES% -DIMSLUG_CJK_EMBED_HEADER=%CJK_HEADER%"
if defined IMSLUG_CMAKE_DEFINES set "DEFINES=%DEFINES% %IMSLUG_CMAKE_DEFINES%"

if not exist "%IMSLUG_BUILD_DIR%\CMakeCache.txt" goto :configure
if /i "%IMSLUG_RECONFIGURE%"=="1" goto :configure
if defined IMSLUG_CMAKE_DEFINES goto :configure
if defined CJK_HEADER goto :configure
goto :build

:configure
echo [build_web] emcmake:    %EMCMAKE%
echo [build_web] cmake:      %CMAKE_EXE%
echo [build_web] build dir:  %IMSLUG_BUILD_DIR%
echo [build_web] defines:    %DEFINES%
call "%EMCMAKE%" "%CMAKE_EXE%" -S . -B "%IMSLUG_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%IMSLUG_BUILD_TYPE% -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" %DEFINES%
if errorlevel 1 (
    echo [build_web] ERROR: configure failed.>&2
    popd & exit /b 1
)

:build
echo [build_web] building imslug_example_web in "%IMSLUG_BUILD_DIR%"
"%CMAKE_EXE%" --build "%IMSLUG_BUILD_DIR%" --target imslug_example_web --parallel
if errorlevel 1 (
    echo [build_web] ERROR: build failed.>&2
    popd & exit /b 1
)

rem --- assemble the site ------------------------------------------------------
set "SITE=%IMSLUG_BUILD_DIR%\site"
if not exist "%SITE%\index.html" (
    echo [build_web] ERROR: "%SITE%\index.html" missing after the build.>&2
    popd & exit /b 1
)
if not exist "%SITE%\index.js" (
    echo [build_web] ERROR: "%SITE%\index.js" missing after the build.>&2
    popd & exit /b 1
)
if not exist "%SITE%\index.wasm" (
    echo [build_web] ERROR: "%SITE%\index.wasm" missing after the build.>&2
    popd & exit /b 1
)
type nul > "%SITE%\.nojekyll"

echo [build_web] site ready: %SITE%
echo [build_web] serve locally with: python -m http.server 8000 --directory "%SITE%"
popd
exit /b 0
