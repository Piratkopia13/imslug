@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\build.bat
rem
rem Configure (only if needed) and build. Canonical entry point for humans,
rem CI and .opencode/commands - see AGENTS.md ("Commands").
rem
rem   scripts\build.bat                    build the default targets
rem   scripts\build.bat --target imslug    any extra arg goes to cmake --build
rem
rem Overrides:
rem   IMSLUG_BUILD_DIR=<dir>    build directory        (default: build)
rem   IMSLUG_BUILD_TYPE=<cfg>   single-config config   (default: Release)
rem   IMSLUG_RECONFIGURE=1      force a configure pass
rem   IMSLUG_CMAKE=<path>       cmake.exe to use
rem   IMSLUG_GENERATOR=<name>   force a CMake generator (fresh configure only)
rem
rem Exports IMSLUG_CMAKE_RESOLVED (used by scripts\test.bat to find ctest).
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_BUILD_DIR  set "IMSLUG_BUILD_DIR=build"
if not defined IMSLUG_BUILD_TYPE set "IMSLUG_BUILD_TYPE=Release"

rem --- Resolve cmake: IMSLUG_CMAKE -> PATH -> Visual Studio --------------------
set "CMAKE_EXE="
if defined IMSLUG_CMAKE (
    if exist "%IMSLUG_CMAKE%" ( set "CMAKE_EXE=%IMSLUG_CMAKE%" ) else (
        echo [build] ERROR: IMSLUG_CMAKE="%IMSLUG_CMAKE%" does not exist>&2
        popd & exit /b 1
    )
)
if not defined CMAKE_EXE for /f "delims=" %%C in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%C"

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%V"

set "VS_CMAKE="
if defined VSINSTALL set "VS_CMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if defined VS_CMAKE if not exist "%VS_CMAKE%" set "VS_CMAKE="
if not defined CMAKE_EXE if defined VS_CMAKE set "CMAKE_EXE=%VS_CMAKE%"

if not defined CMAKE_EXE (
    echo [build] ERROR: cmake not found.>&2
    echo [build]   - put cmake on PATH, or>&2
    echo [build]   - set IMSLUG_CMAKE=C:\path\to\cmake.exe, or>&2
    echo [build]   - install the "C++ CMake tools for Windows" component in Visual Studio.>&2
    popd & exit /b 1
)

rem --- Visual Studio developer environment (cl.exe, headers, libs) ------------
set "VCVARS="
if defined VSINSTALL set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS if not exist "%VCVARS%" set "VCVARS="
if defined VCVARS call "%VCVARS%" >nul 2>&1

rem --- Configure only when there is no cache (or when asked) -------------------
if not exist "%IMSLUG_BUILD_DIR%\CMakeCache.txt" goto :configure
if /i "%IMSLUG_RECONFIGURE%"=="1" goto :configure
goto :build

:configure
set "GEN=%IMSLUG_GENERATOR%"
if defined GEN goto :configure_run
set "NINJA_EXE="
for /f "delims=" %%N in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%N"
if not defined NINJA_EXE if defined VSINSTALL set "NINJA_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if defined NINJA_EXE if not exist "%NINJA_EXE%" set "NINJA_EXE="
if defined NINJA_EXE ( set "GEN=Ninja" ) else ( set "GEN=Visual Studio 17 2022" )

:configure_run
echo [build] root:       "%IMSLUG_ROOT%"
echo [build] cmake:      "%CMAKE_EXE%"
echo [build] build dir:  "%IMSLUG_BUILD_DIR%"
echo [build] generator:  "%GEN%"
if /i "%GEN%"=="Ninja" goto :configure_ninja
echo [build] configuring ...
"%CMAKE_EXE%" -S . -B "%IMSLUG_BUILD_DIR%" -G "%GEN%"
goto :configure_done

:configure_ninja
if not defined NINJA_EXE set "NINJA_EXE=ninja"
echo [build] configuring with Ninja + MSVC ...
"%CMAKE_EXE%" -S . -B "%IMSLUG_BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=%IMSLUG_BUILD_TYPE%

:configure_done
if errorlevel 1 (
    echo [build] ERROR: configure failed.>&2
    popd & exit /b 1
)

:build
set "CONFIG_ARG="
findstr /b /c:"CMAKE_GENERATOR:INTERNAL=Visual Studio" "%IMSLUG_BUILD_DIR%\CMakeCache.txt" >nul 2>nul
if not errorlevel 1 set "CONFIG_ARG=--config %IMSLUG_BUILD_TYPE%"

echo [build] building "%IMSLUG_BUILD_DIR%" %CONFIG_ARG% %*
"%CMAKE_EXE%" --build "%IMSLUG_BUILD_DIR%" --parallel %CONFIG_ARG% %*
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" echo [build] FAILED with exit code %RESULT%.>&2
popd
endlocal & set "IMSLUG_CMAKE_RESOLVED=%CMAKE_EXE%" & exit /b %RESULT%
