@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\verify_fetchcontent.bat
rem
rem Clean-project verification: a throwaway CMake project consumes this
rem repository via FetchContent (a git clone of the committed state), builds
rem the GLFW+Vulkan example and a minimal consumer executable, and runs both.
rem
rem   scripts\verify_fetchcontent.bat
rem
rem The FetchContent clone is made from the repository's committed history, so
rem commit your work first if you want the current tree verified.
rem
rem Overrides:
rem   IMSLUG_FC_BUILD_DIR=<dir>  scratch project dir (default: build\fetchcontent-smoke)
rem   IMSLUG_FC_TAG=<git ref>    ref cloned by FetchContent (default: current HEAD commit)
rem   IMSLUG_FC_SCREENSHOT=1     also render one offscreen frame and check the PPM
rem   IMSLUG_BUILD_TYPE=<cfg>    configuration for the throwaway project (default: Release)
rem   IMSLUG_CMAKE=<path>        cmake.exe (same resolution order as build.bat)
rem   IMSLUG_GENERATOR=<name>    force a CMake generator for the throwaway project
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

if not defined IMSLUG_FC_BUILD_DIR set "IMSLUG_FC_BUILD_DIR=build\fetchcontent-smoke"
if not defined IMSLUG_BUILD_TYPE set "IMSLUG_BUILD_TYPE=Release"

