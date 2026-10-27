/*
 * c1_compile.c — Meta-C compiler core
 *
 * Written in Meta-C: compiles with both gcc (via c1_host) and c1 itself.
 * Included by c1_host.c and c1.c.
 *
 * Entry: long compile(long in, long in_len, long out_buf)
 *   in      = pointer to serialised AST
 *   in_len  = AST byte count
 *   out_buf = pointer to output buffer (at least 1MB)
 *   returns = output byte count
 */

#include "ast.h"

#define C1_MAX_FUNC   256
#define C1_MAX_FIXUP  4096
#define C1_MAX_PATCH  256
#define C1_MAX_GLOB   4096
#define C1_MAX_LOCAL  256

#ifndef C1M_SHARED
struct node *ast;
long code_base, bss_base;
#endif

/* call site fixup: off = rel32 location, fid = target function ID */
struct fixup {
	long off;
	long fid;
};

long func_off[C1_MAX_FUNC];
struct fixup fixes[C1_MAX_FIXUP];
long nfix;

char *out;      /* output code buffer (set by compile) */
long pos;       /* write position in out[] */

/* break/continue targets.
 * cont_target dual mode: in WHILE, set to loop top (direct backward jump);
 * in DO_WHILE, set to 0 (continue offsets deferred into cont_patches[]). */
long brk_patches[C1_MAX_PATCH];
long brk_count;
long cont_patches[C1_MAX_PATCH];
long cont_count;
long cont_target;

long gvar_off[C1_MAX_GLOB];  /* BSS byte offset for global id (index = id - GLOBAL_ID_BASE) */
long lvar_off[C1_MAX_LOCAL];   /* frame byte offset (negative) for local id */
long frame_size;               /* current function frame size in bytes */

/* Sys V param registers: rdi rsi rdx rcx r8 r9 */
long param_reg(long i)
{
	if (i == 0) return 7;
	if (i == 1) return 6;
	if (i == 2) return 2;
	if (i == 3) return 1;
	if (i == 4) return 8;
	return 9;
}

/* emit one byte */
void eb(long b)
{
	out[pos] = (char)b;
	pos++;
}

/* write little-endian dword at arbitrary offset */
void write32(long at, long v)
{
	out[at]     = (char)(v & 0xff);
	out[at + 1] = (char)((v >> 8) & 0xff);
	out[at + 2] = (char)((v >> 16) & 0xff);
	out[at + 3] = (char)((v >> 24) & 0xff);
}

/* emit little-endian dword at pos, advance */
void ed(long v) { write32(pos, v); pos += 4; }

/* emit little-endian qword */
void eq(long v) { ed(v); ed(v >> 32); }

/* patch a rel32 at 'at' to jump to current pos */
void fixup32(long at) { write32(at, pos - at - 4); }

/* emit REX prefix if needed (w=64-bit, r/x/b=extension bits) */
void rex(long w, long r, long x, long b)
{
	long v;
	v = 0x40 | (w << 3) | (r << 2) | (x << 1) | b;
	if (v != 0x40)
		eb(v);
}

void rexw(void) { eb(0x48); }  /* REX.W */
void emit_leave_ret(void) { eb(0xc9); eb(0xc3); }

/* ModRM byte: mod=11 (register-to-register), reg, rm */
void modrm_rr(long reg, long rm)
{
	eb(0xc0 | ((reg & 7) << 3) | (rm & 7));
}

/* ModRM byte for [rbp+disp] addressing (disp8 or disp32) */
void modrm_rbp(long reg, long disp)
{
	if (disp >= -128 && disp <= 127) {
		eb(0x40 | ((reg & 7) << 3) | 5);
		eb(disp & 0xff);
	} else {
		eb(0x80 | ((reg & 7) << 3) | 5);
		ed(disp);
	}
}

/* movabs rax, imm64 */
void mov_rax_imm64(long v)
{
	/* REX.W */
	rexw();
	/* MOV rax, imm64 */
	eb(0xb8);
	eq(v);
}

/* push rax */
void push_rax(void) { eb(0x50); }
/* pop rcx */
void pop_rcx(void)  { eb(0x59); }

