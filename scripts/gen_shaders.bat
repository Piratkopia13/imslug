@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem imslug - scripts\gen_shaders.bat
rem
rem Compile shaders\vulkan\*.{vert,frag,comp} to SPIR-V and update the committed
rem header shaders\imslug_shaders_vulkan.h; embed shaders\wgsl\*.wgsl as
rem NUL-terminated C string arrays in the committed header
rem shaders\imslug_shaders_webgpu.h. Each half is skipped with a message when
rem its source directory has no shader sources; only when both are missing is
rem there nothing to generate.
rem
rem   scripts\gen_shaders.bat           regenerate both headers
rem   scripts\gen_shaders.bat --check   build into temp files and diff against
rem                                     the committed headers (non-zero if stale)
rem
rem Requirements:
rem   * glslangValidator - GLSLANG_VALIDATOR, or %VULKAN_SDK%\Bin, or PATH
rem                        (SPIR-V half only);
rem   * python 3         - PYTHON, or PATH (both halves);
rem Invocation per GLSL source:
rem   glslangValidator -V --target-env vulkan1.2 -o <tmp>\<name>.<stage>.spv <src>
rem WGSL embedding (pure python, no external tool):
rem   python scripts\emit_shader_header.py --mode wgsl --input-dir shaders\wgsl --output <header>
rem Never hand-edit the generated headers (AGENTS.md invariant 1).
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

set "SHADER_DIR=shaders\vulkan"
set "HEADER=shaders\imslug_shaders_vulkan.h"
set "WGSL_DIR=shaders\wgsl"
set "WGSL_HEADER=shaders\imslug_shaders_webgpu.h"
set "CHECK=0"
if /i "%~1"=="--check" set "CHECK=1"
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="--help" goto :usage

rem --- collect shader sources ------------------------------------------------
set "SOURCES="
for /f "delims=" %%F in ('dir /b /a-d "%SHADER_DIR%\*.vert" 2^>nul') do set "SOURCES=!SOURCES! "%SHADER_DIR%\%%F""
for /f "delims=" %%F in ('dir /b /a-d "%SHADER_DIR%\*.frag" 2^>nul') do set "SOURCES=!SOURCES! "%SHADER_DIR%\%%F""
for /f "delims=" %%F in ('dir /b /a-d "%SHADER_DIR%\*.comp" 2^>nul') do set "SOURCES=!SOURCES! "%SHADER_DIR%\%%F""

set "WGSL_COUNT=0"
for /f "delims=" %%F in ('dir /b /a-d "%WGSL_DIR%\*.wgsl" 2^>nul') do set /a WGSL_COUNT+=1

if not defined SOURCES if %WGSL_COUNT%==0 goto :nothing

rem --- python (needed for both headers) --------------------------------------
set "PYTHON_EXE="
if defined PYTHON set "PYTHON_EXE=%PYTHON%"
if not defined PYTHON_EXE for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON_EXE set "PYTHON_EXE=%%P"
if not defined PYTHON_EXE goto :missing_python
"%PYTHON_EXE%" --version >nul 2>nul || goto :missing_python

rem --- temp dir (shared by both halves) --------------------------------------
set "TMPDIR=%TEMP%\imslug-gen-shaders"
if exist "%TMPDIR%" rd /s /q "%TMPDIR%" 2>nul
mkdir "%TMPDIR%" 2>nul
if not exist "%TMPDIR%" goto :fail

rem === SPIR-V half ===========================================================
if not defined SOURCES goto :spirv_skip

set "GLSLANG="
if defined GLSLANG_VALIDATOR if exist "%GLSLANG_VALIDATOR%" set "GLSLANG=%GLSLANG_VALIDATOR%"
if not defined GLSLANG if defined VULKAN_SDK if exist "%VULKAN_SDK%\Bin\glslangValidator.exe" set "GLSLANG=%VULKAN_SDK%\Bin\glslangValidator.exe"
if not defined GLSLANG for /f "delims=" %%G in ('where glslangValidator 2^>nul') do if not defined GLSLANG set "GLSLANG=%%G"
rem glslang 16.x ships the tool as glslang.exe (same CLI for our invocation).
if not defined GLSLANG for /f "delims=" %%G in ('where glslang 2^>nul') do if not defined GLSLANG set "GLSLANG=%%G"
if not defined GLSLANG goto :missing_glslang

