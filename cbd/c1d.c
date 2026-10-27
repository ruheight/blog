/*
 * c1d.c — decompiler for c1-generated x86-64 binaries
 *
 * Pipeline: x86-64 binary → AST (pattern-match c1's fixed codegen)
 *
 * Build:  cpp c1d.c | ./c0 | ./c1_host > tmp/c1d.bin
 *    or:  cpp c1d.c | ./c0 | ./run tmp/c1.bin > tmp/c1d.bin
 * Use:    ./run tmp/c1d.bin < input.bin > output.ast
 */
long _bss_end;

#include "c1_decompile.c"

long main(long in, long in_len, long out)
{
	_bss_end = 0;
	return decompile(in, in_len, out);
}
