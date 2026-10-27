# SETUP.md — Quantum Language Installation Guide

> **Quantum** v1.0.0 — A dynamically-typed, cybersecurity-ready scripting language written in Quantum itself.

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Build from Source](#build-from-source)
- [Adding Quantum to PATH](#adding-quantum-to-path)
- [Verifying Installation](#verifying-installation)
- [Running Your First Script](#running-your-first-script)
- [REPL Mode](#repl-mode)
- [Editor Support](#editor-support)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

| Tool | Notes |
|---|---|
| Windows | x86-64 |
| Git | For cloning the repository |

No compiler, CMake or MSYS2 is needed: Quantum builds itself.

---

## Build from Source

```powershell
git clone https://github.com/SENODROOM/Quantum-Language.git
cd Quantum-Language
build-quantum.bat
```

`seed\qrun.exe`, a prebuilt Quantum compiler checked into the repository, compiles the compiler from `src\`. That new compiler compiles itself once more and then builds the rest of the tools. The binaries land in `build\`:

```
build\qrun.exe          interpreter
build\quantum.exe       compiler + bundler
build\quantum_stub.exe  runtime bundled into produced executables
build\qpm.exe           package manager
```

`build-quantum.bat --verify` also checks that the new `qrun.exe` rebuilds itself byte for byte.

---

## Adding Quantum to PATH

To use `quantum` from any directory, add the binary to your system PATH.

```powershell
$env:PATH += ";C:\path\to\Quantum-Language\build"
```

To make it permanent:

```powershell
[System.Environment]::SetEnvironmentVariable(
    "PATH",
    $env:PATH + ";C:\path\to\Quantum-Language\build",
    [System.EnvironmentVariableTarget]::User
)
```

Alternatively, copy `quantum.exe` and `quantum_stub.exe` to a folder already in your PATH (e.g. a custom `C:\tools\bin`).

#### Using quantum.bat

A helper `quantum.bat` is included in the root directory. You can copy it anywhere in your PATH to invoke Quantum without needing the full path to the binary.

---

## Verifying Installation

After setup, confirm everything works:

```bash
quantum --version
# → Quantum v1.0.0

quantum --help
# → Shows usage information
```

---

## Running Your First Script

1. Create a file called `hello.sa`:

```python
# hello.sa
name = "World"
print("Hello,", name, "from Quantum!")
```

2. Run it:

```bash
quantum hello.sa
# → Hello, World from Quantum!
```

3. Check syntax without executing:

```bash
quantum --check hello.sa
# → Syntax OK (or error details)
```

---

## REPL Mode

Start an interactive session by running `quantum` with no arguments:

```bash
quantum
```

```
Quantum v1.0.0 REPL
Type 'exit' or Ctrl+C to quit.

>>> x = 42
>>> print(x * 2)
84
>>> fn greet(name): return "Hello, " + name
>>> greet("Hacker")
"Hello, Hacker"
>>> exit
```

---

## Editor Support

Editor extensions for syntax highlighting are available in the `extensions/` directory. Currently supported:

| Editor | Status |
|---|---|
| VS Code | ✅ Available in `extensions/vscode/` |

### Installing the VS Code Extension

```bash
# From the repository root
cd extensions/vscode
npm install
npm run compile

# Then install via VS Code Extensions panel → "Install from VSIX"
# Or press F5 inside VS Code to launch a development instance
```

The extension provides:
- Syntax highlighting for `.sa` files
- Keyword recognition for all Quantum language constructs

---

## Troubleshooting

### "seed\qrun.exe is missing"
The seed compiler comes with the repository. Restore it with `git checkout seed/qrun.exe`.

### `quantum` not found after PATH setup
Restart your terminal session.

## Summary

```
git clone https://github.com/SENODROOM/Quantum-Language.git
cd Quantum-Language
build-quantum.bat                 # Build
build\quantum.exe --version       # Verify
build\quantum.exe script.sa       # Run your script
```

---

*For full language reference, see [SYNTAX.md](SYNTAX.md).*