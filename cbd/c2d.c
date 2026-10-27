/*
 * c2d.c — decompiler for c2-generated x86-64 binaries
 *
 * Pipeline: x86-64 binary → IR (c2_decode) → AST (c2_decompile)
 *
 * Usage: ./run c2d.bin < code.bin > ast.bin
 */

#include "ast.h"
#include "cc_lib.c"

#include "c2_decode.c"     /* x86-64 → IR */
#include "c2_decompile.c" /* IR → AST */

long _bss_end;

long main(long in, long in_len, long out)
{
	_bss_end = 0;
	return decompile(in, in_len, out);
}
