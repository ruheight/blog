/*
 * c2_compile.c — AST → IR lowering + top-level compile entry point
 *
 * Pipeline: AST → IR (linear, label-based jumps, vregs) → regalloc → x86-64 emit.
 * Regalloc and emit are in c2_codegen.c (included below).
 * Written in Meta-C. Included by c2.c.
 * Entry: long compile(long in, long in_len, long out_buf)
 */

#include "ast.h"
#include "ir.h"

/* AST input + memory layout (used by compile/codegen) */
struct node *ast;
long code_base, bss_base;
/* rng_state, srng(), rng() defined in ir.h */

/* output buffer */

char *out;
long pos;

void eb(long b) { out[pos] = (char)b; pos++; }

void ed(long v)
{
	out[pos]     = (char)(v & 0xff);
	out[pos + 1] = (char)((v >> 8) & 0xff);
	out[pos + 2] = (char)((v >> 16) & 0xff);
	out[pos + 3] = (char)((v >> 24) & 0xff);
	pos += 4;
}

void eq(long v) { ed(v); ed(v >> 32); }

void write32(long at, long v)
{
	out[at]     = (char)(v & 0xff);
	out[at + 1] = (char)((v >> 8) & 0xff);
	out[at + 2] = (char)((v >> 16) & 0xff);
	out[at + 3] = (char)((v >> 24) & 0xff);
}

/* x86 encoding helpers */

void emit_rex(long w, long reg, long rm)
{
	long v;
	v = 0x40;
	if (w) v |= 8;
	if (reg >= 8) v |= 4;
	if (rm >= 8) v |= 1;
	if (v != 0x40) eb(v);
}

/* emit REX for byte operations — always emits when called.
 * For 0x88 (byte store) / 0x0F B6 (byte load), registers 4-7 without
 * REX encode ah/ch/dh/bh instead of spl/bpl/sil/dil.
 * Caller must only call this when REX is actually needed (reg>=4 || rm>=8). */
void emit_rex_byte(long reg, long rm)
{
	long v;
	v = 0x40;
	if (reg >= 8) v |= 4;
	if (rm >= 8) v |= 1;
	eb(v);
}

void emit_modrm_rr(long reg, long rm)
{
	eb(0xc0 | ((reg & 7) << 3) | (rm & 7));
}

/* ModRM for [rm] dereference with mod=00.
 * Handles two x86 encoding gotchas:
 *   rm & 7 == 5 (rbp/r13): mod=00 rm=5 means RIP-relative, not [rbp].
 *     Fix: emit mod=01 rm=5 disp8=0 instead.
 *   rm & 7 == 4 (rsp/r12): mod=00 rm=4 signals SIB byte follows.
 *     Fix: emit SIB=0x24 (scale=0 index=4=none base=rsp). */
void emit_modrm_deref(long reg, long rm)
{
	if ((rm & 7) == 5) {
		/* mod=01, disp8=0 */
		eb(0x40 | ((reg & 7) << 3) | (rm & 7));
		eb(0);
	} else if ((rm & 7) == 4) {
		/* mod=00, rm=4 (SIB escape), SIB=0x24 (base=rsp, index=none) */
		eb(((reg & 7) << 3) | 4);
		eb(0x24);
	} else {
		eb(((reg & 7) << 3) | (rm & 7));
	}
}

void emit_modrm_rbp(long reg, long disp)
{
	if (disp >= -128 && disp <= 127) {
		eb(0x40 | ((reg & 7) << 3) | 5);
		eb(disp & 0xff);
	} else {
		eb(0x80 | ((reg & 7) << 3) | 5);
		ed(disp);
	}
}

/* setcc needs REX when rd >= 4 (spl/bpl/sil/dil need REX) */
void emit_setcc(long cc, long rd)
{
	if (rd >= 4) {
		if (rd >= 8)
			eb(0x41);
		else
			eb(0x40);
	}
	eb(0x0f); eb(cc);
	eb(0xc0 | (rd & 7));
}

/* movzx rd, rd_8bit (extend setcc result to 64-bit) */
void emit_movzx_byte(long rd)
{
	emit_rex(1, rd, rd);
	eb(0x0f); eb(0xb6);
	emit_modrm_rr(rd & 7, rd & 7);
}

/* mov Rd, Rs (64-bit) */
void emit_mov_rr(long dst, long src)
{
	if (dst == src) return;
	emit_rex(1, src, dst);
	eb(0x89);
	emit_modrm_rr(src & 7, dst & 7);
}

/* [O1] compact mov immediate encoding.
 * Picks the smallest x86 encoding for mov reg, imm:
 *   val==0:                 mov reg32, 0        (5-6 bytes, zero-extends)
 *   val > 0, <= 0x7fffffff: mov reg32, imm32    (5-6 bytes)
 *   val < 0, >= -0x80000000: mov r64, sext-imm32 via C7 /0 (7 bytes)
 *   else:                    movabs reg, imm64   (10 bytes)
 * Naive version always uses movabs imm64.
 * NOT wrappable — decoder must agree with encoder. */
