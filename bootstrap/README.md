# bootstrap/

The C++ implementation of Quantum: lexer, parser, compiler, VM, the other
language front-ends, CLI and qpm. It builds `quantum`, `qrun`, `quantum_stub`
and `qpm` (see `../CMakeLists.txt`).

It is temporary. The compiler's own source is Quantum, in `../src/`, and each
part here is deleted once its Quantum replacement produces the same results
on every example:

| Part            | Here                         | Quantum (`../src/`)         | Check                          |
|-----------------|------------------------------|-----------------------------|--------------------------------|
| Lexer           | `src/lexer/`                 | `lexer.sa`                  | `qrun --dump-tokens` vs `tools/lexdump.sa` |
| Parser          | `src/parser/`, `include/AST.h` | `ast.sa`, `parser*.sa`    | `tools/compare_ast.sh`         |
| Bytecode compiler | `src/compiler/`            | not yet                     | `qrun --emit-qbc`              |
| VM and runtime  | `src/vm/`, `src/Value.cpp`   | not yet (needs a native backend) | the example suite         |

The C++ can only go once nothing needs it to build or run: the Quantum
compiler has to compile itself, and the VM needs native code generation.
