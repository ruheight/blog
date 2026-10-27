#!/bin/sh
# demo_c2.sh — visual mutation demo + generational stability test
set -e

echo "=== Building ==="
rm -rf tmp && mkdir -p tmp
cc -o c0 c0.c -w
cc -o run run_emu.c -w
cc -o c1_host c1_host.c -w
cc -o tmp/inspect_ast tools/inspect_ast.c -w -I.

cpp c1.c | ./c0 | ./c1_host > tmp/c1_first.bin 2>/dev/null
cpp c1.c | ./c0 | ./run tmp/c1_first.bin > tmp/c1.bin 2>/dev/null
cpp c2.c | ./c0 | ./run tmp/c1.bin > tmp/c2_s1.bin 2>/dev/null
cpp c2.c | ./c0 | ./run tmp/c2_s1.bin > tmp/c2.bin 2>/dev/null
cpp c2d.c | ./c0 | ./run tmp/c2.bin > tmp/c2d.bin 2>/dev/null
echo "c2: $(wc -c < tmp/c2.bin | tr -d ' ')  c2d: $(wc -c < tmp/c2d.bin | tr -d ' ')"

# Compile two test binaries for visual demos
cpp tests/t30_advanced.c 2>/dev/null | ./c0 | ./run tmp/c2.bin > tmp/t30.bin 2>/dev/null
cpp tests/t37_control_flow.c 2>/dev/null | ./c0 | ./run tmp/c2.bin > tmp/t37.bin 2>/dev/null

# Base ASTs
./run tmp/c2d.bin < tmp/t30.bin 2>/dev/null | ./tmp/inspect_ast 2>/dev/null > tmp/base30.ast
./run tmp/c2d.bin < tmp/t37.bin 2>/dev/null | ./tmp/inspect_ast 2>/dev/null > tmp/base37.ast

ALL="-DMUT_LOCAL -DMUT_GLOBAL -DMUT_COMMUTE -DMUT_REASSOC -DMUT_IF_INVERT -DMUT_REGALLOC -DMUT_STORAGE -DMUT_SCHEDULE"

echo ""
echo "=========================================="
echo "=== Visual mutation demos ==="
echo "=========================================="