/* pop into register r via pop rax + mov.
 * Uses 58 (pop rax) instead of 59 (pop rcx) so the decompiler
 * can distinguish arg-pops (POP_P) from binop-pops (POP59). */
void pop_reg(long r)
{
	/* pop rax */
	eb(0x58);
	if (r >= 8) {
		/* mov r8+, rax */
		eb(0x49); eb(0x89); eb(0xc0 + (r & 7));
	} else {
		/* mov reg, rax */
		eb(0x48); eb(0x89); eb(0xc0 + r);
	}
}

/* emit rax-based var access: load(0x8b), store(0x89), or lea(0x8d).
 * global: rip-relative; local: rbp-relative. */
void gen_var_op(long id, long opcode)
{
	if (id >= GLOBAL_ID_BASE) {
#ifdef DEBUG
		if (id - GLOBAL_ID_BASE >= C1_MAX_GLOB)
			trap(0x0203, id);
#endif
		rexw(); eb(opcode); eb(0x05);
		ed(bss_base + gvar_off[id - GLOBAL_ID_BASE] - (code_base + pos + 4));
	} else {
		rexw(); eb(opcode);
		modrm_rbp(0, lvar_off[id]);
	}
}

void gen_load_var(long id)  { gen_var_op(id, 0x8b); }
void gen_store_var(long id) { gen_var_op(id, 0x89); }
void gen_addr_var(long id)  { gen_var_op(id, 0x8d); }

/* dereference load: read w bytes from [rax] into rax.
 * 1=movzx byte, 2=movzx word, 4=movsxd dword, 8=mov qword. */
void gen_deref_load(long w)
{
	if (w == 1) {
		/* movzx rax, byte [rax] */
		rexw(); eb(0x0f); eb(0xb6); eb(0x00);
	} else if (w == 2) {
		/* movzx rax, word [rax] */
		rexw(); eb(0x0f); eb(0xb7); eb(0x00);
	} else if (w == 4) {
		/* movsxd rax, dword [rax] */
		rexw(); eb(0x63); eb(0x00);
	} else {
		/* mov rax, qword [rax] */
		rexw(); eb(0x8b); eb(0x00);
	}
}

/* dereference store: write w bytes from rax/ax/al to [rcx].
 * 1=byte, 2=word, 4=dword, 8=qword. */
void gen_deref_store(long w)
{
	if (w == 1) {
		/* mov byte [rcx], al */
		eb(0x88); eb(0x01);
	} else if (w == 2) {
		/* mov word [rcx], ax */
		eb(0x66); eb(0x89); eb(0x01);
	} else if (w == 4) {
		/* mov dword [rcx], eax */
		eb(0x89); eb(0x01);
	} else {
		/* mov qword [rcx], rax */
		rexw(); eb(0x89); eb(0x01);
	}
}

/* map AST comparison kind to x86 SETcc opcode byte */
long setcc_for(long kind)
{
	if (kind == N_EQ)   return 0x94; /* sete */
	if (kind == N_NE)   return 0x95; /* setne */
	if (kind == N_LT_S) return 0x9c; /* setl */
	if (kind == N_LT_U) return 0x92; /* setb */
	if (kind == N_GT_S) return 0x9f; /* setg */
	if (kind == N_GT_U) return 0x97; /* seta */
	if (kind == N_LE_S) return 0x9e; /* setle */
	if (kind == N_LE_U) return 0x96; /* setbe */
	if (kind == N_GE_S) return 0x9d; /* setge */
	if (kind == N_GE_U) return 0x93; /* setae */
#ifdef DEBUG
	trap(0x0101, kind); /* bad comparison kind */
#endif
	return 0x94;
}

void gen_stmt(long ni);
void gen_expr(long ni);

/* patch all break jumps accumulated since save_brk to current pos */
void patch_breaks(long save_brk)
{
	while (brk_count > save_brk) {
		brk_count--;
		fixup32(brk_patches[brk_count]);
	}
}

