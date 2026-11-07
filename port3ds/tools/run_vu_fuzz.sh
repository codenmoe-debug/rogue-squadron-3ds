#!/bin/bash
# run_vu_fuzz.sh OUTDIR: build rsp_vu_fuzz.cpp with and without the 3DS fast paths and compare (run from the repo root)
set -e
OUT="$1"
R=lib/N64ModernRuntime
CXX=${CXX:-/opt/homebrew/opt/llvm/bin/clang++}
INC=(-DRSP_FORCE_SISD -I"$R/librecomp/include" -I"$R/librecomp/include/librecomp" -I"$R/ultramodern/include" -I"$R/N64Recomp/include" -Iinclude)
"$CXX" -std=c++20 -O2 "${INC[@]}" port3ds/tools/rsp_vu_fuzz.cpp -o "$OUT/vu_ref2"
"$CXX" -std=c++20 -O2 -DRSP_VU_FAST32 "${INC[@]}" port3ds/tools/rsp_vu_fuzz.cpp -o "$OUT/vu_fast2"
"$OUT/vu_ref2" > "$OUT/ref2.txt"
"$OUT/vu_fast2" > "$OUT/fast2.txt"
paste "$OUT/ref2.txt" "$OUT/fast2.txt" | awk '{print $1, $2, ($3 == $6 ? "match" : "MISMATCH")}'
if cmp -s "$OUT/ref2.txt" "$OUT/fast2.txt"; then echo "ALL MATCH"; else echo "DIFFERENCES FOUND"; fi
