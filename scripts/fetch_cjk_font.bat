@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\fetch_cjk_font.bat
rem
rem Fetches the pinned Noto Sans SC variable TTF, verifies its SHA-256, and
rem subsets it with fontTools into tests\fonts\cjk\NotoSansSC-subset.ttf (a
rem gitignored test asset; see tests\fonts\cjk\PROVENANCE.txt for the exact
rem provenance and subset command). Suites that need the font skip when it is
rem absent, so this script is optional - CI runs it, local runs may too.
rem
rem   scripts\fetch_cjk_font.bat            fetch + subset when needed
rem
rem Overrides (all optional):
rem   IMSLUG_CJK_URL      source URL (default: pinned google/fonts raw URL)
rem   IMSLUG_CJK_SHA256   expected source SHA-256 (default: pinned)
rem   IMSLUG_CJK_SOURCE   use a local source TTF instead of downloading
rem   IMSLUG_PYTHON       python 3 executable (needs fontTools)
rem
rem Exit codes: 0 = ready (or cannot fetch here: tests skip CJK); 1 = failed.
rem ===========================================================================

for %%I in ("%~dp0..") do set "IMSLUG_ROOT=%%~fI"
pushd "%IMSLUG_ROOT%" || exit /b 1

set "OUT_DIR=tests\fonts\cjk"
set "OUT=%OUT_DIR%\NotoSansSC-subset.ttf"
set "CHARS=%OUT_DIR%\subset_chars.txt"
if not defined IMSLUG_CJK_URL set "IMSLUG_CJK_URL=https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%%5Bwght%%5D.ttf"
if not defined IMSLUG_CJK_SHA256 set "IMSLUG_CJK_SHA256=a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
if not defined IMSLUG_CJK_LICENSE_URL set "IMSLUG_CJK_LICENSE_URL=https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/OFL.txt"

if not exist "%CHARS%" (
    echo [cjk-font] ERROR: missing "%CHARS%" ^(the committed subset character set^).>&2
    goto :fail
)
if exist "%OUT%" (
    echo [cjk-font] "%OUT%" already present - nothing to do.
    goto :done
)

set "PYTHON_EXE="
if defined IMSLUG_PYTHON set "PYTHON_EXE=%IMSLUG_PYTHON%"
if not defined PYTHON_EXE for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON_EXE set "PYTHON_EXE=%%P"
if defined PYTHON_EXE "%PYTHON_EXE%" -c "import fontTools" >nul 2>nul || set "PYTHON_EXE="
if not defined PYTHON_EXE (
    echo [cjk-font] python 3 with fontTools is unavailable ^(pip install fonttools^);
    echo [cjk-font] skipping CJK font fetch - the stress/golden suites will skip CJK.
    goto :done
)

set "SRC=%IMSLUG_CJK_SOURCE%"
if defined SRC goto :have_source
set "CACHE=%OUT_DIR%\.cache"
if not exist "%CACHE%" mkdir "%CACHE%"
set "SRC=%CACHE%\NotoSansSC-vf.ttf"

:have_source
if not exist "%SRC%" (
    echo [cjk-font] downloading "%IMSLUG_CJK_URL%"
    where curl >nul 2>nul
    if errorlevel 1 (
        echo [cjk-font] curl is unavailable; skipping CJK font fetch.
        goto :done
    )
    curl -sSL --fail -o "%SRC%" "%IMSLUG_CJK_URL%" || (
        echo [cjk-font] download failed; skipping CJK font fetch.
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
if /i not "%SRC_HASH%"=="%IMSLUG_CJK_SHA256%" (
    echo [cjk-font] ERROR: source SHA-256 mismatch: got "%SRC_HASH%".>&2
    echo [cjk-font]   expected "%IMSLUG_CJK_SHA256%" - refusing to subset.>&2
    goto :fail
)

echo [cjk-font] subsetting into "%OUT%"
"%PYTHON_EXE%" -m fontTools.subset "%SRC%" --output-file="%OUT%" --text-file="%CHARS%" ^
    --layout-features-=kern,liga,clig,calt --no-hinting --desubroutinize || goto :fail
if not exist "%OUT%" goto :fail
if defined IMSLUG_CJK_LICENSE_URL (
    curl -sSL --fail -o "%OUT_DIR%\OFL.txt" "%IMSLUG_CJK_LICENSE_URL%" 2>nul
)
for %%S in ("%OUT%") do echo [cjk-font] wrote "%OUT%" (%%~zS bytes).

:done
popd
endlocal & exit /b 0

:fail
echo [cjk-font] see tests\fonts\cjk\PROVENANCE.txt for the documented pipeline.>&2
popd
endlocal & exit /b 1