void emit_mov_imm(long reg, long val)
{
	if (val == 0) {
		/* mov reg32, 0 — avoid xor reg,reg so decompiler
		 * can distinguish LOAD_IMM 0 from XOR_SELF (&&/||) */
		if (reg >= 8)
			eb(0x41);
		eb(0xb8 + (reg & 7));
		ed(0);
		return;
	}
	if (val > 0 && val <= 0x7fffffff) {
		/* mov reg32, imm32 (5 or 6 bytes, zero-extends to 64) */
		if (reg >= 8)
			eb(0x41);
		eb(0xb8 + (reg & 7));
		ed(val);
		return;
	}
	if (val < 0 && val >= -0x80000000L) {
		/* mov r64, sign-extended imm32 (7 bytes) */
		emit_rex(1, 0, reg);
		eb(0xc7);
		emit_modrm_rr(0, reg & 7);
		ed(val);
		return;
	}
	/* movabs reg, imm64 (10 bytes) */
	emit_rex(1, 0, reg);
	eb(0xb8 + (reg & 7));
	eq(val);
}

/* mov Rd, [rbp + disp] (qword) */
void emit_load_rbp(long reg, long disp)
{
	emit_rex(1, reg, 0);
	eb(0x8b);
	emit_modrm_rbp(reg & 7, disp);
}

/* mov [rbp + disp], Rs (qword) */
void emit_store_rbp(long reg, long disp)
{
	emit_rex(1, reg, 0);
	eb(0x89);
	emit_modrm_rbp(reg & 7, disp);
}

/* width-aware load from [rbp + disp]:
 *   w=1: MOVZX r32, byte [rbp+disp] (0F B6)
 *   w=2: MOVZX r32, word [rbp+disp] (0F B7)
 *   w=4: MOV r32, dword [rbp+disp] (8B, no REX.W — auto zero-extends)
 *   w=8: MOV r64, qword [rbp+disp] (REX.W 8B) */
void emit_load_rbp_w(long reg, long disp, long w)
{
	/* prefix + opcode vary by width; modrm is always the same */
	if (w == 1) {
		emit_rex(0, reg, 0); eb(0x0f); eb(0xb6);
	} else if (w == 2) {
		emit_rex(0, reg, 0); eb(0x0f); eb(0xb7);
	} else if (w == 4) {
		emit_rex(0, reg, 0); eb(0x8b);
	} else {
		emit_rex(1, reg, 0); eb(0x8b);
	}
	emit_modrm_rbp(reg & 7, disp);
}

/* width-aware store to [rbp + disp]:
 *   w=1: MOV byte [rbp+disp], r8 (88) — needs REX for spl/bpl/sil/dil
 *   w=2: MOV word [rbp+disp], r16 (66 89)
 *   w=4: MOV dword [rbp+disp], r32 (89, no REX.W)
 *   w=8: MOV qword [rbp+disp], r64 (REX.W 89) */
void emit_store_rbp_w(long reg, long disp, long w)
{
	if (w == 1) {
		if (reg >= 4)
			emit_rex_byte(reg, 5); /* spl/bpl/sil/dil need REX */
		eb(0x88);
	} else if (w == 2) {
		eb(0x66); emit_rex(0, reg, 0); eb(0x89);
	} else if (w == 4) {
		emit_rex(0, reg, 0); eb(0x89);
	} else {
		emit_rex(1, reg, 0); eb(0x89);
	}
	emit_modrm_rbp(reg & 7, disp);
}

/* lea Rd, [rbp + disp] */
void emit_lea_rbp(long reg, long disp)
{
	emit_rex(1, reg, 0);
	eb(0x8d);
	emit_modrm_rbp(reg & 7, disp);
}

/* mov Rd, [rip + d32] (qword) */
void emit_load_rip(long reg, long d32)
{
	emit_rex(1, reg, 0);
	eb(0x8b);
	eb(0x05 | ((reg & 7) << 3));
	ed(d32);
}

/* mov [rip + d32], Rs (qword) */
void emit_store_rip(long reg, long d32)
{
	emit_rex(1, reg, 0);
	eb(0x89);
	eb(0x05 | ((reg & 7) << 3));
	ed(d32);
}

/* common tail: modrm(rip) + disp32 */
void emit_rip_modrm(long reg, long d32)
{
	eb(0x05 | ((reg & 7) << 3));
	ed(d32);
}

/* width-aware load from [rip + d32] */
void emit_load_rip_w(long reg, long d32, long w)
{
	if (w == 1) {
		emit_rex(0, reg, 0);
		eb(0x0f); eb(0xb6);
	} else if (w == 2) {
		emit_rex(0, reg, 0);
		eb(0x0f); eb(0xb7);
	} else if (w == 4) {
		emit_rex(0, reg, 0);
		eb(0x8b);
	} else {
		emit_rex(1, reg, 0);
		eb(0x8b);
	}
	emit_rip_modrm(reg, d32);
}

/* width-aware store to [rip + d32] */
void emit_store_rip_w(long reg, long d32, long w)
{
	if (w == 1) {
		if (reg >= 4)
			emit_rex_byte(reg, 0);
		eb(0x88);
	} else if (w == 2) {
		eb(0x66);
		emit_rex(0, reg, 0);
		eb(0x89);
	} else if (w == 4) {
		emit_rex(0, reg, 0);
		eb(0x89);
	} else {
		emit_rex(1, reg, 0);
		eb(0x89);
	}
	emit_rip_modrm(reg, d32);
}

