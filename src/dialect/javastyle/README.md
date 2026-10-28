# Java-style library

What Java-style code in a `.sa` file calls (`System.out.println`, `new ArrayList<>()`, `Collectors`, ...).
`src/dialect/JavaDialect.sa` rewrites the Java syntax; this library supplies the classes. It is Quantum
source, embedded into the compiler (`src/tools/embed.sa` → `src/generated/JavaStyleData.sa`), and
`compileSource()` (`src/Pipeline.sa`) puts it in front of a program that carries the
`// quantum: java-style` marker, files in name order.

| File | Contents |
| --- | --- |
| `Lang.sa` | Helpers the rewritten code calls (`__jit`, `__jarr`, `__jinstanceof`, `__jexc`, ...), `String.format`, `System`, the box classes (`Integer`, `Double`, ...), `Math` additions, `Objects`, the exception classes |
| `Util.sa` | `Optional`, `Comparator`, collections, `Arrays`, `Collections`, streams and `Collectors`, `StringBuilder`, `Scanner`, `BufferedReader`, `Random` (Java's generator: the same seed gives the same numbers) |

How Java values are represented: lists, stacks and vectors are Quantum arrays (their Java methods —
`get`, `add`, `peek`, `stream`, ... — come from `src/vm/VmJavaStyle.sa`); `LinkedList`/`ArrayDeque` are
`__JDeque`; maps and sets are `__JMap`/`__JSet` (sorted for `Tree*`, keys keep their type);
`PriorityQueue` is a binary heap with `java.util`'s ordering; a stream is an array. `List.of`,
`Set.of` and `Map.of` are rewritten to `__jList_of`, ... so JavaScript's `Set`/`Map`/`Object` in the
same file keep working.

A library name must not be a Quantum keyword (`from`, `def`, `is`, `as`, `hash`, ...) — check a change with
`quantum --check`, then run the embed tool from `compiler/` and rebuild.
