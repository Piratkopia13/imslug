@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem imslug - scripts\format.bat
rem
rem Run clang-format over our C/C++ sources. Vendored trees (third_party/,
rem vendor/) are never touched - upstream files keep their own style.
rem
rem   scripts\format.bat            format in place
rem   scripts\format.bat --check    report files that would change, exit non-zero
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

set "CHECK=0"
if /i "%~1"=="--check" set "CHECK=1"

set "CLANG_FORMAT="
for /f "delims=" %%C in ('where clang-format 2^>nul') do if not defined CLANG_FORMAT set "CLANG_FORMAT=%%C"
if not defined CLANG_FORMAT if exist "%ProgramFiles%\LLVM\bin\clang-format.exe" set "CLANG_FORMAT=%ProgramFiles%\LLVM\bin\clang-format.exe"

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%V"
if not defined CLANG_FORMAT if defined VSINSTALL if exist "%VSINSTALL%\VC\Tools\Llvm\x64\bin\clang-format.exe" set "CLANG_FORMAT=%VSINSTALL%\VC\Tools\Llvm\x64\bin\clang-format.exe"

if not defined CLANG_FORMAT (
    echo [format] ERROR: clang-format not found - install LLVM or add it to PATH.>&2
    popd & exit /b 1
)

if "%CHECK%"=="1" ( set "CF_ARGS=--dry-run --Werror" ) else ( set "CF_ARGS=-i" )

set "RC=0"
set "COUNT=0"
for /f "delims=" %%F in ('git ls-files -co --exclude-standard -- "*.h" "*.hpp" "*.c" "*.cpp" "*.cc" "*.cxx" ^| findstr /v /i /b /c:"third_party/" /c:"vendor/"') do (
    "%CLANG_FORMAT%" !CF_ARGS! "%%F" || set "RC=1"
    set /a COUNT+=1
)

if "%RC%"=="0" (
    echo [format] OK ^(%COUNT% file^(s^)^).
) else (
    echo [format] FAIL: formatting differences found - run scripts\format.bat to fix.>&2
)
popd
exit /b %RC%