/* lea Rd, [rip + d32] */
void emit_lea_rip(long reg, long d32)
{
	emit_rex(1, reg, 0);
	eb(0x8d);
	eb(0x05 | ((reg & 7) << 3));
	ed(d32);
}

/* ALU op Rd, Rs (64-bit): add=01, sub=29, and=21, or=09, xor=31, cmp=39 */
void emit_alu_rr(long opcode, long rd, long rs)
{
	emit_rex(1, rs, rd);
	eb(opcode);
	emit_modrm_rr(rs & 7, rd & 7);
}

/* test Rd, Rd */
void emit_test(long rd)
{
	emit_rex(1, rd, rd);
	eb(0x85);
	emit_modrm_rr(rd & 7, rd & 7);
}

/* variable offset + width: gvars[gid].off / lvars[vid].off = byte offset
 * from base (rbp for locals, BSS for globals).
 * gvars[].width / lvars[].width = element width in bytes (1/2/4/8). */
struct cvar { int off; int width; };
struct cvar gvars[MAX_GLOBALS];
struct cvar lvars[MAX_LOCALS];
long frame_size;

/* DECL.a = slot count (1=scalar, N=array)
 * DECL.b = element width (1/2/4/8, or struct size; 0 = 8) */
long decl_slots(long ni)
{
	return ast[ni].a;
}

/* read element width from DECL.b, treating 0 as 8 (backward compat) */
long decl_width(long ni)
{
	long w;
	w = ast[ni].b;
	if (w == 0) w = 8;
	return w;
}

/* natural alignment for a given width (min 1, max 8) */
long align_for(long w)
{
	if (w >= 8) return 8;
	if (w >= 4) return 4;
	if (w >= 2) return 2;
	return 1;
}

/* round up to alignment */
long align_up(long off, long align)
{
	return (off + align - 1) & ~(align - 1);
}

/* total bytes for a DECL: count * ew (or count * 8 for structs with ew > 8) */
long decl_total(long ni)
{
	long count, ew;
	count = decl_slots(ni);
	ew = decl_width(ni);
	if (ew <= 8)
		return count * ew;
	return count * 8;
}

void build_global_offsets(void)
{
	long off, i, gid, ew, al, ta, tb;
	ta = ast[0].a; tb = ast[0].b; off = 0;
	for (i = 0; i < tb; i++) {
		if (ast[ta + i].kind == N_DECL) {
			gid = ast[ta + i].val - GLOBAL_ID_BASE;
#ifdef DEBUG
			if (gid < 0 || gid >= MAX_GLOBALS)
				trap(0x0207, ast[ta + i].val);
#endif
		ew = decl_width(ta + i);
		al = align_for(ew);
			off = align_up(off, al);
			gvars[gid].off = off;
			gvars[gid].width = ew;
			off += decl_total(ta + i);
		}
	}
}

long max_vid_used;

void build_local_offsets(long body_ni)
{
	long off, i, ew, al, bla, blb, vid;
	bla = ast[body_ni].a; blb = ast[body_ni].b; off = 0;
	max_vid_used = -1;
	for (i = 0; i < blb; i++) {
		if (ast[bla + i].kind == N_DECL) {
			vid = ast[bla + i].val;
#ifdef DEBUG
			if (vid < 0 || vid >= MAX_LOCALS)
				trap(0x0206, vid);
#endif
			if (vid > max_vid_used)
				max_vid_used = vid;
			ew = decl_width(bla + i);
			al = align_for(ew);
			off += decl_total(bla + i);
			off = align_up(off, al);
			lvars[vid].off = -off;
			lvars[vid].width = ew;
		}
	}
	/* frame must be at least 8-byte aligned */
	frame_size = align_up(off, 8);
}

/*
 * IR: flat instruction array with label-based jumps.
 * Each struct insn has int fields (op, dst, s1, s2) and long imm.
 * JMP/JZ/JNZ targets are label IDs, resolved to IR indices
 * via label_ir[] before code emission.
 */

#ifndef C2M_SHARED_IR
#include "ir.h"

struct insn ir[MAX_IR];
long nir;
long next_vreg;
#endif

#define MAX_LABELS 1024  /* peak 335 */
int label_ir[MAX_LABELS];
long nlabels;

long ir_emit(long op, long dst, long s1, long s2)
{
	long idx;
#ifdef DEBUG
	if (nir >= MAX_IR)
		trap(0x0300, nir);
#endif
	idx = nir;
	ir[nir].op  = op;
	ir[nir].dst = dst;
	ir[nir].s1  = s1;
	ir[nir].s2  = s2;
	ir[nir].imm = 0;
	nir++;
	return idx;
}

/* emit binop/cmp with immediate in s2 position */
long ir_emit_imm(long op, long dst, long s1, long imm)
{
	long idx;
	idx = ir_emit(op, dst, s1, 0);
	ir[idx].imm = imm;
	return idx;
}

long new_vreg(void)
{
	long v;
#ifdef DEBUG
	if (next_vreg >= MAX_VREG)
		trap(0x0301, next_vreg);
#endif
	v = next_vreg;
	next_vreg++;
	return v;
}

