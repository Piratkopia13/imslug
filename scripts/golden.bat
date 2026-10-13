@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\golden.bat
rem
rem Golden-image tests (offscreen Vulkan rendering, compared against the CPU
rem reference). Not available until M2 (standalone Vulkan renderer).
rem
rem This is a non-fatal stub in M0: it exits 0 with a notice so CI and
rem /golden keep working. Reproduce/update goldens with the same script once
rem M2 lands; never eyeball-approve a golden diff.
rem ===========================================================================
echo [golden] golden-image tests are available from M2 - nothing to do.
exit /b 0
