# Updating the Windows installer

`installer\Output\QuantumSetup.exe` is a **snapshot**. It doesn't rebuild itself when `src\` or the compiler
changes. This guide covers what to run after each kind of change so the setup ships the current compiler.

## What goes into the setup

The Inno Setup script [installer/Quantum.iss](../installer/Quantum.iss) copies files that already exist. It
never compiles anything.

| Installed file | Taken from | Produced by |
| --- | --- | --- |
| `quantum.exe`, `qrun.exe`, `quantum_stub.exe`, `qpm.exe` | `build\` | [build-quantum.bat](../build-quantum.bat) |
| `README.md`, `LICENSE.txt` | repo root (`README.md`, `LICENSE`) | hand-edited |
| `TermsAndConditions.txt`, setup icon `assets\quantum.ico` | `installer\` | hand-edited |

The result is `installer\Output\QuantumSetup.exe`. It's tracked in the `installer` submodule's own repo.

> The setup always packages `build\*.exe`. It never uses the copies at the repo root (`qrun.exe`,
> `quantum.exe`, …), which are older leftovers, or the ones in `seed\`. If `build\` is stale, the setup
> is stale too, and ISCC won't warn you.

## Which steps a change needs

| What changed | Steps |
| --- | --- |
| Any `.sa` under `src\` (compiler, VM, CLI, qpm, native backend) | [1 Build](#1-build), [2 Test](#2-test), [3 Package](#3-package), [4 Smoke test](#4-smoke-test), [5 Publish](#5-publish) |
| A **new** source file in `src\` | Add it to [src/selfhost/quantum_sources.sa](../src/selfhost/quantum_sources.sa) (or `qpm_all.sa` for qpm) first, or it isn't compiled in. Then all steps |
| An embedded library: `lib\cpp`, `lib\native`, `src\java\<package>\`, `src\dialect\javastyle\`, `src\js\lib`, `src\ruby\lib`, `src\python\lib` | [Re-embed](#re-embedding-libraries), then all steps |
| A compiler change that `src\` itself starts **using** (new syntax, a codegen fix the build depends on) | [Seed update](#when-src-needs-a-newer-compiler-the-seed), then all steps |
| The version number | [Bump the version strings](#version-numbers), then all steps |
| Only `README.md`, `LICENSE`, `TermsAndConditions.txt`, the icon or `Quantum.iss` | Only [3 Package](#3-package) and [5 Publish](#5-publish) |

## Steps

Run everything from the `compiler\` folder.

### 1. Build

```bat
build-quantum.bat --verify
```

This takes about 2 minutes and runs five stages:

1. `seed\qrun.exe` compiles qrun from `src\`.
2. That qrun compiles itself.
3. The new qrun builds `quantum.exe`.
4. It builds `quantum_stub.exe`.
5. It builds `qpm.exe`.

`--verify` then checks that the new qrun rebuilds itself byte for byte. Don't package a build that fails this
check.

Close anything that is running a `build\*.exe` first (a REPL, a test run, an open terminal session). A locked
exe makes its stage fail.

Then check that the binaries are fresh:

```bat
dir build\*.exe
build\quantum.exe --version
```

### 2. Test

```bat
build\quantum.exe --test tests
python conformance/run.py
```

`qrun.bat` and `quantum.bat` already prefer `build\`, so a quick `qrun some_file.sa` also runs the new build.

### 3. Package

The quickest way is the script in the installer folder:

```bat
installer\build-installer.bat          :: next patch version: 3.0.0 -> 3.0.1
installer\build-installer.bat 3.1.0    :: that version instead
```

The script works in this order:

1. It takes the version from `installer\version.txt` and adds 1 to the last number, unless you give a version.
2. It checks that the four `build\*.exe` files exist and runs ISCC.
3. It copies the result to the website at `..\frontend\public\downloads\QuantumSetup.exe`.
4. It compares the copy byte for byte with the original.
5. Only then does it write the new version to `version.txt`. A failed run therefore doesn't use up a version
   number.

All its paths are relative to `installer\`.

To run ISCC yourself instead, use this in cmd. Without `/DMyAppVersion=`, `Quantum.iss` uses the version in
`version.txt`.

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DMyAppVersion=3.0.0 installer\Quantum.iss
```