/* binop: opc=opcode, digit=ModRM /r extension, swap=1 evals b before a */
void gen_binop(long ni, long opc, long digit, long swap)
{
	struct node *n;

	n = &ast[ni];
	if (swap) {
		gen_expr(n->b); push_rax();
		gen_expr(n->a); pop_rcx();
	} else {
		gen_expr(n->a); push_rax();
		gen_expr(n->b); pop_rcx();
	}
	rexw(); eb(opc); modrm_rr(digit, 0);
}

/* generate code for expression ni, result in rax */
void gen_expr(long ni)
{
	struct node *n, *lhs;
	long p1, p2, pf, i;

	n = &ast[ni];

	if (n->kind == N_NUM) {
		/* movabs rax, imm64 */
		mov_rax_imm64(n->val);
		return;
	}
	if (n->kind == N_VAR) {
		gen_load_var(n->val);
		return;
	}
	if (n->kind == N_ADDR) {
		if (ast[n->a].kind == N_VAR)
			gen_addr_var(ast[n->a].val);
		else if (ast[n->a].kind == N_DEREF)
			gen_expr(ast[n->a].a);
		else
			gen_expr(n->a);
		return;
	}
	if (n->kind == N_DEREF) {
		gen_expr(n->a);
		gen_deref_load(n->b);
		return;
	}
	if (n->kind == N_NEG) {
		gen_expr(n->a);
		/* neg rax */
		rexw(); eb(0xf7); modrm_rr(3, 0);
		return;
	}
	if (n->kind == N_NOT) {
		gen_expr(n->a);
		/* not rax */
		rexw(); eb(0xf7); modrm_rr(2, 0);
		return;
	}
	if (n->kind == N_LOG_NOT) {
		gen_expr(n->a);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* sete al */
		eb(0x0f); eb(0x94); modrm_rr(0, 0);
		/* movzx rax, al */
		rexw(); eb(0x0f); eb(0xb6); modrm_rr(0, 0);
		return;
	}
	/* add rcx, rax */
	if (n->kind == N_ADD) { gen_binop(ni, 0x01, 1, 0); return; }
	/* sub rax, rcx (swap: a stays in rax) */
	if (n->kind == N_SUB) { gen_binop(ni, 0x29, 1, 1); return; }
	if (n->kind == N_MUL) {
		gen_expr(n->a); push_rax();
		gen_expr(n->b); pop_rcx();
		/* imul rax, rcx */
		rexw(); eb(0x0f); eb(0xaf); modrm_rr(0, 1);
		return;
	}
	/* div/mod: signed uses CQO+IDIV, unsigned uses XOR RDX+DIV.
	 * mod moves remainder (rdx) to rax after division. */
	if (n->kind == N_DIV_S || n->kind == N_DIV_U ||
	    n->kind == N_MOD_S || n->kind == N_MOD_U) {
		/* reuse p1=is_signed, pf=is_mod (no overlap with LOG_AND/OR) */
		p1 = (n->kind == N_DIV_S || n->kind == N_MOD_S);
		pf = (n->kind == N_MOD_S || n->kind == N_MOD_U);
		gen_expr(n->b); push_rax();
		gen_expr(n->a);
		if (p1)
			{ rexw(); eb(0x99); }            /* cqo */
		else
			{ rexw(); eb(0x31); modrm_rr(2, 2); } /* xor rdx, rdx */
		pop_rcx();
		rexw(); eb(0xf7);
		if (p1)
			modrm_rr(7, 1);                  /* idiv rcx */
		else
			modrm_rr(6, 1);                  /* div rcx */
		if (pf)
			{ rexw(); eb(0x89); modrm_rr(2, 0); } /* mov rax, rdx */
		return;
	}
	/* shl rax, cl */
	if (n->kind == N_SHL)   { gen_binop(ni, 0xd3, 4, 1); return; }
	/* sar rax, cl */
	if (n->kind == N_SHR_S) { gen_binop(ni, 0xd3, 7, 1); return; }
	/* shr rax, cl */
	if (n->kind == N_SHR_U) { gen_binop(ni, 0xd3, 5, 1); return; }
	/* and rcx, rax */
	if (n->kind == N_AND) { gen_binop(ni, 0x21, 1, 0); return; }
	/* or rcx, rax */
	if (n->kind == N_OR)  { gen_binop(ni, 0x09, 1, 0); return; }
	/* xor rcx, rax */
	if (n->kind == N_XOR) { gen_binop(ni, 0x31, 1, 0); return; }
	/* comparisons */
	if (n->kind >= N_EQ && n->kind <= N_GE_U) {
		gen_expr(n->b); push_rax();
		gen_expr(n->a); pop_rcx();
		/* cmp rax, rcx */
		rexw(); eb(0x39); modrm_rr(1, 0);
		/* setcc al */
		eb(0x0f); eb(setcc_for(n->kind)); modrm_rr(0, 0);
		/* movzx rax, al */
		rexw(); eb(0x0f); eb(0xb6); modrm_rr(0, 0);
		return;
	}
	/* short-circuit && */
	if (n->kind == N_LOG_AND) {
		gen_expr(n->a);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* je false */
		eb(0x0f); eb(0x84); p1 = pos; ed(0);
		gen_expr(n->b);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* setne al */
		eb(0x0f); eb(0x95); modrm_rr(0, 0);
		/* movzx rax, al */
		rexw(); eb(0x0f); eb(0xb6); modrm_rr(0, 0);
		/* jmp end */
		eb(0xe9); p2 = pos; ed(0);
		/* false: */
		fixup32(p1);
		/* xor rax, rax */
		rexw(); eb(0x31); modrm_rr(0, 0);
		/* end: */
		fixup32(p2);
		return;
	}
	/* short-circuit || */
	if (n->kind == N_LOG_OR) {
		gen_expr(n->a);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* jne true */
		eb(0x0f); eb(0x85); p1 = pos; ed(0);
		gen_expr(n->b);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* je false */
		eb(0x0f); eb(0x84); pf = pos; ed(0);
		/* true: */
		fixup32(p1);
		/* movabs rax, 1 */
		mov_rax_imm64(1);
		/* jmp end */
		eb(0xe9); p2 = pos; ed(0);
		/* false: */
		fixup32(pf);
		/* xor rax, rax */
		rexw(); eb(0x31); modrm_rr(0, 0);
		/* end: */
		fixup32(p2);
		return;
	}
	/* assignment */
	if (n->kind == N_ASSIGN) {
		lhs = &ast[n->a];
		if (lhs->kind == N_VAR) {
			gen_expr(n->b);
			gen_store_var(lhs->val);
		} else if (lhs->kind == N_DEREF) {
			/* address → rax */
			gen_expr(lhs->a);
			push_rax();
			/* value → rax */
			gen_expr(n->b);
			pop_rcx();
			gen_deref_store(lhs->b);
		} else {
			gen_expr(n->b);
		}
		return;
	}
	/* function call */
	if (n->kind == N_CALL) {
#ifdef DEBUG
		if (n->b > 6)
			trap(0x0102, n->b); /* >6 args */
#endif
		for (i = n->b - 1; i >= 0; i--) {
			gen_expr(n->a + i);
			push_rax();
		}
		for (i = 0; i < n->b && i < 6; i++)
			pop_reg(param_reg(i));
		/* call rel32 */
		eb(0xe8);
#ifdef DEBUG
		if (nfix >= C1_MAX_FIXUP)
			trap(0x0104, nfix); /* fixes overflow */
#endif
		fixes[nfix].off = pos;
		fixes[nfix].fid = n->val;
		nfix++;
		ed(0);                                 /* rel32 placeholder */
		return;
	}
#ifdef DEBUG
	trap(0x0100, n->kind); /* bad expr kind */
#endif
}

