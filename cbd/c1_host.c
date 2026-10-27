/*
 * c1_host.c — host-compiled c1 for bootstrap
 *
 * Uses stdio instead of the emulator's memory-mapped I/O so c1
 * can run natively on the host during initial bootstrap.
 *
 * Reads serialised AST from stdin, writes x86-64 binary to stdout.
 *
 * Usage: cpp source.c | ./c0 | ./c1_host > code.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c1_compile.c"

unsigned char ast_buf[1 << 23]; /* 8MB input */
unsigned char out_buf[1 << 20]; /* 1MB output */

int main(void)
{
	long in_len, out_len;

	in_len = fread(ast_buf, 1, sizeof(ast_buf), stdin);
	if (in_len <= 0 || in_len % 32 != 0) { /* each node is 4 longs = 32 bytes */
		fprintf(stderr, "c1: bad AST input (%ld bytes)\n", in_len);
		return 1;
	}
	fprintf(stderr, "c1: %ld AST nodes\n", in_len / 32);

	out_len = compile((long)ast_buf, in_len, (long)out_buf);

	fwrite(out_buf, 1, out_len, stdout);
	fprintf(stderr, "c1: %ld bytes of code\n", out_len);
	return 0;
}
