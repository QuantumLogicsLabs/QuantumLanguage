# SYNTAX.md — Quantum Language Reference

> **Quantum** v1.0.0 — Complete language syntax and built-in function reference.

---

## Table of Contents

- [Overview](#overview)
- [File Extension](#file-extension)
- [Comments](#comments)
- [Variables](#variables)
- [Data Types](#data-types)
- [Operators](#operators)
- [Control Flow](#control-flow)
- [Loops](#loops)
- [Functions](#functions)
- [Closures](#closures)
- [Classes & OOP](#classes--oop)
- [Arrays](#arrays)
- [Dictionaries](#dictionaries)
- [Strings](#strings)
- [I/O — Input & Output](#io--input--output)
- [Format Strings](#format-strings)
- [Standard Library](#standard-library)
  - [Math](#math)
  - [Type Conversion](#type-conversion)
  - [Encoding & Cybersecurity](#encoding--cybersecurity)
  - [Utility](#utility)
- [Bitwise Operations](#bitwise-operations)
- [Reserved Cybersecurity Keywords](#reserved-cybersecurity-keywords)
- [CLI Flags](#cli-flags)

---

## Overview

Quantum is a **multi-paradigm** scripting language. It accepts Python-style, JavaScript-style, and C/C++-style syntax — all valid in the same `.sa` file. There are no strict indentation rules unless you use Python-style colons (in which case indentation guides blocks).

---

## File Extension

Quantum source files use the **`.sa`** extension.

```
myscript.sa
xor_tool.sa
scanner.sa
```

---

## Comments

```python
# This is a single-line comment

# Multi-line comments are written as
# multiple single-line comments.
# There is no block comment syntax.
```

---

## Variables

Quantum supports four declaration styles. All are equivalent at runtime.

```python
# 1. Python-style (bare assignment — most common)
name   = "Alice"
score  = 100
active = true

# 2. Quantum-style keywords
let x     = 42
const MAX = 1000

# 3. C-style type-annotated declarations
int    count   = 0
float  pi      = 3.14
double big     = 1e15
char   initial = "A"
string message = "hello"
bool   flag    = false
```

> **Note:** Type annotations in C-style declarations are hints only. Quantum is dynamically typed — variables can change type at runtime.

`const` variables **cannot** be reassigned after declaration.

---

## Data Types

| Type | Example | Notes |
|---|---|---|
| Number (int/float) | `42`, `3.14`, `1e10` | Unified numeric type |
| String | `"hello"`, `'world'` | Double or single quotes |
| Boolean | `true`, `false` | Lowercase only |
| Null | `null` | Represents absence of value |
| Array | `[1, 2, 3]` | Ordered, mutable, mixed types |
| Dictionary | `{"key": value}` | Key-value map |
| Function | `fn(x) { ... }` | First-class value |

---

## Operators

### Arithmetic

```python
x + y     # Addition
x - y     # Subtraction
x * y     # Multiplication
x / y     # Division (float result)
x % y     # Modulo
x ** y    # Exponentiation (if supported)
```

### Comparison

```python
x == y    # Equal
x != y    # Not equal
x < y     # Less than
x > y     # Greater than
x <= y    # Less than or equal
x >= y    # Greater than or equal
```

### Logical

```python
x and y   # Logical AND
x or  y   # Logical OR
not x     # Logical NOT
```

### Assignment

```python
x = 10    # Assignment
x += 5    # Add and assign
x -= 3    # Subtract and assign
x *= 2    # Multiply and assign
x /= 4    # Divide and assign
x++       # Post-increment
++x       # Pre-increment
x--       # Post-decrement
--x       # Pre-decrement
```

### Bitwise

```python
a & b     # AND
a | b     # OR
a ^ b     # XOR
~a        # NOT (bitwise complement)
a << n    # Left shift
a >> n    # Right shift
```

---

## Control Flow

### If / Elif / Else

**Python style:**
```python
if score > 90:
    print("A grade")
elif score > 75:
    print("B grade")
else:
    print("Try harder")
```

**Brace style:**
```javascript
if score > 90 {
    print("A grade")
} else if score > 75 {
    print("B grade")
} else {
    print("Try harder")
}
```

**C-style (with parentheses):**
```c
if(score > 90) {
    printf("A grade\n")
} else {
    printf("Try harder\n")
}
```

**Single-line (no braces or colon):**
```python
if x > 0 print("positive")
```

---

## Loops

### While Loop

```python
# Python style
i = 0
while i < 10:
    print(i)
    i++

# Brace style
i = 0
while i < 10 {
    print(i)
    i++
}

# C style
while(i < 10) {
    printf("%d\n", i)
    i++
}
```

### For-in Loop (iterating over arrays)

```python
for item in ["a", "b", "c"]:
    print(item)
```

### Range-based For Loop

```python
for n in range(5):          # 0, 1, 2, 3, 4
    print(n)

for n in range(1, 6):       # 1, 2, 3, 4, 5
    print(n * n)
```

### Break and Continue

```python
for n in range(10):
    if n == 5:
        break       # Exit loop
    if n % 2 == 0:
        continue    # Skip to next iteration
    print(n)
```

---

## Functions

Quantum supports **five** function definition styles.

### Quantum Style (`fn`)

```python
fn add(a, b) {
    return a + b
}

fn greet(name):
    return "Hello, " + name + "!"
```

### Python Style (`def`)

```python
def factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)
```

### JavaScript Style (`function`)

```javascript
function multiply(a, b) {
    return a * b
}
```

### Arrow Functions

```javascript
# Expression body (implicit return)
double = (x) => x * 2
add    = (a, b) => a + b
triple = x => x * 3           # Single param, no parens needed

# Block body (explicit return)
clamp = (x, min, max) => {
    if x < min: return min
    if x > max: return max
    return x
}
```

### Anonymous Functions

```python
square = fn(n) { return n * n }
cube   = function(n) { return n * n * n }
```

### Default and Variadic Parameters

> Quantum is dynamically typed — callers may pass fewer args (extras default to `null`). Explicit default syntax may vary by version.

---

## Closures

Functions capture their enclosing scope:

```python
fn make_counter(start):
    count = start
    return fn():
        count += 1
        return count

counter = make_counter(0)
print(counter())   # 1
print(counter())   # 2
print(counter())   # 3
```

---

## Classes & OOP

### Defining a Class

```python
class Animal:
    fn init(name, sound):
        self.name  = name
        self.sound = sound

    fn speak():
        return self.name + " says " + self.sound
```

### Inheritance

```python
class Dog extends Animal:
    fn fetch(item):
        return self.name + " fetches the " + item

dog = Dog("Rex", "Woof")
print(dog.speak())       # Rex says Woof
print(dog.fetch("ball")) # Rex fetches the ball
```

### Notes on OOP

- `init` is the constructor method (called automatically on instantiation)
- `self` refers to the current instance (no explicit `self` parameter needed in method calls)
- `extends` provides single inheritance
- Parent methods can be accessed via the inherited instance

---

## Arrays

```python
nums = [10, 20, 30, 40, 50]

# Access
nums[0]              # 10 (zero-indexed)
nums[-1]             # 50 (last element)

# Mutation
nums.push(60)        # Append element
nums.pop()           # Remove and return last element

# Slicing
nums.slice(1, 4)     # [20, 30, 40]

# Information
len(nums)            # Length: 5
nums.includes(30)    # true
nums.index_of(30)    # 2

# Transformation
doubled = nums.map(fn(n) { return n * 2 })
evens   = nums.filter(fn(n) { return n % 2 == 0 })
total   = nums.reduce(fn(acc, n) { return acc + n }, 0)

# Arrow function style
doubled = nums.map(n => n * 2)
evens   = nums.filter(n => n % 2 == 0)

# Sorting
nums.sort()
nums.reverse()
```

---

## Dictionaries

```python
user = {
    "name":  "Alice",
    "role":  "pentester",
    "level": 99
}

# Access
print(user["name"])            # Alice

# Add / update
user["tools"] = ["nmap", "burp"]

# Methods
user.get("role")               # "pentester"
user.keys()                    # ["name", "role", "level", "tools"]
user.values()                  # ["Alice", "pentester", 99, [...]]
user.remove("level")           # Remove a key
```

---

## Strings

```python
msg = "  Hello Quantum World  "

msg.trim()                     # "Hello Quantum World"
msg.upper()                    # "  HELLO QUANTUM WORLD  "
msg.lower()                    # "  hello quantum world  "
msg.split(" ")                 # Array of words
msg.replace("World", "Earth")  # "  Hello Quantum Earth  "
msg.contains("Quantum")        # true
msg.starts_with("  Hello")     # true
msg.ends_with("World  ")       # true
len(msg)                       # 23
```

### String Concatenation

```python
full_name = "Alice" + " " + "Smith"
greeting  = "Hello, " + name + "!"
```

---

## I/O — Input & Output

Quantum supports three I/O styles interchangeably.

### Output

```python
# Python / Quantum style
print("Hello, World!")
print("x =", x, "| y =", y)        # Multiple args, space-separated

# C-style printf
printf("Score: %d out of %d\n", score, total)
printf("%08X\n", 255)

# C++ style cout
cout << "Hello, " << name << endl
cout << "Value: " << x + y << "\n"
```

### Input

```python
# C-style scanf
scanf("%d", &value)

# C++ style cin
cin >> name
```

---

## Format Strings

Used with `printf()` and `format()`.

| Specifier | Meaning |
|---|---|
| `%d` / `%i` | Integer |
| `%f` | Float |
| `%e` | Scientific notation |
| `%s` | String |
| `%c` | Character |
| `%x` / `%X` | Hex (lower / upper) |
| `%o` | Octal |
| `%b` | Binary |
| `%%` | Literal `%` |

**Width, precision, and flags:**

```python
printf("%-10s %6.2f\n", "Price:", 3.14159)   # Left-aligned string, fixed-width float
printf("%08X\n", 255)                          # Zero-padded hex: 000000FF

s = format("Hello, %s! You scored %d%%", name, score)
```

---

## Standard Library

### Math

| Function | Description |
|---|---|
| `abs(x)` | Absolute value |
| `sqrt(x)` | Square root |
| `pow(x, y)` | x to the power y |
| `floor(x)` | Round down |
| `ceil(x)` | Round up |
| `round(x)` | Round to nearest integer |
| `log(x)` | Natural logarithm |
| `log2(x)` | Base-2 logarithm |
| `sin(x)` | Sine (radians) |
| `cos(x)` | Cosine (radians) |
| `tan(x)` | Tangent (radians) |
| `min(a, b, ...)` | Minimum of values |
| `max(a, b, ...)` | Maximum of values |
| `PI` | π ≈ 3.14159... |
| `E` | e ≈ 2.71828... |
| `INF` | Infinity |

### Type Conversion

| Function | Description |
|---|---|
| `num(x)` | Convert to number |
| `str(x)` | Convert to string |
| `bool(x)` | Convert to boolean |
| `type(x)` | Get type name as string |
| `chr(n)` | Integer to character |
| `ord(c)` | Character to integer |

### Encoding & Cybersecurity

| Function | Description |
|---|---|
| `hex(n)` | Number to hex string (e.g., `0xff`) |
| `bin(n)` | Number to binary string |
| `to_hex(s)` | String bytes → hex representation |
| `from_hex(s)` | Hex string → byte string |
| `xor_bytes(a, b)` | XOR two byte strings together |
| `base64_encode(s)` | Base64-encode a string |
| `rot13(s)` | Apply ROT13 cipher |

### Utility

| Function | Description |
|---|---|
| `len(x)` | Length of string, array, or dict |
| `range(n)` | Integers from 0 to n-1 |
| `range(a, b)` | Integers from a to b-1 |
| `rand()` | Random float between 0.0 and 1.0 |
| `rand(a, b)` | Random float in range [a, b] |
| `rand_int(a, b)` | Random integer in range [a, b] |
| `time()` | Unix timestamp (seconds) |
| `sleep(s)` | Pause execution for s seconds |
| `assert(cond, msg)` | Throw error if condition is false |
| `exit(code)` | Exit program with status code |
| `format(fmt, ...)` | Format string (sprintf-style) |
| `keys(dict)` | Dict keys as array |
| `values(dict)` | Dict values as array |

### Folders & Script Arguments

| Function | Description |
|---|---|
| `sys.argv` | Script path followed by its arguments: `qrun tool.sa --json` → `["tool.sa", "--json"]` |
| `os.listdir(path)` | Names of the entries in a folder, sorted; raises if the folder can't be read |
| `os.path.isdir(path)` | `true` if `path` is a folder |
| `os.path.abspath(path)` | Absolute, normalized path with `/` separators |

---

## Bitwise Operations

```python
a = 0xFF
b = 0x0F

print(a & b)        # AND  → 15
print(a | b)        # OR   → 255
print(a ^ b)        # XOR  → 240
print(~a)           # NOT  → -256
print(1 << 8)       # SHL  → 256
print(256 >> 4)     # SHR  → 16

# Hex display
print(hex(a & b))   # 0xf
```

Hex and binary literals are fully supported:

```python
mask   = 0xFF00
flag   = 0b10101010
result = mask & 0x0F0F
```

---

## Reserved Cybersecurity Keywords

These keywords are **reserved** for upcoming features and cannot be used as identifiers:

| Keyword | Planned Feature |
|---|---|
| `scan` | Network scanning (ports, hosts) |
| `payload` | Exploit payload construction |
| `encrypt` | Cryptographic encryption (AES, RSA) |
| `decrypt` | Cryptographic decryption |
| `hash` | Hashing (MD5, SHA-256, SHA-512, bcrypt) |

---

## CLI Flags

```
quantum <file.sa>          Run a Quantum script
qrun <file.sa> [args...]   Interpret a script; args are available in sys.argv
quantum                    Start interactive REPL
quantum --check <file.sa>  Syntax check without execution
quantum --version          Display version string
quantum --help             Show help text
quantum --aura             Show project achievement board
```

---

*For installation instructions, see [SETUP.md](SETUP.md).*  
*For the full project overview, see [README.md](README.md).*