<div align="center">

```
 ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗██╗   ██╗███╗   ███╗
██╔═══██╗██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██║   ██║████╗ ████║
██║   ██║██║   ██║███████║██╔██╗ ██║   ██║   ██║   ██║██╔████╔██║
██║▄▄ ██║██║   ██║██╔══██║██║╚██╗██║   ██║   ██║   ██║██║╚██╔╝██║
╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║   ██║   ╚██████╔╝██║ ╚═╝ ██║
 ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝    ╚═════╝ ╚═╝     ╚═╝
```

**A multi-paradigm scripting language with a bytecode compiler and VM — written in Quantum itself**

[![Language](https://img.shields.io/badge/language-Quantum-blue?style=flat-square)](#the-compiler-is-written-in-quantum)
[![Version](https://img.shields.io/badge/version-2.0.0-brightgreen?style=flat-square)](https://github.com/SENODROOM/Quantum-Language)
[![Extension](https://img.shields.io/badge/scripts-.sa-orange?style=flat-square)](#)
[![License](https://img.shields.io/badge/license-MIT-purple?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey?style=flat-square)](#build)

</div>

---

## What is Quantum?

Quantum is a dynamically typed, multi-paradigm scripting language that compiles `.sa` source files to bytecode and runs them on a custom register-stack VM. It accepts **Python-style**, **JavaScript-style**, and **C-style** syntax — all valid in the same file.

The build produces **three binaries** with distinct roles:

| Binary             | Role                                                                           |
| ------------------ | ------------------------------------------------------------------------------ |
| `quantum.exe`      | Compiles source → bytecode → bundles into a self-contained `.exe`, then runs it |
| `qrun.exe`         | Interprets source directly — no `.exe` is generated                            |
| `quantum_stub.exe` | The bare VM runtime that gets bundled into produced executables                |

Both `quantum` and `qrun` accept `.sa`, `.js`, `.py`, `.c`, and `.cpp` files. Every extension runs **natively on the Quantum VM** through the same multi-syntax front-end — no Node.js, Python, or GCC installation is required. (Non-`.sa` files are supported at the level of Quantum's multi-syntax subset, not the full host language.)

```bash
quantum hello.sa     # → hello.exe created, then launched
qrun    hello.sa     # → interpreted directly, nothing written to disk
hello.exe            # → the produced standalone executable, no runtime required
```

---

## How it Works

### The Two Execution Paths

**`quantum` — compile + bundle:**

```
.sa source
   │
   ▼
Lexer  →  Token stream
   │
   ▼
Parser  →  AST
   │
   ▼
TypeChecker  (static warnings only, does not block execution)
   │
   ▼
Compiler  →  Bytecode Chunk
   │
   ▼
Serializer  →  binary payload
   │
   ▼
Copy quantum_stub.exe → hello.exe
Append [payload bytes][payload size: uint32 LE]["QNTM_VM!" magic]
   │
   ▼
Launch hello.exe via Win32 CreateProcess, wait for exit
```

When `hello.exe` runs, it checks for the `QNTM_VM!` magic trailer, reads back the payload, deserializes the `Chunk`, and immediately feeds it to the VM.

**`qrun` — direct interpretation:**

```
.sa source  →  Lexer  →  Parser  →  TypeChecker  →  Compiler  →  VM::run()
```

Same front-end and same VM. The only difference is that no `.exe` is written to disk.

---

### The Compiler

The compiler (`src/compiler/`) walks the AST in a single pass and emits a flat array of `Instruction` structs into a `Chunk`. Each `Chunk` represents one function body (or the top-level script). Functions are compiled into nested `Chunk`s stored as constants of the parent chunk, then wrapped at runtime by `MAKE_CLOSURE`.

The compiler maintains a `CompilerState` stack that tracks locals, scope depth, and upvalue captures across nested function boundaries. Forward jumps are emitted with a placeholder operand and backpatched once the target instruction index is known.

Key instruction categories:

| Category     | Opcodes                                                                   |
| ------------ | ------------------------------------------------------------------------- |
| Stack        | `LOAD_CONST`, `LOAD_NIL`, `LOAD_TRUE`, `LOAD_FALSE`, `POP`, `DUP`, `SWAP` |
| Variables    | `DEFINE_GLOBAL/LOCAL/CONST`, `LOAD/STORE_GLOBAL/LOCAL`                    |
| Upvalues     | `LOAD_UPVALUE`, `STORE_UPVALUE`, `CLOSE_UPVALUE`                          |
| Arithmetic   | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `FLOOR_DIV`, `NEG`              |
| Bitwise      | `BIT_AND`, `BIT_OR`, `BIT_XOR`, `BIT_NOT`, `LSHIFT`, `RSHIFT`             |
| Comparison   | `EQ`, `NEQ`, `LT`, `LTE`, `GT`, `GTE`                                     |
| Control flow | `JUMP`, `JUMP_IF_FALSE`, `JUMP_IF_TRUE`, `LOOP`, `JUMP_ABSOLUTE`          |
| Functions    | `CALL`, `RETURN`, `RETURN_NIL`, `MAKE_FUNCTION`, `MAKE_CLOSURE`           |
| Collections  | `MAKE_ARRAY`, `MAKE_DICT`, `MAKE_TUPLE`                                   |
| Members      | `GET_INDEX`, `SET_INDEX`, `GET_MEMBER`, `SET_MEMBER`                      |
| Classes      | `MAKE_CLASS`, `INHERIT`, `BIND_METHOD`, `INSTANCE_NEW`                    |
| Exceptions   | `PUSH_HANDLER`, `POP_HANDLER`, `RAISE`, `RERAISE`                         |
| Pointers     | `ADDRESS_OF`, `DEREF`, `ARROW`                                            |
| Iteration    | `MAKE_ITER`, `FOR_ITER`                                                   |

---

### The VM

The VM (`src/vm/`) is a stack-based bytecode interpreter built around a `CallFrame` stack. Each frame holds a `Closure` (the compiled chunk + captured upvalues), an instruction pointer, and a stack base offset for locals.

```
struct CallFrame {
    std::shared_ptr<Closure> closure;
    size_t ip;         // instruction pointer into closure->chunk->code
    size_t stackBase;  // where this frame's locals start on the value stack
};
```

Upvalues are heap-allocated shared cells (`Upvalue::cell`) that initially point into the live value stack. When the enclosing variable goes out of scope, `CLOSE_UPVALUE` copies the live value into `Upvalue::closed` so closures keep working after their enclosing frame exits.

Exception handling uses a separate `ExceptionHandler` stack. `PUSH_HANDLER` records the catch IP and the frame/stack depths to unwind to. `RAISE` walks the handler stack, unwinds call frames, restores the value stack, and jumps to the catch block. `try/catch` and `throw` are fully supported at the language level.

**Value representation** uses `std::variant` over twelve concrete types:

```
nil · bool · double · string · Array* · Dict* · Closure* ·
NativeFunc* · Instance* · Class* · BoundMethod* · Pointer*
```

Pointers (`&var`, `*ptr`, `ptr->member`) are first-class values backed by `shared_ptr<QuantumValue>`, enabling C-style pointer semantics alongside the rest of the type system.

---

### Serializer

`src/Serializer.sa` converts a `Chunk` tree to a flat binary payload and back. It walks the chunk recursively — opcodes, operands, line info, and constants (including nested sub-chunks for functions) — writing everything to a `vector<uint8_t>`. This is the payload appended to `quantum_stub.exe` to create `hello.exe`.

The trailer format is:

```
[bytecode payload ...] [payloadSize: uint32 LE] ["QNTM_VM!" : 8 bytes]
```

On startup, `quantum_stub.exe` seeks to the end of its own PE image, checks for the magic string, reads the payload size, deserializes the `Chunk`, and runs it.

---

### Disassembler

Pass `--dis` or `--debug` to dump compiled bytecode before running:

```bash
quantum --dis   hello.sa    # print bytecode, exit
quantum --debug hello.sa    # print bytecode, then run
```

Output shows each instruction offset, opcode name, operand, source line number, and — for `LOAD_CONST` — the constant value inline.

---

## Language

### File extension

Quantum source files use the `.sa` extension.

### Multi-syntax

The same constructs can be written in Python-style, brace-style, or C-style — all in the same file:

```python
# if — three styles
if x > 0:
    print("positive")           # Python-style

if x > 0 { print("positive") } # brace-style

if(x > 0) { printf("%d\n", x) } # C-style
```

### Variables

```python
name   = "Alice"               # bare assignment
let x  = 42                    # quantum-style
const MAX = 100                # constant — cannot be reassigned

int   count = 0                # C-style type hint (hint only — dynamically typed)
float pi    = 3.14
bool  flag  = false
```

### Functions — five styles

```python
fn add(a, b) { return a + b }           # quantum

def greet(name): return "Hi, " + name  # python

function mul(a, b) { return a * b }    # javascript

double = (x) => x * 2                  # arrow

square = fn(n) { return n * n }        # anonymous
```

### Closures

Functions capture their enclosing scope through upvalues:

```python
fn make_counter(start) {
    let count = start
    return fn() {
        count += 1
        return count
    }
}

let c = make_counter(0)
print(c(), c(), c())   # 1 2 3
```

### Classes and inheritance

```python
class Animal {
    fn init(name, sound) {
        self.name  = name
        self.sound = sound
    }
    fn speak() { return self.name + " says " + self.sound }
}

class Dog extends Animal {
    fn fetch(item) { return self.name + " fetches " + item }
}

let dog = Dog("Rex", "Woof")
print(dog.speak())
print(dog.fetch("ball"))
```

### Exception handling

```python
try {
    if x == 0 { throw "division by zero" }
    print(100 / x)
} catch (e) {
    print("Caught:", e)
}
```

### Pointers

C-style pointer semantics are first-class values in the VM:

```python
let x = 42
let p = &x        # address-of — p holds a live reference to x
*p = 99           # dereference + assign
print(x)          # 99
```

### Bitwise operations

```python
let a = 0xFF
let b = 0x0F

print(a & b)        # AND   → 15
print(a | b)        # OR    → 255
print(a ^ b)        # XOR   → 240
print(~a)           # NOT   → -256
print(1 << 8)       # SHL   → 256
print(256 >> 4)     # SHR   → 16
print(hex(a ^ b))   # 0xf0
```

### I/O — multiple styles

```python
print("hello", name)
printf("Score: %d / %d\n", score, total)
cout << "Value: " << x << endl

scanf("%d", &n)
cin >> name
```

---

## Standard Library

Quantum ships a large set of native functions registered directly in the VM.

### Core

```
len()  type()  typeof()  range()  print()  input()  assert()  exit()
list()  enumerate()  zip()  map()  filter()  sorted()  reversed()
sum()  any()  all()  isinstance()
```

### Math

```
abs  sqrt  floor  ceil  round  pow  log  log2  log10
sin  cos  tan  asin  acos  atan  atan2  min  max
is_prime  gcd  lcm  mod_pow
PI  E  INF
```

### Type conversion

```
num  int  float  str  bool  chr  ord
parseInt  parseFloat  isNaN  hex  bin
```

### String methods

```
.trim()  .upper()  .lower()  .split(sep)  .replace(a, b)
.contains(s)  .starts_with(s)  .ends_with(s)  .index_of(s)
.slice(a, b)  .repeat(n)
```

### Array methods

```
.push(v)  .pop()  .slice(a, b)  .map(fn)  .filter(fn)  .reduce(fn, init)
.includes(v)  .index_of(v)  .sort()  .reverse()  .join(sep)
```

### Dictionary methods

```
.get(k)  .set(k, v)  .has(k)  .remove(k)  .keys()  .values()
```

### File I/O

```python
write_file("output.txt", content)
data = read_file("input.txt")
```

### Encoding

```
base64_encode  base64_decode
to_hex  from_hex
url_encode  url_decode
str_to_hex_escape
xor_bytes(a, b)  rot13(s)
```

### Hashing and crypto

```
sha256(s)                              # → hex digest
sha1(s)                                # → hex digest
md5(s)                                 # → hex digest
hmac_sha256(key, msg)                  # → hex digest
aes128_ecb_encrypt(key, plaintext)
aes128_ecb_decrypt(key, ciphertext)
vigenere_encrypt(key, text)
vigenere_decrypt(key, text)
pkcs7_pad(data, block_size)
pkcs7_unpad(data)
constant_time_eq(a, b)                 # timing-safe string comparison
```

### Random and entropy

```
secure_random_hex(n_bytes)
secure_random_int(min, max)
entropy(s)             # Shannon entropy of a string
```

### Network helpers

```
ip_to_int(ip)
ip_in_cidr(ip, cidr)
cidr_hosts(cidr)       # → array of host IPs in subnet
parse_http_request(raw)
```

### String distance

```
hamming_distance(a, b)
edit_distance(a, b)    # Levenshtein
luhn_check(n)
```

### printf format specifiers

| Spec        | Meaning             |
| ----------- | ------------------- |
| `%d` / `%i` | Integer             |
| `%f`        | Float               |
| `%e`        | Scientific notation |
| `%s`        | String              |
| `%c`        | Character           |
| `%x` / `%X` | Hex lower / upper   |
| `%o`        | Octal               |
| `%b`        | Binary              |

---

## CLI Reference

### `quantum` — compiler + bundler

```
quantum <file>              Compile → <file>.exe, then run it
quantum --run <file>        Interpret directly (no .exe created)
quantum --check <file>      Parse + type-check only, no execution
quantum --debug <file>      Dump bytecode disassembly, then run
quantum --dis   <file>      Dump bytecode disassembly only, then exit
quantum --test  [dir]       Batch-test all .sa/.js/.py/.c/.cpp files in directory
quantum --version           Print version string
quantum --help              Show usage
quantum --aura              Print achievement board
```

`<file>` may be `.sa`, `.js`, `.py`, `.c`, or `.cpp` — all compiled and run by Quantum itself (e.g. `quantum prog.c` produces a standalone `prog.exe` without gcc).

### `qrun` — direct interpreter

```
qrun <file>                 Interpret in-place, no .exe produced
qrun                        Start interactive REPL
```

`qrun` is identical to `quantum --run` but as a standalone binary — useful when you want a pure interpreter that never writes to disk.

### REPL

Launch `qrun` with no arguments. The VM state persists across lines, so variables and functions defined in earlier lines remain in scope:

```
quantum[1]> let x = 10
quantum[2]> fn double(n) { return n * 2 }
quantum[3]> print(double(x))
20
quantum[4]> exit
```

### Batch test runner

```bash
quantum --test examples/
quantum --test tests/
```

Runs every `.sa` file in the directory, prints `PASS` / `FAIL` per file with error location and source context, and writes a full `test_results.txt` report. The runner is crash-guarded — a segfault or abort in one file is caught via POSIX signal handlers and `setjmp/longjmp` so the process continues testing the rest.

---

## Build

### The compiler is written in Quantum

Everything under `src/` is Quantum. A file whose first line is `// quantum: cpp` is written in Quantum's C-style syntax; `X.h.sa` declares and `X.sa` defines. Quantum compiles these itself: `qrun src/tools/selfhost/quantumc.sa <file> <out.qbc>` is the whole compiler, compiled by Quantum and running on the Quantum VM. Fed its own source, it reproduces itself byte for byte (`src/tools/bootstrap.sh`).

### Building

```bat
build-quantum.bat            :: build\{qrun,quantum,quantum_stub,qpm}.exe
build-quantum.bat --verify   :: and check the new qrun rebuilds itself byte for byte
build-quantum.bat --seed     :: and make the new qrun the seed
```

No external compiler or toolchain is involved, only Windows' `cmd`. `seed\qrun.exe` is qrun compiled to x86-64 machine code by Quantum's own native backend (`src/native/`). It compiles `src\` (`--native src\tools\selfhost\qrun_all.sa`) into a new qrun, which compiles itself again; that one builds `quantum.exe`, `quantum_stub.exe` and `qpm.exe`. Every tool, the VM included, is native code Quantum generated: there is no outside compiler, assembler or linker anywhere in the chain, and the C runtime functions used are Windows' own (`ucrtbase.dll`). A qrun build takes about 30 seconds.

The build is a fixed point: the qrun built by the seed and the qrun built by itself are byte-identical (`bash src/tools/native_bootstrap.sh` checks this). Any edit under `src\`, the VM runtime included, takes effect through this build; `--seed` then makes it the new seed.

---

## Project Structure

```
Quantum-Language/
├── src/                          # The compiler's source — Quantum
│   ├── main.sa                   # Entry point — mode dispatch, REPL, bundler, test runner
│   ├── include/                  # Declarations (X.h.sa): AST, Token, Value, Vm, Opcode, ...
│   ├── lexer/  parser/           # Lexer and Pratt parser
│   ├── compiler/                 # AST → bytecode (closures, upvalues)
│   ├── vm/                       # Bytecode VM, natives, dialect runtimes
│   ├── cpp/                      # Typed C-family front-end
│   ├── java/                     # Java front-end; lang/ io/ util/: its standard library
│   ├── dialect/                  # Python / JS / Ruby / C dialect passes
│   ├── cli/                      # CLI, bundler, test runner, self-host hooks
│   ├── qpm/                      # qpm package manager
│   ├── Serializer.sa  Disassembler.sa  TypeChecker.sa  Pipeline.sa  ...
│   ├── lexer.sa  parser*.sa  ast.sa   # an independent port in Quantum's own syntax
│   └── tools/
│       ├── selfhost/quantumc.sa  # the whole compiler as one program (self-compile driver)
│       ├── bootstrap.sh          # fixed point: q1 == q2 == q3
│       └── compare_selfhost.sh   # self-hosted output check on every example
├── examples/                     # Runnable .sa programs
│   ├── hello.sa
│   ├── features.sa               # Full feature showcase
│   ├── advanced.sa               # Higher-order functions, closures, algorithms
│   └── cybersec.sa               # Encoding, hashing, XOR demos
├── tests/                        # Test programs (run with --test)
│   ├── hangman.sa
│   ├── minesweeper.sa
│   ├── snake.sa
│   ├── dungeon.sa
│   ├── merge sort.sa
│   ├── password_generator.sa
│   ├── Tower of Hanoi Algorithm.sa
│   ├── text_analyzer.sa
│   └── ...
├── docs/
│   ├── SYNTAX.md                 # Complete language reference
│   ├── ARCHITECTURE.md           # Pipeline deep-dive
│   ├── SETUP.md                  # Installation guide
│   ├── CONTRIBUTING.md
│   └── SECURITY.md
├── seed/qrun.exe                 # The seed compiler that starts the self-hosted build
├── build-quantum.bat             # Builds every tool with Quantum itself
├── quantum.bat                   # Launcher wrapper (locates quantum.exe)
└── qrun.bat                      # Launcher wrapper (locates qrun.exe)
```

---

## Documentation

| File                                         | Contents                               |
| -------------------------------------------- | -------------------------------------- |
| [docs/SYNTAX.md](docs/SYNTAX.md)             | Complete language and stdlib reference |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Compiler and VM internals              |
| [docs/SETUP.md](docs/SETUP.md)               | Installation and build guide           |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) | Contribution guidelines                |
| [docs/SECURITY.md](docs/SECURITY.md)         | Security policy                        |

---

<div align="center">

**Built in Quantum · Self-hosted · Bytecode VM edition**

[⭐ Star this repo](https://github.com/SENODROOM/Quantum-Language) · [🐛 Report a bug](https://github.com/SENODROOM/Quantum-Language/issues)

</div>