long new_label(void)
{
	long l;
#ifdef DEBUG
	if (nlabels >= MAX_LABELS) trap(0x0315, nlabels);
#endif
	l = nlabels;
	label_ir[l] = -1;
	nlabels++;
	return l;
}

void set_label(long l)
{
#ifdef DEBUG
	if (l < 0 || l >= MAX_LABELS) trap(0x0304, l);
#endif
	label_ir[l] = nir;
}

#ifdef DUMP_IR
void dump_ir_func(void);
#endif

/* [VAR_CACHE] register-resident local variables.
 * Keeps locals in vregs between uses, avoiding redundant LOAD_LOCAL
 * and STORE_LOCAL IR instructions.
 * vcache[id].vreg = vreg holding current value, -1 = on stack only.
 * vcache[id].dirty = 1 = register value newer than stack.
 * Dirty vars flushed before control flow joins and calls.
 * Invalidated after calls and at control flow merges.
 * NOT wrappable — deeply integrated with correctness. */
struct var_cache {
	int vreg;   /* vreg holding current value, -1 = on stack only */
	int dirty;  /* 1 = register value newer than stack */
};
struct var_cache vcache[MAX_LOCALS];

/* emit STORE_LOC for a single dirty local and clear dirty flag */
void flush_one_var(long vid)
{
	long idx;
	idx = ir_emit(IR_STORE_LOC, 0, vid, vcache[vid].vreg);
	ir[idx].imm = lvars[vid].width;
	vcache[vid].dirty = 0;
}

void flush_vars(void)
{
	long i, j, best, best_def, lim, nflush;
	long flushed[MAX_LOCALS];

	/* collect dirty vars, then flush in vreg-definition order
	 * so the store order matches the original assignment order.
	 * This lets the decompiler emit assigns in the right sequence. */
	lim = max_vid_used + 1;
	nflush = 0;
	for (i = 0; i < lim; i++) {
		if (vcache[i].vreg >= 0 && vcache[i].dirty) {
#ifdef DEBUG
			if (nflush >= MAX_LOCALS) trap(0x0316, nflush);
#endif
			flushed[nflush] = i;
			nflush++;
		}
	}
	if (nflush == 0)
		return;
	/* insertion sort by vcache[flushed[i]].vreg ascending (skip if single) */
	for (i = 1; i < nflush; i++) {
		best = flushed[i];
		best_def = vcache[best].vreg;
		j = i - 1;
		while (j >= 0 && vcache[flushed[j]].vreg > best_def) {
			flushed[j + 1] = flushed[j];
			j--;
		}
		flushed[j + 1] = best;
	}
	for (i = 0; i < nflush; i++)
		flush_one_var(flushed[i]);
}

void invalidate_all_vars(void)
{
	long i, lim;
	lim = max_vid_used + 1;
	for (i = 0; i < lim; i++) {
		vcache[i].vreg = -1;
		vcache[i].dirty = 0;
	}
}

void flush_and_invalidate(void)
{
	flush_vars();
	invalidate_all_vars();
}

/* AST → IR lowering (linear, pure vreg) */

/* check if AST node ni is a nonzero constant that fits sign-extended imm32 */
long is_foldable_imm(long ni)
{
	if (ast[ni].kind != N_NUM) return 0;
	if (ast[ni].val == 0) return 0;
	if (ast[ni].val < -0x80000000L) return 0;
	if (ast[ni].val > 0x7fffffffL) return 0;
	return 1;
}

long func_is_leaf;

void lower_stmt(long ni);

