@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\bench.bat
rem
rem Benchmark harness (CPU pre-pass, draw calls, GPU time). Not available until
rem M4 (robustness/perf milestone).
rem
rem This is a non-fatal stub in M0: it exits 0 with a notice so CI and /bench
rem keep working. Benchmarks must be run with a fixed build type and reported
rem with machine details once M4 lands.
rem ===========================================================================
echo [bench] benchmarks are available from M4 - nothing to do.
exit /b 0
