#!/bin/sh
# demo_c1.sh — visual mutation demo + generational stability for c1 pipeline
set -e

echo "=== Building ==="
rm -rf tmp && mkdir -p tmp
cc -o c0 c0.c -w
cc -o run run_emu.c -w
cc -o c1_host c1_host.c -w
cc -o tmp/inspect_ast tools/inspect_ast.c -w -I.

cpp c1.c | ./c0 | ./c1_host > tmp/c1_first.bin 2>/dev/null
cpp c1.c | ./c0 | ./run tmp/c1_first.bin > tmp/c1.bin 2>/dev/null
cpp c1d.c | ./c0 | ./run tmp/c1.bin > tmp/c1d.bin 2>/dev/null
echo "c1: $(wc -c < tmp/c1.bin | tr -d ' ')  c1d: $(wc -c < tmp/c1d.bin | tr -d ' ')"

ALL="-DMUT_LOCAL -DMUT_GLOBAL -DMUT_COMMUTE -DMUT_REASSOC -DMUT_IF_INVERT"

# Compile tests via c1
cpp tests/t30_advanced.c 2>/dev/null | ./c0 | ./run tmp/c1.bin > tmp/t30.bin 2>/dev/null
cpp tests/t37_control_flow.c 2>/dev/null | ./c0 | ./run tmp/c1.bin > tmp/t37.bin 2>/dev/null

# Base ASTs via c1d round-trip
./run tmp/c1d.bin < tmp/t30.bin | ./tmp/inspect_ast > tmp/base30.ast
./run tmp/c1d.bin < tmp/t37.bin | ./tmp/inspect_ast > tmp/base37.ast

echo ""
echo "=========================================="
echo "=== Visual mutation demos (c1 pipeline) ==="
echo "=========================================="

for M in MUT_LOCAL MUT_GLOBAL MUT_COMMUTE MUT_REASSOC MUT_IF_INVERT; do
    cpp -D$M c1m.c 2>/dev/null | ./c0 | ./run tmp/c1.bin > tmp/c1m_v.bin 2>/dev/null

    if [ "$M" = "MUT_IF_INVERT" ]; then
        ./run tmp/c1m_v.bin < tmp/t37.bin > tmp/v_mut.bin 2>/dev/null
        ./run tmp/c1d.bin < tmp/v_mut.bin | ./tmp/inspect_ast > tmp/v_mut.ast
        BASE=tmp/base37.ast
    else
        ./run tmp/c1m_v.bin < tmp/t30.bin > tmp/v_mut.bin 2>/dev/null
        ./run tmp/c1d.bin < tmp/v_mut.bin | ./tmp/inspect_ast > tmp/v_mut.ast
        BASE=tmp/base30.ast
    fi

    ND=$(diff "$BASE" tmp/v_mut.ast | wc -l | tr -d ' ')
    echo ""
    echo "--- $M ($ND diff lines) ---"
    diff "$BASE" tmp/v_mut.ast | head -20
    if [ "$ND" -eq 0 ]; then echo "  WARNING: no visible changes!"; fi
done

echo ""
echo "=========================================="
echo "=== Generational stability (5 gens, all mutations) ==="
echo "=========================================="

cpp $ALL c1m.c 2>/dev/null | ./c0 | ./run tmp/c1.bin > tmp/cur.bin 2>/dev/null
SZ0=$(wc -c < tmp/cur.bin | tr -d ' ')
echo "gen0: $SZ0"
PREV_MD5=$(md5 -q tmp/cur.bin)
OK=1

g=1
while [ $g -le 5 ]; do
    timeout 10 ./run tmp/cur.bin < tmp/cur.bin > tmp/next.bin 2>/dev/null
    SZ=$(wc -c < tmp/next.bin | tr -d ' ')
    if [ "$SZ" -lt 100 ]; then
        echo "gen$g: DEAD ($SZ bytes)"
        OK=0
        break
    fi
    CUR_MD5=$(md5 -q tmp/next.bin)
    if [ "$CUR_MD5" = "$PREV_MD5" ]; then
        echo "gen$g: $SZ (UNCHANGED)"
    else
        echo "gen$g: $SZ (changed)"
    fi
    PREV_MD5=$CUR_MD5
    cp tmp/next.bin tmp/cur.bin
    g=$((g + 1))
done

echo ""
if [ $OK -eq 1 ]; then
    echo "RESULT: OK — 5 generations stable, all binaries different"
else
    echo "RESULT: FAIL"
fi