long lower_expr(long ni)
{
	long k, v, v1, v2, i, lir_op, vid, idx;
	long vaddr, vval;
	long lbl_false, lbl_end, lbl_true, lbl_right;
	long arg_vregs[MAX_PARAMS], nargs;

	k = ast[ni].kind;

	if (k == N_NUM) {
		v = new_vreg();
		ir_emit_imm(IR_LOAD_IMM, v, 0, ast[ni].val);
		return v;
	}
	if (k == N_FUNC_ADDR) {
		v = new_vreg();
		ir_emit(IR_ADDR_FUNC, v, ast[ni].val, 0);
		return v;
	}
	if (k == N_VAR) {
		vid = ast[ni].val;
		if (vid >= GLOBAL_ID_BASE) {
			v = new_vreg();
			idx = ir_emit(IR_LOAD_GLOB, v, vid, 0);
			ir[idx].imm = gvars[vid - GLOBAL_ID_BASE].width;
			return v;
		}
		/* local: return cached vreg if available */
		if (vcache[vid].vreg >= 0)
			return vcache[vid].vreg;
		v = new_vreg();
		idx = ir_emit(IR_LOAD_LOC, v, vid, 0);
		ir[idx].imm = lvars[vid].width;
		vcache[vid].vreg = v;
		return v;
	}
	if (k == N_ADDR) {
		if (ast[ast[ni].a].kind == N_VAR) {
			vid = ast[ast[ni].a].val;
			v = new_vreg();
			if (vid >= GLOBAL_ID_BASE) {
				ir_emit(IR_ADDR_GLOB, v, vid, 0);
			} else {
				/* flush var to stack — address points at stack slot */
				if (vcache[vid].vreg >= 0 && vcache[vid].dirty)
					flush_one_var(vid);
				ir_emit(IR_ADDR_LOC, v, vid, 0);
				/* invalidate — caller may modify via pointer */
				vcache[vid].vreg = -1;
			}
			return v;
		}
		if (ast[ast[ni].a].kind == N_DEREF)
			return lower_expr(ast[ast[ni].a].a);
		return lower_expr(ast[ni].a);
	}
	if (k == N_DEREF) {
		v1 = lower_expr(ast[ni].a);
		v = new_vreg();
		ir_emit(IR_DEREF_LD, v, v1, ast[ni].b);
		return v;
	}
	if (k == N_NEG) {
		v1 = lower_expr(ast[ni].a);
		v = new_vreg();
		ir_emit(IR_NEG, v, v1, 0);
		return v;
	}
	if (k == N_NOT) {
		v1 = lower_expr(ast[ni].a);
		v = new_vreg();
		ir_emit(IR_NOT, v, v1, 0);
		return v;
	}
	if (k == N_LOG_NOT) {
		v1 = lower_expr(ast[ni].a);
		v = new_vreg();
		ir_emit(IR_LNOT, v, v1, 0);
		return v;
	}

	/* binops */
	if (k >= N_ADD && k <= N_XOR) {
		lir_op = IR_ADD + (k - N_ADD);
/* [O2] immediate fold in binops: emit op vreg, imm instead of
 * LOAD_IMM + separate vreg + op vreg, vreg.
 * Saves 3-7 bytes per binop with a constant operand.
 * Disable: #if 0 falls back to LOAD_IMM + vreg. */
#if 1
		/* fold immediate: op vreg, imm (skip LOAD_IMM for s2).
		 * immediate must fit sign-extended imm32 (-0x80000000..0x7fffffff)
		 * since x86 ALU insns only encode 32-bit immediates. */
		if (is_foldable_imm(ast[ni].b) &&
		    k != N_MUL && k != N_DIV_S && k != N_DIV_U &&
		    k != N_MOD_S && k != N_MOD_U &&
		    k != N_SHL && k != N_SHR_S && k != N_SHR_U) {
			v1 = lower_expr(ast[ni].a);
			v = new_vreg();
			ir_emit_imm(lir_op, v, v1, ast[ast[ni].b].val);
			return v;
		}
		/* commutative: fold left immediate */
		if (is_foldable_imm(ast[ni].a) &&
		    (k == N_ADD || k == N_AND || k == N_OR || k == N_XOR)) {
			v2 = lower_expr(ast[ni].b);
			v = new_vreg();
			ir_emit_imm(lir_op, v, v2, ast[ast[ni].a].val);
			return v;
		}
#else
		/* naive: always lower both operands into vregs */
#endif
		v1 = lower_expr(ast[ni].a);
		v2 = lower_expr(ast[ni].b);
		v = new_vreg();
		ir_emit(lir_op, v, v1, v2);
		return v;
	}

	/* comparisons */
	if (k >= N_EQ && k <= N_GE_U) {
		lir_op = IR_EQ + (k - N_EQ);
/* [O2] immediate fold in comparisons: emit cmp vreg, imm instead of
 * LOAD_IMM + cmp vreg, vreg.
 * Saves 3-7 bytes per comparison with a constant operand.
 * Disable: #if 0 falls back to LOAD_IMM + vreg. */
#if 1
		/* fold immediate: cmp vreg, imm.
		 * must fit sign-extended imm32 — x86 CMP has no imm64 form. */
		if (is_foldable_imm(ast[ni].b)) {
			v1 = lower_expr(ast[ni].a);
			v = new_vreg();
			ir_emit_imm(lir_op, v, v1, ast[ast[ni].b].val);
			return v;
		}
#else
		/* naive: always lower both operands into vregs */
#endif
		v1 = lower_expr(ast[ni].a);
		v2 = lower_expr(ast[ni].b);
		v = new_vreg();
		ir_emit(lir_op, v, v1, v2);
		return v;
	}

	/* LOG_AND: eval left in cur label, JZ to false label, eval right
	 * in right label, TEST_SETNE, JMP to end label.
	 * false label: XOR_SELF, fall through to end label. */
	if (k == N_LOG_AND) {
		v1 = lower_expr(ast[ni].a);
		flush_and_invalidate(); /* force reload in right branch (vreg safety) */
		lbl_right = new_label();
		lbl_false = new_label();
		lbl_end = new_label();
		v = new_vreg();
		ir_emit(IR_JZ, 0, v1, lbl_false);
		set_label(lbl_right);
		v2 = lower_expr(ast[ni].b);
		ir_emit(IR_SETNE, v, v2, 0);
		ir_emit(IR_JMP, 0, lbl_end, 0);
		set_label(lbl_false);
		ir_emit(IR_CLR, v, 0, 0);
		ir_emit(IR_JMP, 0, lbl_end, 0);
		set_label(lbl_end);
		invalidate_all_vars(); /* merge point: var cache is stale */
		return v;
	}
	/* LOG_OR: eval left, JNZ to true label, eval right in right label,
	 * JZ to false label. true label: LOAD_IMM 1, JMP end.
	 * false label: XOR_SELF, fall through to end. */
	if (k == N_LOG_OR) {
		v1 = lower_expr(ast[ni].a);
		flush_and_invalidate(); /* force reload in right branch (vreg safety) */
		lbl_right = new_label();
		lbl_true = new_label();
		lbl_false = new_label();
		lbl_end = new_label();
		v = new_vreg();
		ir_emit(IR_JNZ, 0, v1, lbl_true);
		set_label(lbl_right);
		v2 = lower_expr(ast[ni].b);
		ir_emit(IR_JZ, 0, v2, lbl_false);
		set_label(lbl_true);
		ir_emit_imm(IR_LOAD_IMM, v, 0, 1);
		ir_emit(IR_JMP, 0, lbl_end, 0);
		set_label(lbl_false);
		ir_emit(IR_CLR, v, 0, 0);
		ir_emit(IR_JMP, 0, lbl_end, 0);
		set_label(lbl_end);
		invalidate_all_vars(); /* merge point: var cache is stale */
		return v;
	}

	/* assignment */
	if (k == N_ASSIGN) {
		if (ast[ast[ni].a].kind == N_VAR) {
			vid = ast[ast[ni].a].val;
			/* flush target var if dirty — ensures previous assignment
			 * is visible as a separate STORE_LOCAL in the IR,
			 * so the decompiler recovers both statements. */
			if (vid < GLOBAL_ID_BASE && vid < MAX_LOCALS &&
			    vcache[vid].vreg >= 0 && vcache[vid].dirty) {
				flush_one_var(vid);
				vcache[vid].vreg = -1;
			}
			v1 = lower_expr(ast[ni].b);
			if (vid >= GLOBAL_ID_BASE) {
				flush_vars(); /* ensure local stores before global store
					       * so c2d raise sees STORE_LOCAL first */
				idx = ir_emit(IR_STORE_GLOB, 0, vid, v1);
				ir[idx].imm = gvars[vid - GLOBAL_ID_BASE].width;
			} else {
				/* break shared vreg: if v1 is cached for
				 * another local (e.g. v3 = v2 reuses v2's
				 * vreg), invalidate the old owner so next
				 * read emits a fresh LOAD_LOCAL.  This keeps
				 * the one-vreg-one-var invariant needed by
				 * the decompiler. */
				for (i = 0; i < max_vid_used + 1; i++) {
					if (i != vid && vcache[i].vreg == v1)
						vcache[i].vreg = -1;
				}
				vcache[vid].vreg = v1;
				vcache[vid].dirty = 1;
			}
			return v1;
		}
		if (ast[ast[ni].a].kind == N_DEREF) {
			vaddr = lower_expr(ast[ast[ni].a].a);
			vval = lower_expr(ast[ni].b);
			ir_emit(IR_DEREF_ST, ast[ast[ni].a].b, vaddr, vval);
			return vval;
		}
		return lower_expr(ast[ni].b);
	}

	/* function call (direct or indirect): evaluate ALL args first,
	 * then emit IR_ARGs. Prevents interleaving from nested calls. */
	if (k == N_CALL || k == N_CALL_IND) {
		func_is_leaf = 0;
		flush_vars(); /* callee may read via pointers */
		nargs = ast[ni].b;
#ifdef DEBUG
		if (nargs > MAX_PARAMS)
			trap(0x0102, nargs);
#endif
		if (k == N_CALL) {
			for (i = 0; i < nargs; i++)
				arg_vregs[i] = lower_expr(ast[ni].a + i);
		} else {
			for (i = 0; i < nargs; i++)
				arg_vregs[i] = lower_expr(ast[ni].val + i);
			v1 = lower_expr(ast[ni].a); /* callee address */
		}
		for (i = 0; i < nargs; i++)
			ir_emit(IR_ARG, 0, arg_vregs[i], i);
		v = new_vreg();
		if (k == N_CALL)
			ir_emit(IR_CALL, v, ast[ni].val, nargs);
		else
			ir_emit(IR_CALL_IND, v, v1, nargs);
		/* invalidate vcache (already flushed before call) */
		invalidate_all_vars();
		return v;
	}

#ifdef DEBUG
	trap(0x0100, k);
#endif
	return 0;
}