/* generate code for statement ni */
void gen_stmt(long ni)
{
	struct node *n;
	long p, pe, top, i;
	long save_brk, save_cont, save_cc;

	n = &ast[ni];

	if (n->kind == N_BLOCK) {
		for (i = 0; i < n->b; i++)
			gen_stmt(n->a + i);
		return;
	}
	if (n->kind == N_DECL)
		return;
	if (n->kind == N_RETURN) {
		if (n->a)
			gen_expr(n->a);
		emit_leave_ret();
		return;
	}
	if (n->kind == N_IF) {
		gen_expr(n->a);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		if (n->val) {
			/* je else */
			eb(0x0f); eb(0x84); pe = pos; ed(0);
			gen_stmt(n->b);
			/* jmp end */
			eb(0xe9); p = pos; ed(0);
			fixup32(pe);
			gen_stmt(n->val);
			fixup32(p);
		} else {
			/* je end */
			eb(0x0f); eb(0x84); p = pos; ed(0);
			gen_stmt(n->b);
			fixup32(p);
		}
		return;
	}
	if (n->kind == N_BREAK) {
		/* jmp <break_exit> */
		eb(0xe9);
#ifdef DEBUG
		if (brk_count >= C1_MAX_PATCH)
			trap(0x0105, brk_count); /* brk overflow */
#endif
		brk_patches[brk_count] = pos;
		brk_count++;
		ed(0);                                 /* placeholder */
		return;
	}
	if (n->kind == N_CONTINUE) {
		/* jmp <cont_target> */
#ifdef HOST_BUILD
		fprintf(stderr, "  N_CONTINUE at pos=0x%lx cont_target=%ld cont_count=%ld\n", pos, cont_target, cont_count);
#endif
		eb(0xe9);
		if (cont_target > 0) {
			ed(cont_target - pos - 4);
		} else {
#ifdef DEBUG
			if (cont_count >= C1_MAX_PATCH)
				trap(0x0106, cont_count); /* cont overflow */
#endif
			cont_patches[cont_count] = pos;
			cont_count++;
			ed(0);
		}
		return;
	}
	if (n->kind == N_WHILE) {
		save_brk = brk_count;
		save_cont = cont_target;
		/* loop: */
		top = pos;
		cont_target = top;
		gen_expr(n->a);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* je exit */
		eb(0x0f); eb(0x84); p = pos; ed(0);
		gen_stmt(n->b);
		/* jmp loop */
		eb(0xe9); ed(top - pos - 4);
		/* exit: */
		fixup32(p);
		patch_breaks(save_brk);
		cont_target = save_cont;
		return;
	}
	/* do_while: cont_target=0 defers continue jumps into cont_patches[] */
	if (n->kind == N_DO_WHILE) {
#ifdef HOST_BUILD
		fprintf(stderr, "  do_while at pos=0x%lx cont_target=%ld cont_count=%ld\n", pos, cont_target, cont_count);
#endif
		save_brk = brk_count;
		save_cont = cont_target;
		save_cc = cont_count;
		cont_target = 0;
		/* loop: */
		top = pos;
		gen_stmt(n->a);
		/* patch continue targets to here (before condition) */
#ifdef HOST_BUILD
		if (cont_count > save_cc)
			fprintf(stderr, "  do_while fixup: pos=0x%lx (relative to code_base)\n", pos);
#endif
		while (cont_count > save_cc) {
			cont_count--;
#ifdef HOST_BUILD
			fprintf(stderr, "    cont[%ld] at 0x%lx -> target=0x%lx\n",
				cont_count, cont_patches[cont_count],
				pos);
#endif
			fixup32(cont_patches[cont_count]);
		}
		gen_expr(n->b);
		/* test rax, rax */
		rexw(); eb(0x85); modrm_rr(0, 0);
		/* jne loop */
		eb(0x0f); eb(0x85); ed(top - pos - 4);
		patch_breaks(save_brk);
		cont_target = save_cont;
		return;
	}
	gen_expr(ni);
}

