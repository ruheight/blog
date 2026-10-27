/*
 * c1.c — single-function all-spill compiler (rax/rcx only)
 *
 * Compiles serialised AST to x86-64. Simplest backend: every value
 * is spilled to the stack, only rax and rcx used as scratch.
 *
 * Build:  cpp c1.c | ./c0 | ./c1_host > tmp/c1.bin
 * Use:    cpp source.c | ./c0 | ./run tmp/c1.bin > tmp/out.bin
 */
long _bss_end; /* emulator BSS watermark — reset to 0 each invocation */

#include "c1_compile.c"

long main(long in, long in_len, long out)
{
	_bss_end = 0;
	return compile(in, in_len, out);
}