/* break/continue: store target label ids, patch at loop end */
long brk_target;     /* label for break destination, or -1 */
long cont_target;    /* label for continue destination, or -1 */

void lower_stmt(long ni)
{
	long k, i, v;
	long lbl_then, lbl_else, lbl_end2, lbl_top, lbl_cond, lbl_exit, lbl_body;
	long save_brk, save_cont;

	k = ast[ni].kind;

	if (k == N_BLOCK) {
		for (i = 0; i < ast[ni].b; i++) {
			lower_stmt(ast[ni].a + i);
			flush_vars();
		}
		return;
	}
	if (k == N_DECL) return;

	if (k == N_RETURN) {
		if (ast[ni].a) {
			v = lower_expr(ast[ni].a);
			ir_emit(IR_RET, 0, v, 0);
		} else {
			ir_emit(IR_RET_VOID, 0, 0, 0);
		}
		invalidate_all_vars();
		return;
	}

	if (k == N_IF) {
		v = lower_expr(ast[ni].a);
		flush_and_invalidate();
		if (ast[ni].val) {
			lbl_then = new_label();
			lbl_else = new_label();
			lbl_end2 = new_label();
			ir_emit(IR_JZ, 0, v, lbl_else);
			set_label(lbl_then);
			lower_stmt(ast[ni].b);
			flush_and_invalidate();
			ir_emit(IR_JMP, 0, lbl_end2, 0);
			set_label(lbl_else);
			lower_stmt(ast[ni].val);
			flush_and_invalidate();
			ir_emit(IR_JMP, 0, lbl_end2, 0);
			set_label(lbl_end2);
		} else {
			lbl_then = new_label();
			lbl_end2 = new_label();
			ir_emit(IR_JZ, 0, v, lbl_end2);
			set_label(lbl_then);
			lower_stmt(ast[ni].b);
			flush_and_invalidate();
			ir_emit(IR_JMP, 0, lbl_end2, 0);
			set_label(lbl_end2);
		}
		return;
	}

	if (k == N_BREAK) {
		flush_and_invalidate();
		ir_emit(IR_JMP, 0, brk_target, 0);
		/* start dead block for any code after break */
		set_label(new_label());
		return;
	}
	if (k == N_CONTINUE) {
		flush_and_invalidate();
		ir_emit(IR_JMP, 0, cont_target, 0);
		set_label(new_label());
		return;
	}

	/* top: cond; JZ exit; body; JMP top; exit: */
	if (k == N_WHILE) {
		flush_and_invalidate();
		save_brk = brk_target;
		save_cont = cont_target;
		lbl_top = new_label();    /* loop header: eval condition */
		lbl_body = new_label();
		lbl_exit = new_label();
		brk_target = lbl_exit;
		cont_target = lbl_top;
		ir_emit(IR_JMP, 0, lbl_top, 0);
		set_label(lbl_top);
		v = lower_expr(ast[ni].a);
		ir_emit(IR_JZ, 0, v, lbl_exit);
		set_label(lbl_body);
		lower_stmt(ast[ni].b);
		flush_and_invalidate();
		ir_emit(IR_JMP, 0, lbl_top, 0);
		set_label(lbl_exit);
		invalidate_all_vars(); /* break may jump here; var cache is stale */
		brk_target = save_brk;
		cont_target = save_cont;
		return;
	}

	/* top: body; JMP cond; cond: expr; JNZ top; exit: */
	if (k == N_DO_WHILE) {
		flush_and_invalidate();
		save_brk = brk_target;
		save_cont = cont_target;
		lbl_top = new_label();    /* body start */
		lbl_cond = new_label();   /* condition block */
		lbl_exit = new_label();
		brk_target = lbl_exit;
		cont_target = lbl_cond;
		ir_emit(IR_JMP, 0, lbl_top, 0);
		set_label(lbl_top);
		lower_stmt(ast[ni].a);
		flush_and_invalidate();
		ir_emit(IR_JMP, 0, lbl_cond, 0);
		set_label(lbl_cond);
		v = lower_expr(ast[ni].b);
		ir_emit(IR_JNZ, 0, v, lbl_top);
		set_label(lbl_exit);
		invalidate_all_vars(); /* break may jump here; var cache is stale */
		brk_target = save_brk;
		cont_target = save_cont;
		return;
	}

	/* expression statement */
	lower_expr(ni);
}