Or in PowerShell:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DMyAppVersion=3.0.0 installer\Quantum.iss
```

This writes `installer\Output\QuantumSetup.exe` (about 9 MB).

- `/DMyAppVersion=` sets the version shown in the wizard and in *Installed apps*. Without it, the version in
  `installer\version.txt` is used. Keep it in step with the [version strings](#version-numbers).
- `/DBuildDir=..\other-folder` packages exes from a folder other than `build\`. The path is relative to
  `installer\`.
- `/O<folder>` writes the setup somewhere else, which is useful for a trial build that shouldn't overwrite the
  tracked `Output\QuantumSetup.exe`.
- A per-user Inno Setup installs ISCC at `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`.

### 4. Smoke test

Run the new `QuantumSetup.exe`. It installs per user, with no UAC prompt, to `%LOCALAPPDATA%\Programs\Quantum`.
It also adds that folder to the user `PATH` unless you untick the option. The AppId is fixed, so it upgrades an
existing install in place.

Open a **new** terminal, because `PATH` changes don't reach open ones, and run:

```bat
quantum --version
qrun --version
qpm --version
echo print("hello") > %TEMP%\hello.sa && quantum %TEMP%\hello.sa
```

The last command checks the bundler, which needs `quantum_stub.exe` to be installed next to `quantum.exe`.

### 5. Publish

`installer\` is a git submodule, so the setup is committed in **its** repo first. Then the pointer is updated
here:

```bat
cd installer
git add Output/QuantumSetup.exe Quantum.iss
git commit -m "QuantumSetup 3.0.0"
git push
cd ..
git add installer
git commit -m "Update installer"
```

Commit the `src\` change in this repo too, together with `seed\qrun.exe` if you updated the seed.

`build\` is a separate git checkout of `QuantumLanguageCompiler`. `build-quantum.bat` overwrites the exes in it.
Publishing those binaries is a separate commit in that repo and isn't needed for the installer.

## When `src\` needs a newer compiler (the seed)

Stage [1/5] always compiles `src\` with the checked-in `seed\qrun.exe`. Suppose a change teaches the compiler
something new, and `src\` then starts relying on it. On a clean checkout the old seed can't build `src\`, so
stage 1 fails or produces a broken stage-1 compiler. Do the update in two phases:

1. **Add the feature without using it in `src\`.** Then build with the current seed and make the result the new
   seed:

   ```bat
   build-quantum.bat --verify --seed
   ```

   This copies `build\qrun.exe` over `seed\qrun.exe`. Commit the seed together with the change.
2. **Now use the feature in `src\`** and build again (step 1 onwards). This time stage 1 runs on the new seed.

Notes:

- A successful `build-quantum.bat` on your machine only proves the **committed** seed works if
  `git status seed` is clean. If you re-seeded locally, commit `seed\qrun.exe` with the change. Otherwise
  other machines will fail at stage 1.
- `--seed` updates `seed\qrun.exe` only. `seed\quantum.exe` isn't used by the build.
- Never seed a build that failed `--verify`.

## Re-embedding libraries

The compiler carries some libraries inside itself as generated source in `src\generated\`. After editing one of
the folders below, regenerate them and then rebuild:

```bat
build\qrun.exe --embed
build-quantum.bat --verify
```

| Edited folder | Regenerated file |
| --- | --- |
| `lib\cpp\` | `CppLibData.sa` |
| `lib\native\` | `NativeLibData.sa` |
| `src\java\<package>\` (not the front-end files directly in `src\java\`) | `JavaPreludeData.sa` |
| `src\dialect\javastyle\` | `JavaStyleData.sa` |
| `src\js\lib\` | `JsLibData.sa` |
| `src\ruby\lib\` | `RubyLibData.sa` |
| `src\python\lib\` | `PyLibData.sa` |

Commit the regenerated files with the library change, and don't edit them by hand.

## Version numbers

Only the installer's version is bumped automatically, by `build-installer.bat`. The others are typed in by hand,
and they currently disagree:

| Location | Currently | Shown as |
| --- | --- | --- |
| [installer/version.txt](../installer/version.txt) (read by `Quantum.iss`; `ISCC /DMyAppVersion=` overrides it) | `3.0.0` | setup wizard, *Installed apps* |
| [src/main.sa](../src/main.sa) (both `--version` handlers and the header comment) | `v2.0.0` | `quantum --version`, `qrun --version` |
| [src/cli/Cli.sa](../src/cli/Cli.sa) (banner) | `v2.0.0` | `--help` / REPL banner |
| [src/qpm/QpmMain.sa](../src/qpm/QpmMain.sa) | `qpm 1.0.0` | `qpm --version` (versioned separately) |

For a release with a new number in `quantum --version`, edit the `src\` strings and rebuild (step 1). Then run
`installer\build-installer.bat <that version>` so the installer gets the same number.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| The installed compiler lacks a recent fix | `build\` was stale when you ran ISCC. Rebuild (step 1), check `build\quantum.exe --version` and the timestamps, then package again. |
| ISCC: *Source file "…\build\qpm.exe" does not exist* | `build\` is empty or partial. Run `build-quantum.bat`. |
| Build fails at **[1/5]** but your local qrun can build `src\` | The seed is too old for `src\`. See [the seed](#when-src-needs-a-newer-compiler-the-seed). |
| Build fails at a later stage right after a test run or REPL | A `build\*.exe` was still running and locked. Close it and rebuild. |
| `[error] not a fixed point` | The compiler compiles itself differently the second time. That's a codegen or nondeterminism bug: fix it, and don't seed or ship that build. |
| `quantum` isn't found after installing | The terminal was opened before the install. Open a new one, or check that *Add to PATH* was ticked. |
