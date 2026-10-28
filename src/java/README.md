# Java

Quantum's Java support: the front-end that compiles `.java` programs, and the
Java standard library it compiles them with.

## Layout

| Path | What it is |
| --- | --- |
| `Java*.sa` (this folder) | The front-end, in Quantum's C++ syntax: lexer (`JavaLex`), parser (`JavaParse`, `JavaAst.h`), semantic analysis (`JavaSema.h`, `JavaEnter`, `JavaTypes`, `JavaExpr`, `JavaStmt`, `JavaCall`), lowering to the Quantum AST (`JavaEmit`, `JavaLib`), number formatting (`JavaNumberFormat`) and the entry point (`JavaMain`). |
| `lang/` | `java.lang` |
| `io/` | `java.io` |
| `util/` | `java.util`, with `util/function/`, `util/stream/`, `util/regex/`, `util/concurrent/` (and `concurrent/atomic/`) for its subpackages |

The library is written in Java, one file per top-level type, named after it
(`util/ArrayList.sa` holds `java.util.ArrayList`). The files use the `.sa`
extension like the rest of the compiler source, but their content is Java.

The compiler carries the library inside itself: `src/tools/embed.sa` collects
every `.sa` file in this folder's subfolders (not the front-end's own files)
into `src/generated/JavaPreludeData.sa` (`java::preludeSources()`), naming each
one `<Type>.java`, the file name a library frame shows in a stack trace. After
changing a library file, run the embed tool from `compiler/`
(`qrun src/tools/embed.sa`) and rebuild.

## Conventions

- **Runtime representation** (see `src/include/JavaOps.h.sa`): a `String` is a
  VM string; a boxed `Integer` is the integer itself, `Double` / `Float` the
  double, `Boolean` the bool. `Long`, `Short`, `Byte` and `Character` boxes are
  objects of their class (with a `value` field), cached for -128..127. Methods of
  the value-represented classes treat `this` as the value.
- **Native methods**: a method declared `native` is implemented by the VM native
  `__jn_<Class>_<method>` (`src/vm/VmJava.sa`, `src/vm/VmJavaLib.sa`).
- **Internal names** start with `$` (`$wrap`, `$In`, `$StdIn`): members and
  helper types the front-end and VM use, which no Java program can name. A `$`
  helper type lives in the file of the type it serves (`$In` in `System.sa`).
- One type per file, except those `$` helpers. Several public types may share a
  file only where noted.

## Packages

| Package | Files | Notes |
| --- | --- | --- |
| `java.lang` | 61 | `Object`, `String`, `StringBuilder`, the boxes, `Math`, `System`, `Thread`, `Enum`, `Record`, and the exceptions and errors. An exception the VM raises itself (division by zero, a bad index, a failed cast) reaches a `catch` as a descriptor string `"\u001fJ<class>\u001f<message>\u001f<frames>"`, which `Throwable.$wrap` turns into the exception object. |
| `java.io` | 23 | Console streams, readers and writers. `System.in`, `System.out` and `System.err` are the process's standard streams; buffered writers keep their text until flushed, as on a JVM. |
| `java.util` | 67 | Collection interfaces and their skeletal `Abstract*` implementations; `ArrayList`, `LinkedList`, `ArrayDeque`, `PriorityQueue`, `Vector` / `Stack`; `HashMap`, `LinkedHashMap`, `HashSet`, `LinkedHashSet` follow the JDK's implementation closely (same hashing, table sizes, resize splitting and insertion points), so iteration order matches a real JVM; `TreeMap` / `TreeSet` are red-black trees, with sub-map / sub-set views (`headMap`, `tailSet`, `descendingMap`, ...) as copies; `EnumMap` / `EnumSet` iterate in ordinal order (`EnumSet.allOf` / `range` / `complementOf` are not provided: a `Class` object cannot list an enum's constants here); `Arrays` (primitive arrays sorted, filled, copied and printed by VM natives), `Collections` and the immutable `List.of` / `Set.of` / `Map.of`; `Objects`, `Optional*`, `Random`, `Scanner`, `StringTokenizer`, `StringJoiner`, `Locale`, the summary statistics, `BitSet`, and the exceptions of `java.util`. |
| `java.util.function` | 43 | The functional interfaces, with their default and static methods. |
| `java.util.stream` | 13 | Sequential, pull-based pipelines: each stage is an iterator over the previous one, so elements flow through one at a time in the same order as on a JVM (`sorted()` drains its input first). `Collectors`. |
| `java.util.regex` | 4 | `Pattern` / `Matcher` over the VM's regular-expression engine: ECMAScript syntax with Java's `\p{...}` classes; inline flags other than a leading `(?i)`, and named groups, are not supported. |
| `java.util.concurrent` | 11 | For a single-threaded VM: the concurrent collections behave like their plain counterparts. |
| `java.util.concurrent.atomic` | 6 | Atomics as simple holders (single-threaded VM). |

## Adding a class

1. Create `<package folder>/<Type>.sa` (`util/BitSet.sa`; a new subpackage gets
   its folder under its parent's) starting with its `package` line and the
   imports it needs.
2. Write it in the Java the front-end accepts; declare VM-implemented methods
   `native` and register `__jn_<Type>_<method>` in `src/vm/VmJavaLib.sa`.
3. Run the embed tool and rebuild, then `python conformance/run.py java`.
