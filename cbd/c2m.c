/*
 * c2m.c — Meta-C metamorphic engine (c2 pipeline)
 *
 * decompile(c2d) → AST → mutate → compile(c2) → binary
 *
 * Seed from hash of first 1024 input bytes. xorshift64 PRNG.
 * Output is byte-different but semantically identical.
 *
 * Build: cpp -DMUT_LOCAL -DMUT_GLOBAL -DMUT_COMMUTE -DMUT_REASSOC \
 *            -DMUT_IF_INVERT -DMUT_REGALLOC c2m.c | ./c0 | ./run tmp/c2.bin > tmp/c2m.bin
 */

#include "ast.h"
#include "cc_lib.c"

#include "c2_decode.c"     /* x86-64 → IR */
#include "c2_decompile.c" /* IR → AST */

/* C2M_SHARED_IR: reuse ir[]/nir/next_vreg arrays from c2_decode */
#define C2M_SHARED_IR
#include "c2_compile.c"

#include "cc_mutate.c" /* AST-level semantic-preserving mutations */

long _bss_end;
#define TMP_BUF_LONGS 131072 /* 131072 longs = 1M bytes ÷ 32 = 32K nodes, peak ~22K */
long tmp_buf[TMP_BUF_LONGS]; /* intermediate AST buffer between decompile and compile */

/* deterministic seed from input bytes so the same binary always mutates the same way */
long hash_input(long in, long in_len)
{
	long h, i, n;
	unsigned char *p;
	p = (unsigned char *)in;
	n = in_len;
	if (n > 1024)
		n = 1024;
	h = 0;
	for (i = 0; i < n; i++)
		h = h * 31 + (long)p[i];
	return h;
}

long main(long in, long in_len, long out)
{
	long ast_len, seed;
	_bss_end = 0;

	seed = hash_input(in, in_len);
	srng(seed);
	ast_len = decompile(in, in_len, (long)tmp_buf);
	ast_len = mutate((long)tmp_buf, ast_len);
	/* rng_state carries through: mutate randomises AST, then compile
	   uses the continued PRNG stream for register shuffling */
	return compile((long)tmp_buf, ast_len, out);
}
