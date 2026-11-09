# src/

The source of Quantum, written in Quantum: lexer, parser, bytecode compiler,
VM, the other language front-ends (C/C++, Java, Python, JS, Ruby), CLI and
qpm.

Most of it is Quantum's C++ syntax. Such a file starts with the line
`// quantum: cpp`; `X.h.sa` declares and `X.sa` defines. `lexer.sa`, `ast.sa`
and `parser*.sa` are a separate port of the lexer and parser in Quantum's own
syntax.

## Self-hosting

`selfhost/quantumc.sa` is the whole compiler as one program. Quantum
compiles it, and it then compiles any Quantum program on the Quantum VM:

    qrun --emit-qbc src/selfhost/quantumc.sa build/quantumc.qbc
    qrun --exec build/quantumc.qbc prog.sa prog.qbc

The other drivers in `selfhost/` run one stage of it the same way, and
each prints what a `qrun` flag prints, so the two can be compared on any file:

| Driver                 | Same output as                      |
|------------------------|-------------------------------------|
| `selfhost/lexdump.sa`  | `qrun --dump-tokens <file>`         |
| `selfhost/astdump.sa`  | `qrun --dump-ast <file>`            |
| `selfhost/qbc.sa`      | `qrun --emit-qbc-core <file> <out>` |
| `selfhost/quantumc.sa` | `qrun --emit-qbc <file> <out>`      |

## Building without a C++ compiler

`../build-quantum.bat` builds every tool from this directory with no C++
compiler. `../seed/qrun.exe` is qrun compiled to x86-64 by `native/`:
- it compiles `selfhost/qrun_all.sa` (all of qrun: compiler and VM) to a
  new native qrun;
- that qrun compiles itself again, then `quantum_all.sa`, `stub_all.sa` and
  `qpm_all.sa` (quantum.exe, quantum_stub.exe, qpm.exe).

The drivers include every source as its own translation unit
(`#pragma quantum_tu`), the files CMake builds for each target. `--verify`
checks the fixed point: the new qrun rebuilds itself byte for byte.

## The native backend

`native/` compiles C++ straight to an x86-64 Windows executable: no
assembler, linker or C++ compiler (`quantum --native prog.cpp prog.exe`).
It handles:
- integers, floating point, pointers, arrays
- classes with constructors, destructors, inheritance and virtual functions
- references, operator overloading, function and class templates
- lambdas, exceptions, threads and atomics
- the standard library in `../lib/native/` (strings, containers, streams,
  filesystem, regex, random, chrono, zlib, the Windows API), written in C++
  and carried inside the compiler (`qrun --embed` writes `generated/`)

It compiles the whole of Quantum: qrun, quantum, quantum_stub and qpm.
