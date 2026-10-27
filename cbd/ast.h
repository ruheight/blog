/* ast.h — shared AST node definition, kind enum, address-space layout */
#ifndef AST_H
#define AST_H

/* address-space partitions for the emulator's flat memory model */
#define CODE_BASE  0x2000000
#define BSS_BASE   0x4000000
/* variable IDs at or above this are globals, below are locals */
#define GLOBAL_ID_BASE 0x1000

#ifdef DEBUG
/* force a fault the emulator can catch as a breakpoint */
void trap(long code, long val)
{
	*(unsigned char *)0xFFFF0000 = 0;
}
#endif

struct node {
	long kind; /* node type (N_BLOCK, N_FUNC, etc.) */
	long a;    /* first child/operand; DECL: slot count (1=scalar, N=array) */
	long b;    /* second child/operand or child count; DECL: element width (1/2/4/8, 0=8) */
	long val;  /* NUM: literal; VAR/DECL: variable ID; FUNC/CALL: function ID */
};

/* node kinds: control flow, expressions, comparisons, unary ops, loop control */
/* signed/unsigned variants (_S/_U) kept separate so codegen picks the right insn */
enum {
	N_BLOCK = 1, N_FUNC, N_DECL, N_IF, N_WHILE, N_DO_WHILE,
	N_RETURN, N_ASSIGN, N_DEREF, N_ADDR, N_NUM, N_VAR, N_CALL,
	N_ADD, N_SUB, N_MUL,
	N_DIV_S, N_DIV_U, N_MOD_S, N_MOD_U,
	N_SHL, N_SHR_S, N_SHR_U,
	N_AND, N_OR, N_XOR, N_LOG_AND, N_LOG_OR,
	N_EQ, N_NE,
	N_LT_S, N_LT_U, N_GT_S, N_GT_U,
	N_LE_S, N_LE_U, N_GE_S, N_GE_U,
	N_NEG, N_NOT, N_LOG_NOT,
	N_BREAK, N_CONTINUE,
	N_CALL_IND, /* indirect call: a=callee_expr, b=nargs, val=arg_start */
	N_FUNC_ADDR /* address of function: val=fid */
};

#endif

