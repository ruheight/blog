/*
 * Execute generated x86-64 code through the emulator.
 *
 * Binary format: offset 0: CALL main; HLT
 * main(in, len, out) returns out_len in rax.
 *
 * Usage:
 *   run code.bin [-T]
 *
 * stdin  → in buffer at IN_BASE (0x6000000)
 * stdout ← out buffer at OUT_BASE (0x8000000), length from rax
 *
 * Memory is zero-filled. No .dat files — arrays get compile-time addresses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "x86emu.c"

/* Memory layout (must match compiler / x86emu.c) */
#define IN_BASE      0x6000000
#define OUT_BASE     0x8000000

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t code[1 << 20];
	int code_len;
	x86emu_t e;
	int insn_trace = 0;
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <code.bin> [-T]\n", argv[0]);
		return 1;
	}

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-T")) insn_trace = 1;
	}

	/* Load code */
	f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	code_len = fread(code, 1, sizeof(code), f);
	fclose(f);

	fprintf(stderr, "code: %d bytes\n", code_len);

	/* Init emulator */
	x86emu_init(&e, code, code_len);
	e.trace = insn_trace;

	/* Read stdin → in buffer */
	int in_len_val = 0;
	if (!isatty(fileno(stdin))) {
		in_len_val = fread(e.mem + IN_BASE, 1, 32 << 20, stdin);
	}

	/* Pass in/in_len/out via registers */
	e.regs[RDI] = IN_BASE;
	e.regs[RSI] = in_len_val;
	e.regs[RDX] = OUT_BASE;

	fprintf(stderr, "in=%d bytes\n", in_len_val);

	/* Run */
	long long steps = x86emu_run(&e);
	fprintf(stderr, "executed %lld steps, rsp=0x%llx\n",
		steps, (unsigned long long)e.regs[RSP]);

	/* halted==2 = normal HLT (rax holds output length).
	 * halted==1 = fault/error (rax is stale, suppress output). */
	if (e.halted == 1) {
		fprintf(stderr, "HALTED (fault)\n");
		free(e.mem);
		return 1;
	}
	{
		uint64_t emu_len = e.regs[RAX];
		if (emu_len > 0 && emu_len < (32 << 20)) {
			fwrite(e.mem + OUT_BASE, 1, (size_t)emu_len, stdout);
			fprintf(stderr, "output: %llu bytes\n",
				(unsigned long long)emu_len);
		} else {
			fprintf(stderr, "output: 0 bytes\n");
		}
	}
	free(e.mem);
	return 0;
}