/* DECL.a = number of 8-byte slots (c1 ignores DECL.b) */
long decl_slots(long ni)
{
	return ast[ni].a;
}

/* build global variable BSS offset table from top-level DECLs.
 * Each variable gets slots*8 bytes (c1 ignores DECL.b). */
void build_global_offsets(void)
{
	struct node *top, *child;
	long off, i, gid, slots;

	top = &ast[0];
	off = 0;
	for (i = 0; i < top->b; i++) {
		child = &ast[top->a + i];
		if (child->kind == N_DECL) {
			gid = child->val - GLOBAL_ID_BASE;
#ifdef DEBUG
			if (gid < 0 || gid >= C1_MAX_GLOB)
				trap(0x0207, child->val);
#endif
			if (gid >= 0 && gid < C1_MAX_GLOB)
				gvar_off[gid] = off;
			slots = decl_slots(top->a + i);
			off += slots * 8;
		}
	}
}

/* build local variable frame offsets from function body DECLs.
 * Each variable gets slots*8 bytes, allocated at negative RBP offsets. */
void build_local_offsets(long body_ni)
{
	struct node *body, *child;
	long off, i, slots;

	body = &ast[body_ni];
	off = 0;
	for (i = 0; i < body->b; i++) {
		child = &ast[body->a + i];
		if (child->kind == N_DECL) {
#ifdef DEBUG
			if (child->val < 0 || child->val >= C1_MAX_LOCAL)
				trap(0x0206, child->val);
#endif
			slots = decl_slots(body->a + i);
			off += slots * 8;
			lvar_off[child->val] = -off;
		}
	}
	frame_size = off;
}