rem --- cmake resolution (same order as scripts\build.bat) ---------------------
set "CMAKE_EXE="
if defined IMSLUG_CMAKE (
    if exist "%IMSLUG_CMAKE%" ( set "CMAKE_EXE=%IMSLUG_CMAKE%" ) else (
        echo [fetchcontent] ERROR: IMSLUG_CMAKE="%IMSLUG_CMAKE%" does not exist>&2
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
    echo [fetchcontent] ERROR: cmake not found - put it on PATH, set IMSLUG_CMAKE, or install the VS C++ CMake tools.>&2
    popd & exit /b 1
)

rem --- Visual Studio developer environment (cl.exe for the Ninja path) --------
if defined VSINSTALL (
    set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
    if exist "%VCVARS%" call "%VCVARS%" >nul 2>&1
)

rem --- default ref: the current commit ----------------------------------------
set "FC_TAG=%IMSLUG_FC_TAG%"
if not defined FC_TAG for /f "delims=" %%H in ('git rev-parse HEAD 2^>nul') do set "FC_TAG=%%H"
if not defined FC_TAG (
    echo [fetchcontent] ERROR: cannot resolve HEAD; set IMSLUG_FC_TAG.>&2
    popd & exit /b 1
)

rem --- scratch project ---------------------------------------------------------
set "FC_DIR=%IMSLUG_FC_BUILD_DIR%"
if exist "%FC_DIR%" rmdir /s /q "%FC_DIR%"
mkdir "%FC_DIR%" 2>nul
if not exist "%FC_DIR%" (
    echo [fetchcontent] ERROR: cannot create "%FC_DIR%".>&2
    popd & exit /b 1
)
xcopy /q /y "%~dp0fetchcontent_smoke\CMakeLists.txt" "%FC_DIR%\" >nul
xcopy /q /y "%~dp0fetchcontent_smoke\main.cpp" "%FC_DIR%\" >nul

rem --- generator (Ninja + MSVC like build.bat, else the VS generator) ---------
set "GEN=%IMSLUG_GENERATOR%"
if defined GEN goto :gen_ready
set "NINJA_EXE="
for /f "delims=" %%N in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%N"
if not defined NINJA_EXE if defined VSINSTALL set "NINJA_EXE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if defined NINJA_EXE if not exist "%NINJA_EXE%" set "NINJA_EXE="
if defined NINJA_EXE ( set "GEN=Ninja" ) else ( set "GEN=Visual Studio 17 2022" )
:gen_ready

echo [fetchcontent] source:    "%IMSLUG_ROOT%"
echo [fetchcontent] ref:       %FC_TAG%
echo [fetchcontent] project:   "%FC_DIR%"
echo [fetchcontent] cmake:     "%CMAKE_EXE%"
echo [fetchcontent] generator: "%GEN%"

if /i "%GEN%"=="Ninja" goto :configure_ninja
"%CMAKE_EXE%" -S "%FC_DIR%" -B "%FC_DIR%\build" -G "%GEN%" -DIMSLUG_FC_SOURCE="%IMSLUG_ROOT%" -DIMSLUG_FC_TAG=%FC_TAG%
goto :configure_done

:configure_ninja
if not defined NINJA_EXE set "NINJA_EXE=ninja"
"%CMAKE_EXE%" -S "%FC_DIR%" -B "%FC_DIR%\build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=%IMSLUG_BUILD_TYPE% -DIMSLUG_FC_SOURCE="%IMSLUG_ROOT%" -DIMSLUG_FC_TAG=%FC_TAG%

:configure_done
if errorlevel 1 (
    echo [fetchcontent] ERROR: configure failed.>&2
    popd & exit /b 1
)

set "CONFIG_ARG="
findstr /b /c:"CMAKE_GENERATOR:INTERNAL=Visual Studio" "%FC_DIR%\build\CMakeCache.txt" >nul 2>nul
if not errorlevel 1 set "CONFIG_ARG=--config %IMSLUG_BUILD_TYPE%"

echo [fetchcontent] building fc_smoke + imslug_example_glfw_vulkan ...
"%CMAKE_EXE%" --build "%FC_DIR%\build" --parallel %CONFIG_ARG% --target fc_smoke imslug_example_glfw_vulkan
if errorlevel 1 (
    echo [fetchcontent] ERROR: build failed.>&2
    popd & exit /b 1
)

set "EXE_DIR=%FC_DIR%\build"
if defined CONFIG_ARG set "EXE_DIR=%FC_DIR%\build\%IMSLUG_BUILD_TYPE%"
if not exist "%EXE_DIR%\fc_smoke.exe" (
    echo [fetchcontent] ERROR: fc_smoke.exe not found under "%EXE_DIR%".>&2
    popd & exit /b 1
)

rem The example is a target of the fetched imslug project, so FetchContent
rem places its binary in the dependency's build tree (multi-config generators
rem add the configuration directory).
set "EXAMPLE_EXE=%EXE_DIR%\imslug_example_glfw_vulkan.exe"
if not exist "%EXAMPLE_EXE%" set "EXAMPLE_EXE=%FC_DIR%\build\_deps\imslug-build\imslug_example_glfw_vulkan.exe"
if not exist "%EXAMPLE_EXE%" set "EXAMPLE_EXE=%FC_DIR%\build\_deps\imslug-build\%IMSLUG_BUILD_TYPE%\imslug_example_glfw_vulkan.exe"
if not exist "%EXAMPLE_EXE%" (
    echo [fetchcontent] ERROR: imslug_example_glfw_vulkan.exe not found under "%FC_DIR%\build".>&2
    popd & exit /b 1
)

echo [fetchcontent] running fc_smoke ...
"%EXE_DIR%\fc_smoke.exe"
if errorlevel 1 (
    echo [fetchcontent] ERROR: fc_smoke failed.>&2
    popd & exit /b 1
)

echo [fetchcontent] running imslug_example_glfw_vulkan --help ...
"%EXAMPLE_EXE%" --help
if errorlevel 1 (
    echo [fetchcontent] ERROR: example --help failed.>&2
    popd & exit /b 1
)

if not "%IMSLUG_FC_SCREENSHOT%"=="1" goto :done
echo [fetchcontent] rendering one offscreen frame (scene 1) ...
"%EXAMPLE_EXE%" --screenshot "%FC_DIR%\example.ppm" 1
if errorlevel 1 (
    echo [fetchcontent] ERROR: example --screenshot failed.>&2
    popd & exit /b 1
)
if not exist "%FC_DIR%\example.ppm" (
    echo [fetchcontent] ERROR: screenshot missing.>&2
    popd & exit /b 1
)
echo [fetchcontent] screenshot: "%FC_DIR%\example.ppm"

:done
echo [fetchcontent] OK: clean-project FetchContent build + example run passed.
popd
exit /b 0
