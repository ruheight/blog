/*
 * c1m.c — c1 metamorphic engine
 *
 * decompile → mutate → compile: produces a byte-different but
 * semantically identical binary.  Uses c1's all-spill backend.
 *
 * Seed from hash of first 1024 input bytes (xorshift64 PRNG).
 * No MUT_REGALLOC (c1 has no register allocator to shuffle).
 *
 * Build: cpp -DMUT_LOCAL -DMUT_GLOBAL -DMUT_COMMUTE -DMUT_REASSOC \
 *            -DMUT_IF_INVERT c1m.c | ./c0 | ./run tmp/c1.bin > tmp/c1m.bin
 * Use:   ./run tmp/c1m.bin < input.bin > output.bin
 * Self:  ./run tmp/c1m.bin < tmp/c1m.bin > tmp/c1m_gen1.bin
 */
long _bss_end;

#include "cc_lib.c"
#define C1M_SHARED /* share globals between c1_decompile and c1_compile */
#include "c1_decompile.c"
#include "c1_compile.c"
#include "cc_mutate.c"

long tmp_buf[1048576]; /* 1M longs = 8MB, holds AST between decompile and compile */

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
	return compile((long)tmp_buf, ast_len, out);
}
