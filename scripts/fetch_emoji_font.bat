@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\fetch_emoji_font.bat
rem
rem Fetches the pinned Noto Color Emoji TTF (COLR v1) and verifies its SHA-256
rem into tests\fonts\emoji\NotoColorEmoji-Regular.ttf (a gitignored test asset;
rem see tests\fonts\emoji\PROVENANCE.txt for the exact provenance). Suites that
rem need the font skip when it is absent, so this script is optional - CI runs
rem it, local runs may too.
rem
rem   scripts\fetch_emoji_font.bat          fetch when needed
rem
rem Overrides (all optional):
rem   IMSLUG_EMOJI_URL      source URL (default: pinned google/fonts raw URL)
rem   IMSLUG_EMOJI_SHA256   expected source SHA-256 (default: pinned)
rem   IMSLUG_EMOJI_SOURCE   use a local source TTF instead of downloading
rem
rem Exit codes: 0 = ready (or cannot fetch here: tests skip color fonts);
rem             1 = failed.
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

set "OUT_DIR=tests\fonts\emoji"
set "OUT=%OUT_DIR%\NotoColorEmoji-Regular.ttf"
if not defined IMSLUG_EMOJI_URL set "IMSLUG_EMOJI_URL=https://raw.githubusercontent.com/google/fonts/main/ofl/notocoloremoji/NotoColorEmoji-Regular.ttf"
if not defined IMSLUG_EMOJI_SHA256 set "IMSLUG_EMOJI_SHA256=4d82a18d8d95f60ba883ce242bbadbf84a576e987745dd7ba38d71c67cff2d73"
if not defined IMSLUG_EMOJI_LICENSE_URL set "IMSLUG_EMOJI_LICENSE_URL=https://raw.githubusercontent.com/google/fonts/main/ofl/notocoloremoji/OFL.txt"

if exist "%OUT%" (
    echo [emoji-font] "%OUT%" already present - nothing to do.
    goto :done
)

set "SRC=%IMSLUG_EMOJI_SOURCE%"
if defined SRC goto :have_source
set "CACHE=%OUT_DIR%\.cache"
if not exist "%CACHE%" mkdir "%CACHE%"
set "SRC=%CACHE%\NotoColorEmoji-Regular.ttf"

:have_source
if not exist "%SRC%" (
    echo [emoji-font] downloading "%IMSLUG_EMOJI_URL%"
    where curl >nul 2>nul
    if errorlevel 1 (
        echo [emoji-font] curl is unavailable; skipping emoji font fetch.
        goto :done
    )
    curl -sSL --fail -o "%SRC%" "%IMSLUG_EMOJI_URL%" || (
        echo [emoji-font] download failed; skipping emoji font fetch.
        del /q "%SRC%" 2>nul
        goto :done
    )
)

for /f "skip=1 tokens=*" %%H in ('certutil -hashfile "%SRC%" SHA256 ^| findstr /r /v "^CertUtil"') do (
    set "SRC_HASH=%%H"
    goto :hash_done
)
:hash_done
set "SRC_HASH=%SRC_HASH: =%"
if /i not "%SRC_HASH%"=="%IMSLUG_EMOJI_SHA256%" (
    echo [emoji-font] ERROR: source SHA-256 mismatch: got "%SRC_HASH%".>&2
    echo [emoji-font]   expected "%IMSLUG_EMOJI_SHA256%" - refusing to copy.>&2
    goto :fail
)

copy /y "%SRC%" "%OUT%" >nul || goto :fail
if defined IMSLUG_EMOJI_LICENSE_URL (
    curl -sSL --fail -o "%OUT_DIR%\OFL.txt" "%IMSLUG_EMOJI_LICENSE_URL%" 2>nul
)
for %%S in ("%OUT%") do echo [emoji-font] wrote "%OUT%" (%%~zS bytes).

:done
popd
endlocal & exit /b 0

:fail
echo [emoji-font] see tests\fonts\emoji\PROVENANCE.txt for the documented pipeline.>&2
popd
endlocal & exit /b 1
