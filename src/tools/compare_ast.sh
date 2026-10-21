#!/usr/bin/env bash
# Compares the self-hosted parser with the C++ parser on every example:
#   src/tools/compare_ast.sh [dir] [outdir]
# Mismatching files are listed in <outdir>/ast_mismatch.txt, with both dumps
# kept as <outdir>/ast/<n>.cpp.txt and <n>.sa.txt.
cd "$(dirname "$0")/../.."
DIR=${1:-examples}
OUT=${2:-build/selfhost-compare}
QRUN=build/qrun.exe
mkdir -p "$OUT/ast"
: > "$OUT/ast_mismatch.txt"
ok=0
n=0
while IFS= read -r f; do
    n=$((n + 1))
    "$QRUN" --dump-ast "$f" 2>&1 | tr -d '\r' > "$OUT/ast/$n.cpp.txt"
    timeout 300 "$QRUN" src/tools/astdump.sa "$f" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' > "$OUT/ast/$n.sa.txt"
    if cmp -s "$OUT/ast/$n.cpp.txt" "$OUT/ast/$n.sa.txt"; then
        ok=$((ok + 1))
        rm -f "$OUT/ast/$n.cpp.txt" "$OUT/ast/$n.sa.txt"
    else
        echo "$n	$f" >> "$OUT/ast_mismatch.txt"
    fi
done < <(find "$DIR" -type f \( -name '*.sa' -o -name '*.py' -o -name '*.js' -o -name '*.rb' -o -name '*.c' -o -name '*.cpp' \) | sort)
echo "AST match: $ok / $n"
