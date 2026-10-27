/* ir.h — shared IR opcode enum and instruction struct
 *
 * Used by: c2_compile.c, c2_decode.c, c2_decompile.c, c2m.c
 *
 *   LOAD_IMM:  dst=vreg  s1=0             s2=0    imm=value
 *   LOAD_LOC:  dst=vreg  s1=var_id|disp    s2=0
 *   STORE_LOC: dst=0     s1=var_id|disp    s2=vreg
 *   ADDR_LOC:  dst=vreg  s1=var_id|disp    s2=0
 *   LOAD_GLOB: dst=vreg  s1=gid|addr       s2=0
 *   STORE_GLOB:dst=0     s1=gid|addr       s2=vreg
 *   ADDR_GLOB: dst=vreg  s1=gid|addr       s2=0
 *   DEREF_LD:  dst=vreg  s1=vreg(addr)     s2=width
 *   DEREF_ST:  dst=width s1=vreg(addr)     s2=vreg(val)
 *   ADD..XOR:  dst=vreg  s1=vreg           s2=vreg
 *   NEG, NOT:  dst=vreg  s1=vreg           s2=0
 *   EQ..GEU:   dst=vreg  s1=vreg           s2=vreg
 *   LNOT:      dst=vreg  s1=vreg           s2=0
 *   SETNE:     dst=vreg  s1=vreg           s2=0
 *   CLR:       dst=vreg  s1=0|vreg(prior)  s2=0
 *   JMP:       dst=0     s1=target          s2=0
 *   JZ:        dst=0     s1=vreg(cond)     s2=target
 *   JNZ:       dst=0     s1=vreg(cond)     s2=target
 *   RET:       dst=0     s1=vreg           s2=0
 *   RET_VOID:  dst=0     s1=0              s2=0
 *   CALL:      dst=vreg  s1=fid|addr       s2=nargs
 *   CALL_IND:  dst=vreg  s1=vreg(addr)     s2=nargs
 *   ARG:       dst=0     s1=vreg           s2=arg_index
 *   FUNC:      dst=fid   s1=frame_size     s2=nparams
 *   PARAM:     dst=var_id s1=0             s2=param_index
 *   NOP:       dst=0     s1=0              s2=0
 */
#ifndef IR_H
#define IR_H

enum {
	IR_LOAD_IMM = 1,
	IR_LOAD_LOC, IR_STORE_LOC, IR_ADDR_LOC,
	IR_LOAD_GLOB, IR_STORE_GLOB, IR_ADDR_GLOB,
	IR_DEREF_LD, IR_DEREF_ST,

	IR_ADD, IR_SUB, IR_MUL,
	IR_DIVS, IR_DIVU, IR_MODS, IR_MODU,
	IR_SHL, IR_SHRS, IR_SHRU,
	IR_AND, IR_OR, IR_XOR,
	IR_NEG, IR_NOT,

	IR_EQ, IR_NE,
	IR_LTS, IR_LTU, IR_GTS, IR_GTU,
	IR_LES, IR_LEU, IR_GES, IR_GEU,

	IR_LNOT,
	IR_SETNE,
	IR_CLR,

	IR_JMP, IR_JZ, IR_JNZ,
	IR_RET, IR_RET_VOID,

	IR_CALL, IR_ARG,

	IR_FUNC, IR_PARAM, IR_NOP,
	IR_CALL_IND, /* indirect call: dst=vreg, s1=vreg(addr), s2=nargs */
	IR_ADDR_FUNC /* address of function: dst=vreg, s1=fid */
};

/* op/dst/s1/s2 are int (not long) to halve struct size; imm stays long for addresses */
struct insn {
	int op;
	int dst;
	int s1;
	int s2;
	long imm; /* when nonzero, replaces s2 as the immediate operand */
};

#define MAX_IR      8192  /* peak ~2725, 2× = 5450 */
#define MAX_VREG    4096  /* peak ~2131, 52% — keep */
#define MAX_LOCALS  64    /* peak 24 */
#define MAX_GLOBALS 256   /* peak 75 */
#define MAX_FIXUPS  2048  /* peak 809 */
#define MAX_JMP_FIX 512   /* peak 216 */
#define MAX_PARAMS  6
#define MAX_LOOPS   64    /* peak 6 */
#define MAX_STMTS   256   /* peak 30 on c2m, but t40_spill needs ~65 */
#define MAX_FUNC    256   /* peak 104, 41% — keep */

#endif
