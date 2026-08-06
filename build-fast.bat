@echo off
setlocal EnableDelayedExpansion

echo.
echo   Building Quantum Language v2.0.0  ^|  Incremental Build
echo   Only changed files will be recompiled
echo.

rem ── Add MSYS2 ucrt64 to PATH for this session ─────────────────────────────────
set "PATH=C:\msys64\ucrt64\bin;C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\MinGW\bin;C:\MinGW64\bin;C:\TDM-GCC-64\bin;%PATH%"

rem ── Locate make ───────────────────────────────────────────────────────────────
set "MAKE_EXE="
where mingw32-make >nul 2>&1 && set "MAKE_EXE=mingw32-make" && goto :make_found
where make         >nul 2>&1 && set "MAKE_EXE=make"         && goto :make_found

echo.
echo   [ERROR] mingw32-make / make not found after searching common paths.
echo.
echo   Please run this in PowerShell (no admin needed) then reopen terminal:
echo   [System.Environment]::SetEnvironmentVariable("PATH","C:\msys64\ucrt64\bin;$env:PATH","User")
echo.
pause
exit /b 1

:make_found
echo   Using: !MAKE_EXE!

rem ── Locate cmake ──────────────────────────────────────────────────────────────
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1 || (
    if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
)

rem ── If build directory doesn't exist, run full configure first ────────────────
if not exist build (
    echo   No build directory found — running first-time configure...
    mkdir build
    cd build
    "%CMAKE_EXE%" .. -DCMAKE_BUILD_TYPE=Release -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="!MAKE_EXE!" > ..\build_log.txt 2>&1
    if errorlevel 1 (
        cd ..
        echo   [ERROR] CMake configure failed:
        type build_log.txt
        pause
        exit /b 1
    )
    cd ..
    echo   Configure done.
) else (
    echo   Existing build directory found — skipping configure.
)

rem ── Incremental build (make only recompiles changed files) ────────────────────
echo   Compiling changed files...
cd build
"!MAKE_EXE!" 2> ..\build_errors.txt
if errorlevel 1 (
    cd ..
    echo.
    echo   [ERROR] Compile failed:
    type build_errors.txt
    pause
    exit /b 1
)
cd ..

rem ── Copy all FOUR binaries to the project root ────────────────────────────────
copy /Y build\quantum.exe      quantum.exe      >nul
copy /Y build\qrun.exe         qrun.exe         >nul
copy /Y build\quantum_stub.exe quantum_stub.exe >nul
copy /Y build\qpm.exe          qpm.exe          >nul

echo.
echo   Incremental build successful
echo.
echo   Binaries copied to project root:
echo     quantum.exe       ^<-- compiler + bundler
echo     qrun.exe          ^<-- direct interpreter
echo     quantum_stub.exe  ^<-- standalone runtime  ^(template for hello.exe etc.^)
echo     qpm.exe           ^<-- standalone npm-compatible package manager
echo.
echo   Usage:
echo     quantum hello.sa        ^<-- compiles hello.sa into hello.exe, then runs it
echo     hello.exe               ^<-- run the compiled program directly
echo     qrun    hello.sa        ^<-- interprets hello.sa in-place, no .exe created
echo.
echo   Other flags:
echo     quantum --debug hello.sa    ^<-- dump bytecode then run
echo     quantum --dis   hello.sa    ^<-- dump bytecode only
echo     quantum --check hello.sa    ^<-- parse + type-check only
echo     quantum --test  examples    ^<-- batch test all .sa files
echo.
echo   Tip: Run build.bat for a full clean rebuild if you hit stale-binary issues.
echo.
endlocal