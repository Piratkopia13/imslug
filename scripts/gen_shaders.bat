@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem imslug - scripts\gen_shaders.bat
rem
rem Compile shaders\vulkan\*.{vert,frag,comp} to SPIR-V and update the committed
rem header shaders\imslug_shaders_vulkan.h (M2). The no-sources branch only
rem matters on a stripped tree that removed the shader sources.
rem
rem   scripts\gen_shaders.bat           regenerate the header
rem   scripts\gen_shaders.bat --check   compile into a temp file and diff against
rem                                     the committed header (non-zero if stale)
rem
rem Requirements:
rem   * glslangValidator - GLSLANG_VALIDATOR, or %VULKAN_SDK%\Bin, or PATH;
rem   * python 3         - PYTHON, or PATH;
rem Invocation per source:
rem   glslangValidator -V --target-env vulkan1.2 -o <tmp>\<name>.<stage>.spv <src>
rem Never hand-edit the generated header (AGENTS.md invariant 1).
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

set "SHADER_DIR=shaders\vulkan"
set "HEADER=shaders\imslug_shaders_vulkan.h"
set "CHECK=0"
if /i "%~1"=="--check" set "CHECK=1"
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="--help" goto :usage

rem --- collect shader sources ------------------------------------------------
set "SOURCES="
for /f "delims=" %%F in ('dir /b /a-d "%SHADER_DIR%\*.vert" 2^>nul') do set "SOURCES=!SOURCES! "%SHADER_DIR%\%%F""
for /f "delims=" %%F in ('dir /b /a-d "%SHADER_DIR%\*.frag" 2^>nul') do set "SOURCES=!SOURCES! "%SHADER_DIR%\%%F""
for /f "delims=" %%F in ('dir /b /a-d "%SHADER_DIR%\*.comp" 2^>nul') do set "SOURCES=!SOURCES! "%SHADER_DIR%\%%F""
if not defined SOURCES goto :nothing

rem --- tool detection --------------------------------------------------------
set "GLSLANG="
if defined GLSLANG_VALIDATOR if exist "%GLSLANG_VALIDATOR%" set "GLSLANG=%GLSLANG_VALIDATOR%"
if not defined GLSLANG if defined VULKAN_SDK if exist "%VULKAN_SDK%\Bin\glslangValidator.exe" set "GLSLANG=%VULKAN_SDK%\Bin\glslangValidator.exe"
if not defined GLSLANG for /f "delims=" %%G in ('where glslangValidator 2^>nul') do if not defined GLSLANG set "GLSLANG=%%G"
if not defined GLSLANG goto :missing_glslang

set "PYTHON_EXE="
if defined PYTHON set "PYTHON_EXE=%PYTHON%"
if not defined PYTHON_EXE for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON_EXE set "PYTHON_EXE=%%P"
if not defined PYTHON_EXE goto :missing_python
"%PYTHON_EXE%" --version >nul 2>nul || goto :missing_python

rem --- compile every source into a temp dir ----------------------------------
set "TMPDIR=%TEMP%\imslug-gen-shaders"
if exist "%TMPDIR%" rd /s /q "%TMPDIR%" 2>nul
mkdir "%TMPDIR%" 2>nul
if not exist "%TMPDIR%" goto :fail

set "COUNT=0"
for %%S in (%SOURCES%) do (
    call :compile_one %%S
    if errorlevel 1 goto :compile_failed
)

rem --- emit the header (to a temp file in --check mode) -----------------------
if "%CHECK%"=="1" goto :emit_check_temp
set "OUT_HEADER=%HEADER%"
goto :emit_run

:emit_check_temp
set "OUT_HEADER=%TMPDIR%\imslug_shaders_vulkan.h"

:emit_run
"%PYTHON_EXE%" "scripts\emit_shader_header.py" --input-dir "%TMPDIR%" --output "%OUT_HEADER%"
if errorlevel 1 goto :emit_failed

if not "%CHECK%"=="1" goto :written
if not exist "%HEADER%" goto :missing_header
fc /b "%HEADER%" "%OUT_HEADER%" >nul 2>nul
if errorlevel 1 goto :stale
echo [gen_shaders] --check OK: "%HEADER%" is up to date ^(%COUNT% shader^(s^)^).
goto :cleanup

:written
echo [gen_shaders] wrote "%HEADER%" ^(%COUNT% shader^(s^)^).
goto :cleanup

:missing_header
echo [gen_shaders] ERROR: "%HEADER%" is missing but shader sources exist.>&2
echo [gen_shaders]   run scripts\gen_shaders.bat and commit the generated header.>&2
goto :fail

:stale
echo [gen_shaders] ERROR: committed SPIR-V header is stale: "%HEADER%">&2
echo [gen_shaders]   run scripts\gen_shaders.bat, review the diff, and commit the header with its sources.>&2
goto :fail

:missing_glslang
echo [gen_shaders] ERROR: glslangValidator not found.>&2
echo [gen_shaders]   install the Vulkan SDK and set VULKAN_SDK, or add its Bin directory to PATH,>&2
echo [gen_shaders]   or set GLSLANG_VALIDATOR to the executable.>&2
goto :fail

:missing_python
echo [gen_shaders] ERROR: python 3 not found - needed to emit the SPIR-V header.>&2
echo [gen_shaders]   put python on PATH or set PYTHON to the python 3 executable.>&2
goto :fail

:emit_failed
echo [gen_shaders] ERROR: header emission failed ^(scripts\emit_shader_header.py^).>&2
goto :fail

:compile_failed
echo [gen_shaders] ERROR: glslangValidator failed for a shader source.>&2
goto :fail

:nothing
if "%CHECK%"=="1" (
    echo [gen_shaders] no shader sources found - --check passes trivially.
) else (
    echo [gen_shaders] no shader sources found - nothing to generate.
)
popd
exit /b 0

:cleanup
if exist "%TMPDIR%" rd /s /q "%TMPDIR%" 2>nul
popd
exit /b 0

:fail
if exist "%TMPDIR%" rd /s /q "%TMPDIR%" 2>nul
popd
exit /b 1

:usage
echo Usage: scripts\gen_shaders.bat [--check]
echo   ^(no args^)  regenerate "%HEADER%"
echo   --check    compile into a temp file and diff against the committed header
popd
exit /b 0

:compile_one
set "SRC=%~1"
set "STAGE=%~x1"
set "STAGE=%STAGE:~1%"
set "OUT=%TMPDIR%\%~n1.%STAGE%.spv"
"%GLSLANG%" -V --target-env vulkan1.2 -o "%OUT%" "%SRC%" || exit /b 1
set /a COUNT+=1
exit /b 0
