@echo off
setlocal

rem ── quantum.bat ───────────────────────────────────────────────────────────────
rem  Launcher wrapper for quantum.exe (compiler + bundler).
rem  quantum hello.sa   --> compiles hello.sa into hello.exe, then runs it
rem  quantum --run hello.sa  --> interprets directly (no .exe)
rem
rem  Priority: build\ first (where build-quantum.bat puts it), then the root.

set QUANTUM_EXE=

if exist "%~dp0build\quantum.exe" (
    set QUANTUM_EXE=%~dp0build\quantum.exe
    goto :run
)
if exist "%~dp0quantum.exe" (
    set QUANTUM_EXE=%~dp0quantum.exe
    goto :run
)

echo [Error] quantum.exe not found. Run build-quantum.bat first.
exit /b 1

:run
rem Run quantum.exe, passing all arguments through.
rem Both stdout AND stderr go to the console so you see [Compiled], [Running], errors etc.
"%QUANTUM_EXE%" %*
exit /b %errorlevel%