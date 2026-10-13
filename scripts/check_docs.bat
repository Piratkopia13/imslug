@echo off
setlocal EnableExtensions
rem ===========================================================================
rem imslug - scripts\check_docs.bat
rem
rem Docs-hygiene check (PLAN §12.5): see scripts\check_docs.ps1 for the rules.
rem Thin wrapper so cmd/PowerShell/CI all have one entry point.
rem ===========================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_docs.ps1"
exit /b %errorlevel%