/* compile one function: prologue, param saves, body, implicit return */
void gen_func(long ni)
{
	struct node *fn, *body, *child;
	long fid, nparams, r, i;

	fn = &ast[ni];
	fid = fn->val;
	nparams = fn->b;
	body = &ast[fn->a];

#ifdef DEBUG
	if (fid >= C1_MAX_FUNC)
		trap(0x0103, fid); /* func_off overflow */
#endif
	if (fid < C1_MAX_FUNC)
		func_off[fid] = pos;

	build_local_offsets(fn->a);

	/* prologue: push rbp; mov rbp, rsp; sub rsp, frame_size */
	eb(0x55);
	rexw(); eb(0x89); modrm_rr(4, 5);
	if (frame_size > 0) {
		rexw(); eb(0x81); modrm_rr(5, 4);
		ed(frame_size);
	}

	/* save param registers */
	for (i = 0; i < nparams && i < 6; i++) {
		r = param_reg(i);
		if (r >= 8)
			rex(1, 1, 0, 0);
		else
			rex(1, 0, 0, 0);
		/* mov [rbp+disp], param_reg */
		eb(0x89);
		modrm_rbp(r & 7, lvar_off[i]);
	}

	/* emit body, skip DECLs */
	for (i = 0; i < body->b; i++) {
		child = &ast[body->a + i];
		if (child->kind != N_DECL)
			gen_stmt(body->a + i);
	}

	/* emit leave+ret if body doesn't already end with a return */
	if (body->b > 0) {
		child = &ast[body->a + body->b - 1];
		if (child->kind != N_RETURN)
			emit_leave_ret();
	} else {
		emit_leave_ret();
	}
}

/* patch CALL rel32 placeholders with actual function offsets */
void resolve_fixups(void)
{
	long i;

	for (i = 0; i < nfix; i++)
		write32(fixes[i].off,
			func_off[fixes[i].fid] - (fixes[i].off + 4));
}

/* compile serialised AST to x86-64 machine code.
 * Returns byte count written to out_buf. */
long compile(long in, long in_len, long out_buf)
{
	struct node *top;
	long i, j;

	ast = (struct node *)in;

	/* write directly into output buffer */
	out = (char *)out_buf;
	pos = 0;

	code_base = CODE_BASE;
	bss_base = BSS_BASE;
	nfix = 0;
	brk_count = 0;
	cont_count = 0;
	cont_target = 0;

	build_global_offsets();

	/* entry: call main; hlt */
	eb(0xe8); i = pos; ed(0);
	eb(0xf4);

	/* compile all functions */
	top = &ast[0];
	for (j = 0; j < top->b; j++) {
		if (ast[top->a + j].kind == N_FUNC)
			gen_func(top->a + j);
	}

	/* patch entry to main */
	write32(i, func_off[0] - (i + 4));
	resolve_fixups();

	return pos;
}