#ifdef MUT_SCHEDULE
/*
 * MUT_SCHEDULE — reorder independent IR instructions within basic blocks.
 *
 * Pure computations (LOAD_IMM, ADD, MUL, etc.) are found by def-use
 * chains in the decompiler, not by position.  Swapping two adjacent
 * pure instructions that have no data dependency changes register
 * pressure and thus the x86 output, without affecting semantics.
 *
 * Barriers (STORE_*, DEREF_STORE, JMP, JZ, JNZ, RET, CALL, ARG,
 * FUNC_ENTRY, SAVE_PARAM, XOR_SELF, NOP) are never moved.
 * DEREF_LOAD is movable but not across a DEREF_STORE.
 *
 * Called once per function, after lower_func(), before regalloc.
 */

long sched_is_pure(long i)
{
	long op;
	op = ir[i].op;
	if (op == IR_LOAD_IMM || op == IR_LOAD_LOC || op == IR_LOAD_GLOB)
		return 1;
	if (op == IR_ADDR_LOC || op == IR_ADDR_GLOB || op == IR_ADDR_FUNC)
		return 1;
	if (op == IR_DEREF_LD)
		return 1;
	if (op >= IR_ADD && op <= IR_XOR)
		return 1;
	if (op == IR_NEG || op == IR_NOT)
		return 1;
	if (op >= IR_EQ && op <= IR_GEU)
		return 1;
	if (op == IR_LNOT || op == IR_SETNE)
		return 1;
	return 0;
}

/* precomputed by mutate_schedule before the swap loop */
int sched_label_tgt[MAX_IR];

void sched_build_label_tgt(void)
{
	long i;
	for (i = 0; i < nir; i++)
		sched_label_tgt[i] = 0;
	for (i = 0; i < nlabels; i++) {
		if (label_ir[i] >= 0 && label_ir[i] < nir)
			sched_label_tgt[label_ir[i]] = 1;
	}
}

long sched_is_label_target(long idx)
{
	return sched_label_tgt[idx];
}

