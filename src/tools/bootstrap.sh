#!/usr/bin/env bash
# The self-compile fixed point: the whole compiler (src/tools/selfhost/quantumc.sa)
# compiled by itself, generation after generation, must not change.
#   src/tools/bootstrap.sh [outdir]
# q1: build/qrun.exe (from build-quantum.bat) compiles the compiler
# q2: q1, run on qrun, compiles the compiler
# q3: q2 compiles the compiler
# Passes when q1 == q2 == q3 byte for byte. Each self-compile runs on the VM
# and takes a while (minutes). QRUN=path overrides the qrun used; give it a
# copy of qrun.exe so rebuilding qrun meanwhile can still link.
cd "$(dirname "$0")/../.."
OUT=${1:-build/bootstrap}
QRUN=${QRUN:-build/qrun.exe}
SRC=src/tools/selfhost/quantumc.sa
mkdir -p "$OUT"
echo "q1: $QRUN --emit-qbc"
"$QRUN" --emit-qbc "$SRC" "$OUT/q1.qbc" || exit 1
for g in 2 3; do
    prev=$((g - 1))
    echo "q$g: compiled by q$prev"
    start=$(date +%s)
    "$QRUN" --exec "$OUT/q$prev.qbc" "$SRC" "$OUT/q$g.qbc" || exit 1
    echo "    $(( $(date +%s) - start ))s"
    if ! cmp "$OUT/q$prev.qbc" "$OUT/q$g.qbc"; then
        echo "FIXED POINT FAILED: q$prev != q$g"
        exit 1
    fi
done
echo "fixed point: q1 == q2 == q3 ($(wc -c < "$OUT/q1.qbc") bytes)"
# The self-hosted compiler as one program: q2 bundled into the VM stub
# (trailer: payload, its size as uint32 LE, "QNTM_VM!"), like `quantum` does.
STUB=$(dirname "$QRUN")/quantum_stub.exe
if [ -f "$STUB" ]; then
    size=$(wc -c < "$OUT/q2.qbc")
    { cat "$STUB" "$OUT/q2.qbc"
      printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((size & 255)) $((size >> 8 & 255)) \
          $((size >> 16 & 255)) $((size >> 24 & 255)))"
      printf 'QNTM_VM!'; } > "$OUT/quantum-sh.exe"
    echo "self-hosted compiler: $OUT/quantum-sh.exe <file> <out.qbc>"
fi
