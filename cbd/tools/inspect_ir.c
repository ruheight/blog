/*
 * inspect_ir.c — compile AST via c2 and dump IR to stderr
 *
 * gcc -Wall -Wextra -o inspect_ir inspect_ir.c
 * cpp source.c | ./c0 | ./inspect_ir > code.bin 2>ir_dump.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_IR
#ifdef MUT_SCHEDULE
#include "cc_lib.c"
#endif
#include "c2_compile.c"

static const char *ir_opname(long op)
{
	switch (op) {
	case IR_LOAD_IMM: return "load_imm";
	case IR_LOAD_LOC: return "load_loc";
	case IR_STORE_LOC: return "store_loc";
	case IR_ADDR_LOC: return "addr_loc";
	case IR_LOAD_GLOB: return "load_glob";
	case IR_STORE_GLOB: return "store_glob";
	case IR_ADDR_GLOB: return "addr_glob";
	case IR_DEREF_LD: return "deref_ld";
	case IR_DEREF_ST: return "deref_st";
	case IR_ADD: return "add";
	case IR_SUB: return "sub";
	case IR_MUL: return "mul";
	case IR_DIVS: return "divs";
	case IR_DIVU: return "divu";
	case IR_MODS: return "mods";
	case IR_MODU: return "modu";
	case IR_SHL: return "shl";
	case IR_SHRS: return "shrs";
	case IR_SHRU: return "shru";
	case IR_AND: return "and";
	case IR_OR: return "or";
	case IR_XOR: return "xor";
	case IR_NEG: return "neg";
	case IR_NOT: return "not";
	case IR_EQ: return "eq";
	case IR_NE: return "ne";
	case IR_LTS: return "lts";
	case IR_LTU: return "ltu";
	case IR_GTS: return "gts";
	case IR_GTU: return "gtu";
	case IR_LES: return "les";
	case IR_LEU: return "leu";
	case IR_GES: return "ges";
	case IR_GEU: return "geu";
	case IR_LNOT: return "lnot";
	case IR_SETNE: return "setne";
	case IR_CLR: return "clr";
	case IR_JMP: return "jmp";
	case IR_JZ: return "jz";
	case IR_JNZ: return "jnz";
	case IR_RET: return "ret";
	case IR_RET_VOID: return "ret_void";
	case IR_CALL: return "call";
	case IR_CALL_IND: return "call_ind";
	case IR_ARG: return "arg";
	case IR_FUNC: return "func";
	case IR_PARAM: return "param";
	case IR_NOP: return "nop";
	case IR_ADDR_FUNC: return "addr_func";
	default: return "???";
	}
}

/* width suffix: 1=.b 2=.w 4=.d 8/.q (omit for default qword) */
static const char *wsuffix(long w)
{
	if (w == 1) return ".b";
	if (w == 2) return ".w";
	if (w == 4) return ".d";
	return "";
}

void dump_ir_func(void)
{
	long i, op;
	char name_buf[32];

	for (i = 0; i < nir; i++) {
		op = ir[i].op;
		/* for load/store loc/glob, append width suffix from imm */
		if ((op == IR_LOAD_LOC || op == IR_STORE_LOC ||
		     op == IR_LOAD_GLOB || op == IR_STORE_GLOB) &&
		    ir[i].imm > 0 && ir[i].imm <= 8) {
			snprintf(name_buf, sizeof(name_buf), "%s%s",
				ir_opname(op), wsuffix(ir[i].imm));
			fprintf(stderr, "  %4ld: %-14s dst=%-4ld s1=%-8ld s2=%-8ld\n",
				i, name_buf, ir[i].dst, ir[i].s1, ir[i].s2);
		} else if (ir[i].imm != 0) {
			fprintf(stderr, "  %4ld: %-14s dst=%-4ld s1=%-8ld imm=%-8ld\n",
				i, ir_opname(ir[i].op), ir[i].dst, ir[i].s1, ir[i].imm);
		} else {
			fprintf(stderr, "  %4ld: %-14s dst=%-4ld s1=%-8ld s2=%-8ld\n",
				i, ir_opname(ir[i].op), ir[i].dst, ir[i].s1, ir[i].s2);
		}
	}
	fprintf(stderr, "  (%ld insns, %ld vregs)\n\n", nir, next_vreg);
}

unsigned char ast_buf[1 << 23];
unsigned char out_buf[1 << 22];

int main(void)
{
	long in_len, out_len;

	in_len = fread(ast_buf, 1, sizeof(ast_buf), stdin);
	if (in_len <= 0 || in_len % 32 != 0) {
		fprintf(stderr, "c2: bad AST input (%ld bytes)\n", in_len);
		return 1;
	}
	fprintf(stderr, "c2: %ld AST nodes\n", in_len / 32);

#ifdef MUT_SCHEDULE
	srng(42);
#endif
	out_len = compile((long)ast_buf, in_len, (long)out_buf);
	fprintf(stderr, "c2: %ld bytes of code\n", out_len);

	/* dump function offset table */
	{
		extern int func_off[];
		struct node *a = (struct node *)ast_buf;
		long ta = a[0].a, tb = a[0].b, fi = 0;
		long j;
		fprintf(stderr, "\n=== Function offsets ===\n");
		for (j = 0; j < tb; j++) {
			if (a[ta + j].kind == N_FUNC) {
				fprintf(stderr, "  f%-3ld  off=0x%04x\n", fi, func_off[fi]);
				fi++;
			}
		}
	}

	fwrite(out_buf, 1, out_len, stdout);
	return 0;
}