long sched_has_dep(long a, long b)
{
	/* a reads what b defines, or b reads what a defines */
	if (ir[a].dst > 0) {
		if (ir[b].s1 == ir[a].dst) return 1;
		if (ir[b].s2 == ir[a].dst && ir[b].imm == 0) return 1;
	}
	if (ir[b].dst > 0) {
		if (ir[a].s1 == ir[b].dst) return 1;
		if (ir[a].s2 == ir[b].dst && ir[a].imm == 0) return 1;
	}
	/* same dst = output dep */
	if (ir[a].dst > 0 && ir[a].dst == ir[b].dst)
		return 1;
	/* DEREF_LOAD + DEREF_LOAD: fine (both reads).
	 * DEREF_LOAD + DEREF_STORE would not happen (store is not pure). */
	return 0;
}

void mutate_schedule(void)
{
	long i;
	struct insn tmp;

	sched_build_label_tgt();
	i = 0;
	while (i + 1 < nir) {
		if (!sched_is_pure(i) || !sched_is_pure(i + 1)) {
			i++;
		} else if (sched_is_label_target(i + 1)) {
			i++;
		} else if (sched_has_dep(i, i + 1)) {
			i++;
		} else if (rng() & 1) {
			/* swap adjacent independent pure instructions */
			tmp = ir[i];
			ir[i] = ir[i + 1];
			ir[i + 1] = tmp;
			i = i + 2; /* skip the pair we just swapped */
		} else {
			i++;
		}
	}
}
#endif

#include "c2_codegen.c"

void lower_func(long ni)
{
	long fid, nparams, body_ni, i;
	long ba, bc;

	fid = ast[ni].val;
	nparams = ast[ni].b;
	body_ni = ast[ni].a;
	build_local_offsets(body_ni);
	func_is_leaf = 1;
	invalidate_all_vars();

#ifdef DEBUG
	if (nparams > MAX_PARAMS)
		trap(0x0314, nparams);
#endif
	ir_emit(IR_FUNC, fid, frame_size, nparams);
	for (i = 0; i < nparams && i < MAX_PARAMS; i++)
		ir_emit(IR_PARAM, ast[ast[body_ni].a + i].val, 0, i);

	ba = ast[body_ni].a;
	bc = ast[body_ni].b;
	for (i = 0; i < bc; i++) {
		if (ast[ba + i].kind != N_DECL) {
			lower_stmt(ba + i);
			flush_vars();
		}
	}

	/* flush locals that were never spilled to stack — ensures every
	 * local's displacement appears in IR (decompiler needs this) */
	flush_vars();

	if (bc > 0) {
		if (ast[ba + bc - 1].kind != N_RETURN)
			ir_emit(IR_RET_VOID, 0, 0, 0);
	} else {
		ir_emit(IR_RET_VOID, 0, 0, 0);
	}
}

long compile(long in, long in_len, long out_buf)
{
	long i, entry_fix, ta, tb;

	ast = (struct node *)in;
	out = (char *)out_buf;
	pos = 0;
	code_base = CODE_BASE;
	bss_base = BSS_BASE;
	nfix = 0;

	build_global_offsets();
	init_param_regs();
	init_liveness_tables();

	/* entry: call main; hlt */
	eb(0xe8);
	entry_fix = pos;
	ed(0);
	eb(0xf4);

	ta = ast[0].a;
	tb = ast[0].b;
	for (i = 0; i < tb; i++) {
		if (ast[ta + i].kind == N_FUNC) {
			nir = 0;
			nlabels = 0;
			next_vreg = 1;
			njmp_fix = 0;
			brk_target = -1;
			cont_target = -1;
			lower_func(ta + i);
#ifdef MUT_SCHEDULE
			if (rng_state)
				mutate_schedule();
#endif
#ifdef MEASURE
			{
				extern long wm_nir_compile, wm_next_vreg_comp, wm_nlabels;
				if (nir > wm_nir_compile) wm_nir_compile = nir;
				if (next_vreg > wm_next_vreg_comp) wm_next_vreg_comp = next_vreg;
				if (nlabels > wm_nlabels) wm_nlabels = nlabels;
			}
#endif
				init_regalloc();
			compute_liveness();
			run_regalloc();

			/* always emit sub rsp when locals exist, even for leaf
			 * functions. This ensures frame_size is visible in the
			 * binary for decompilation (c2d recovers local count
			 * from sub rsp). Without this, leaf functions with
			 * register-resident locals have no stack trace. */
			if (func_is_leaf && frame_size == 0)
				total_frame = 0;
			else
				total_frame = frame_size - spill_off;


#ifdef DUMP_IR
			dump_ir_func();
#endif
#ifdef MEASURE
			{
				extern long wm_total_frame, wm_spill_slots, wm_njmp_fix, wm_nfix;
				if (total_frame > wm_total_frame) wm_total_frame = total_frame;
				if (-spill_off / 8 > wm_spill_slots) wm_spill_slots = -spill_off / 8;
			}
#endif
			emit_ir();
			resolve_jumps();
#ifdef MEASURE
			{
				extern long wm_njmp_fix, wm_nfix;
				if (njmp_fix > wm_njmp_fix) wm_njmp_fix = njmp_fix;
				if (nfix > wm_nfix) wm_nfix = nfix;
			}
#endif
		}
	}

	write32(entry_fix, func_off[0] - (entry_fix + 4));
	resolve_fixups();
	return pos;
}
