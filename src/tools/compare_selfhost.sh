#!/usr/bin/env bash
# Runs a bootstrap subsystem compiled by Quantum's own C/C++ front-end
# against qrun's built-in passes, on every example:
#   src/tools/compare_selfhost.sh <tokens|ast|qbc|full> [dir] [outdir]
# tokens: qrun --dump-tokens        vs  qrun src/tools/selfhost/lexdump.sa
# ast:    qrun --dump-ast           vs  qrun src/tools/selfhost/astdump.sa
# qbc:    qrun --emit-qbc-core f o  vs  qrun src/tools/selfhost/qbc.sa f o
# full:   qrun --emit-qbc f o       vs  the whole compiler (src/tools/selfhost/quantumc.sa),
#         compiled once to build/quantumc.qbc and run with qrun --exec
#         (qbc, full: the bytecode files and any output must both match)
# Mismatching files are listed in <outdir>/<mode>_mismatch.txt, with both
# outputs kept as <outdir>/<mode>/<n>.gpp.txt and <n>.q.txt (qbc: also .qbc).
# QRUN=path overrides the qrun used. NATIVE=exe (tokens, ast) runs that
# executable instead of the driver on qrun: the driver compiled by
#   qrun --native src/tools/selfhost/<driver>.sa <exe>
cd "$(dirname "$0")/../.."
MODE=${1:?usage: compare_selfhost.sh <tokens|ast|qbc|full> [dir] [outdir]}
DIR=${2:-examples}
OUT=${3:-build/selfhost-compare}
QRUN=${QRUN:-build/qrun.exe}
case "$MODE" in
    tokens) REF=--dump-tokens; DRIVER=src/tools/selfhost/lexdump.sa ;;
    ast) REF=--dump-ast; DRIVER=src/tools/selfhost/astdump.sa ;;
    qbc) REF=--emit-qbc-core; DRIVER=src/tools/selfhost/qbc.sa ;;
    full) REF=--emit-qbc; DRIVER="--exec build/quantumc.qbc"
          "$QRUN" --emit-qbc src/tools/selfhost/quantumc.sa build/quantumc.qbc || exit 1 ;;
    *) echo "unknown mode: $MODE" >&2; exit 2 ;;
esac
mkdir -p "$OUT/$MODE"
: > "$OUT/${MODE}_mismatch.txt"
ok=0
n=0
while IFS= read -r f; do
    n=$((n + 1))
    base="$OUT/$MODE/$n"
    if [ "$MODE" = qbc ] || [ "$MODE" = full ]; then
        "$QRUN" $REF "$f" "$base.gpp.qbc" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' > "$base.gpp.txt"
        timeout 300 "$QRUN" $DRIVER "$f" "$base.q.qbc" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' > "$base.q.txt"
        same=0
        cmp -s "$base.gpp.txt" "$base.q.txt" &&
            { [ ! -e "$base.gpp.qbc" ] && [ ! -e "$base.q.qbc" ] || cmp -s "$base.gpp.qbc" "$base.q.qbc"; } && same=1
    else
        "$QRUN" $REF "$f" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' > "$base.gpp.txt"
        if [ -n "$NATIVE" ]; then
            timeout 300 "$NATIVE" "$f" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' > "$base.q.txt"
        else
            timeout 300 "$QRUN" "$DRIVER" "$f" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' > "$base.q.txt"
        fi
        same=0
        cmp -s "$base.gpp.txt" "$base.q.txt" && same=1
    fi
    if [ $same = 1 ]; then
        ok=$((ok + 1))
        rm -f "$base".*
    else
        echo "$n	$f" >> "$OUT/${MODE}_mismatch.txt"
    fi
done < <(find "$DIR" -type f \( -name '*.sa' -o -name '*.py' -o -name '*.js' -o -name '*.rb' -o -name '*.c' -o -name '*.cpp' \) | sort)
echo "$MODE match: $ok / $n"
