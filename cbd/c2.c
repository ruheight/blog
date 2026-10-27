/*
 * c2.c — optimizing compiler with 8-register linear-scan regalloc
 *
 * AST → IR → register allocation → x86-64.  Uses all 8 GPRs
 * (rax, rcx, rdx, rbx, rsi, rdi, r8, r9) with spill/reload.
 *
 * Build: cpp c2.c | ./c0 | ./run c1.bin > c2.bin     (bootstrap via c1)
 *        cpp c2.c | ./c0 | ./run c2.bin > c2_gen1.bin (self-compile)
 */
long _bss_end;

#include "cc_lib.c"    /* shared helpers: memcpy, PRNG, global variable table */
#include "c2_compile.c"

long main(long in, long in_len, long out)
{
	_bss_end = 0;
	/* rng_state = 0 from BSS: regalloc deterministic (no MUT_REGALLOC) */
	return compile(in, in_len, out);
}