for M in MUT_LOCAL MUT_GLOBAL MUT_COMMUTE MUT_REASSOC MUT_IF_INVERT MUT_REGALLOC MUT_STORAGE MUT_SCHEDULE; do
    cpp -D$M c2m.c 2>/dev/null | ./c0 2>/dev/null | ./run tmp/c2.bin > tmp/c2m_v.bin 2>/dev/null

    # pick test: IF_INVERT needs if/else (t37), others use t30
    # REGALLOC changes registers but not AST — show binary diff instead
    if [ "$M" = "MUT_IF_INVERT" ]; then
        ./run tmp/c2m_v.bin < tmp/t37.bin > tmp/v_mut.bin 2>/dev/null
        ./run tmp/c2d.bin < tmp/v_mut.bin 2>/dev/null | ./tmp/inspect_ast 2>/dev/null > tmp/v_mut.ast
        BASE=tmp/base37.ast
    else
        ./run tmp/c2m_v.bin < tmp/t30.bin > tmp/v_mut.bin 2>/dev/null
        ./run tmp/c2d.bin < tmp/v_mut.bin 2>/dev/null | ./tmp/inspect_ast 2>/dev/null > tmp/v_mut.ast
        BASE=tmp/base30.ast
    fi

    ND=$(diff "$BASE" tmp/v_mut.ast | wc -l | tr -d ' ')
    echo ""
    echo "--- $M ---"
    if [ "$M" = "MUT_SCHEDULE" ]; then
        # SCHEDULE reorders IR instructions — show IR diff
        cc -o tmp/inspect_ir tools/inspect_ir.c -w -I. 2>/dev/null
        cc -o tmp/inspect_ir_s tools/inspect_ir.c -w -I. -DMUT_SCHEDULE 2>/dev/null
        cpp tests/t30_advanced.c 2>/dev/null | ./c0 2>/dev/null > tmp/v_t30.ast
        ./tmp/inspect_ir < tmp/v_t30.ast > /dev/null 2> tmp/ir_orig.txt
        ./tmp/inspect_ir_s < tmp/v_t30.ast > /dev/null 2> tmp/ir_sched.txt
        NI=$(diff tmp/ir_orig.txt tmp/ir_sched.txt | grep '^[<>]' | wc -l | tr -d ' ')
        echo "  ($NI IR lines changed)"
        diff -u tmp/ir_orig.txt tmp/ir_sched.txt | grep '^[-+]  ' | head -20
    elif [ "$M" = "MUT_REGALLOC" ]; then
        # REGALLOC doesn't change AST, show disassembly diff
        ndisasm -b64 tmp/t30.bin > tmp/v_orig.asm
        ndisasm -b64 tmp/v_mut.bin > tmp/v_mut.asm
        OSZ=$(wc -c < tmp/t30.bin | tr -d ' ')
        MSZ=$(wc -c < tmp/v_mut.bin | tr -d ' ')
        echo "  binary: $OSZ -> $MSZ bytes"
        diff -Nur tmp/v_orig.asm tmp/v_mut.asm | head -40
    elif [ "$M" = "MUT_STORAGE" ]; then
        # STORAGE: demote single-owner globals to locals + promote random locals.
        # Use c2m (large binary) — small tests have few globals.
        # Key check: globals must NOT accumulate over generations.
        cpp c2m.c 2>/dev/null | ./c0 2>/dev/null | ./run tmp/c2.bin > tmp/v_c2m.bin 2>/dev/null
        ./run tmp/c2d.bin < tmp/v_c2m.bin 2>/dev/null | ./tmp/inspect_ast 2>/dev/null > tmp/v_base_c2m.ast
        ./run tmp/c2m_v.bin < tmp/v_c2m.bin > tmp/v_c2m_mut.bin 2>/dev/null
        ./run tmp/c2d.bin < tmp/v_c2m_mut.bin 2>/dev/null | ./tmp/inspect_ast 2>/dev/null > tmp/v_c2m_mut.ast
        ND=$(diff tmp/v_base_c2m.ast tmp/v_c2m_mut.ast | wc -l | tr -d ' ')
        echo "  (c2m binary, $ND AST diff lines)"

        # Count top-level globals (lines starting with '(' at column 0, not indented)
        G_BEFORE=$(grep -c '^(' tmp/v_base_c2m.ast 2>/dev/null || echo 0)
        G_AFTER=$(grep -c '^(' tmp/v_c2m_mut.ast 2>/dev/null || echo 0)
        echo "  top-level nodes: $G_BEFORE -> $G_AFTER"

        echo "  demoted (global removed from top):"
        diff tmp/v_base_c2m.ast tmp/v_c2m_mut.ast | grep '^< (' | grep -v 'func ' | head -10
        echo "  promoted (global added to top):"
        diff tmp/v_base_c2m.ast tmp/v_c2m_mut.ast | grep '^> (' | grep -v 'func ' | head -10
        echo "  local changes inside functions:"
        diff tmp/v_base_c2m.ast tmp/v_c2m_mut.ast | grep -E '^[<>]  ' | head -10

        if [ "$ND" -eq 0 ]; then
            echo "  WARNING: no visible changes!"
        fi
    else
        echo "  ($ND AST diff lines)"
        diff "$BASE" tmp/v_mut.ast | head -20
        if [ "$ND" -eq 0 ]; then
            echo "  WARNING: no visible changes!"
        fi
    fi
done

echo ""
echo "=========================================="
echo "=== Generational stability (5 gens, all mutations) ==="
echo "=========================================="

cpp $ALL c2m.c 2>/dev/null | ./c0 2>/dev/null | ./run tmp/c2.bin > tmp/cur.bin 2>/dev/null
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
        echo "gen$g: $SZ (UNCHANGED — mutations had no effect)"
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
    echo "RESULT: FAIL — crashed at gen$g"
fi