set "COUNT=0"
for %%S in (%SOURCES%) do (
    call :compile_one %%S
    if errorlevel 1 goto :compile_failed
)

if "%CHECK%"=="1" (set "OUT_HEADER=%TMPDIR%\imslug_shaders_vulkan.h") else (set "OUT_HEADER=%HEADER%")
"%PYTHON_EXE%" "scripts\emit_shader_header.py" --mode spirv --input-dir "%TMPDIR%" --output "%OUT_HEADER%"
if errorlevel 1 goto :emit_failed

if "%CHECK%"=="1" goto :spirv_check
echo [gen_shaders] wrote "%HEADER%" ^(%COUNT% shader^(s^)^).
goto :wgsl_half

:spirv_check
if not exist "%HEADER%" goto :missing_header
fc /b "%HEADER%" "%OUT_HEADER%" >nul 2>nul
if errorlevel 1 goto :stale
echo [gen_shaders] --check OK: "%HEADER%" is up to date ^(%COUNT% shader^(s^)^).
goto :wgsl_half

:spirv_skip
echo [gen_shaders] no SPIR-V shader sources in "%SHADER_DIR%" - skipping the SPIR-V header.

rem === WGSL half =============================================================
:wgsl_half
if %WGSL_COUNT%==0 goto :wgsl_skip
if "%CHECK%"=="1" (set "WGSL_OUT=%TMPDIR%\imslug_shaders_webgpu.h") else (set "WGSL_OUT=%WGSL_HEADER%")
"%PYTHON_EXE%" "scripts\emit_shader_header.py" --mode wgsl --input-dir "%WGSL_DIR%" --output "%WGSL_OUT%"
if errorlevel 1 goto :emit_failed

if "%CHECK%"=="1" goto :wgsl_check
echo [gen_shaders] wrote "%WGSL_HEADER%" ^(%WGSL_COUNT% file^(s^)^).
goto :cleanup

:wgsl_check
if not exist "%WGSL_HEADER%" goto :wgsl_missing_header
fc /b "%WGSL_HEADER%" "%WGSL_OUT%" >nul 2>nul
if errorlevel 1 goto :wgsl_stale
echo [gen_shaders] --check OK: "%WGSL_HEADER%" is up to date ^(%WGSL_COUNT% file^(s^)^).
goto :cleanup

:wgsl_skip
echo [gen_shaders] no WGSL sources in "%WGSL_DIR%" - skipping the WGSL header.
goto :cleanup

rem --- failures and helpers ---------------------------------------------------
:missing_header
echo [gen_shaders] ERROR: "%HEADER%" is missing but shader sources exist.>&2
echo [gen_shaders]   run scripts\gen_shaders.bat and commit the generated header.>&2
goto :fail

:wgsl_missing_header
echo [gen_shaders] ERROR: "%WGSL_HEADER%" is missing but WGSL sources exist.>&2
echo [gen_shaders]   run scripts\gen_shaders.bat and commit the generated header.>&2
goto :fail

:stale
echo [gen_shaders] ERROR: committed SPIR-V header is stale: "%HEADER%">&2
echo [gen_shaders]   run scripts\gen_shaders.bat, review the diff, and commit the header with its sources.>&2
goto :fail

:wgsl_stale
echo [gen_shaders] ERROR: committed WGSL header is stale: "%WGSL_HEADER%">&2
echo [gen_shaders]   run scripts\gen_shaders.bat, review the diff, and commit the header with its sources.>&2
goto :fail

:missing_glslang
echo [gen_shaders] ERROR: glslangValidator not found.>&2
echo [gen_shaders]   install the Vulkan SDK and set VULKAN_SDK, or add its Bin directory to PATH,>&2
echo [gen_shaders]   or set GLSLANG_VALIDATOR to the executable ^(glslang 16.x: glslang.exe^).>&2
goto :fail

:missing_python
echo [gen_shaders] ERROR: python 3 not found - needed to emit the shader headers.>&2
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
echo   ^(no args^)  regenerate "%HEADER%" and "%WGSL_HEADER%"
echo   --check    build into temp files and diff against the committed headers
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
