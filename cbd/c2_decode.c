/*
 * c2_decode.c — x86-64 binary → IR decoder
 *
 * Shared state for c2d (decompiler) and c2m (metamorphic engine).
 * Included by c2d.c and c2m.c.
 *
 * Pass 0: scan_functions — find function prologues, frame sizes
 * Pass 1: decode_function — linear x86 decode into IR per function
 *
 * Register tracking: reg_vreg[R] (int array) maps physical register to
 * current vreg.  Push/pop around calls tracked via save_stack[] (int array).
 * Comparison operands saved for deferred setcc → IR emit.
 *
 * insn_off[] is an int array mapping IR index → code offset.
 * struct func fields are int; struct dec fields are int except disp/imm (long).
 */

unsigned char *code;
long code_len;

struct node *ast_out;
long nn;

#include "ir.h"

struct insn ir[MAX_IR];
long nir;
long next_vreg;

/* maps IR index to code offset; int suffices since code_len fits in 32 bits */
int insn_off[MAX_IR];
long cur_decode_off;

struct func { int off; int end; int npar; int frame; int fid; int _pad; };
struct func funcs[MAX_FUNC];
long nfuncs;

struct loop { int top; int cond; int exit; int back; };
struct loop loops[MAX_LOOPS];
long nloops;

struct dowhile { int top; int cond; int cond_start; int exit; };
struct dowhile dws[MAX_LOOPS];
long ndowhiles;

long rb(long off) { return code[off]; }
/* decoded instruction fields */
struct dec {
	int opc;       /* primary opcode */
	int op2;       /* secondary opcode (for 0F xx) */
	int rex;       /* REX byte (0 = none) */
	int p66;       /* 0x66 operand-size prefix present */
	int len;       /* total instruction length */
	int modrm;     /* ModRM byte */
	int mod;       /* ModRM mod field */
	int reg;       /* ModRM reg field (extended by REX.R) */
	int rm;        /* ModRM rm field (extended by REX.B) */
	long disp;     /* displacement — can be large (rip-relative) */
	long imm;      /* immediate value — can be 64-bit */
};

/* width from 0x66 prefix and REX.W: 0x66→2, REX.W→8, else 4 */
long mov_width(struct dec *d)
{
	if (d->p66) return 2;
	if (d->rex & 8) return 8;
	return 4;
}

/* sign-extend 32-bit value to 64-bit */
long sext32(long v)
{
	if (v & 0x80000000)
		v |= -1L << 32;
	return v;
}

/* parse ModRM + optional SIB + displacement, return offset past consumed bytes.
 * SIB handling: when mod != 3 and rm & 7 == 4, a SIB byte follows ModRM.
 * Our codegen emits SIB=0x24 (base=rsp/r12, index=none) for [rsp]/[r12],
 * so we read and skip the SIB, keeping rm = base register. */
long parse_modrm(long p, struct dec *d)
{
	d->modrm = rb(p);
	d->mod = (d->modrm >> 6) & 3;
	d->reg = (d->modrm >> 3) & 7;
	if (d->rex & 4) d->reg |= 8;
	d->rm = d->modrm & 7;
	if (d->rex & 1) d->rm |= 8;
	p++;
	/* SIB byte present when mod != 3 and rm & 7 == 4 */
	if (d->mod != 3 && (d->modrm & 7) == 4) {
		/* SIB: base = low 3 bits, extended by REX.B */
		d->rm = rb(p) & 7;
		if (d->rex & 1) d->rm |= 8;
		p++;
	}
	d->disp = 0;
	if (d->mod == 1) {
		d->disp = rb(p);
		if (d->disp & 0x80) d->disp |= -1L << 8;
		p++;
	} else if (d->mod == 2 || (d->mod == 0 && (d->modrm & 7) == 5)) {
		d->disp = sext32(rb(p) | (rb(p+1) << 8) | (rb(p+2) << 16) | (rb(p+3) << 24));
		p += 4;
	}
	return p;
}

/* decode x86 instruction at off into struct dec, return instruction length */
long x86_decode(long off, struct dec *d)
{
	long p, op;

	p = off;
	d->p66 = 0;
	d->rex = 0;
	d->op2 = 0;
	d->imm = 0;

	if (rb(p) == 0x66) { d->p66 = 1; p++; }
	if (rb(p) >= 0x40 && rb(p) <= 0x4F) { d->rex = rb(p); p++; }

	op = rb(p);
	d->opc = op;
	p++;

	/* single-byte */
	if (op == 0x55 || op == 0xC9 || op == 0xC3 || op == 0xF4 || op == 0x99) {
		d->len = p - off; return d->len;
	}
	if (op >= 0x50 && op <= 0x5F) {
		d->len = p - off; return d->len;
	}

	/* FF with mod=3: INC /0, DEC /1, CALL /2 (register operand) */
	if (op == 0xFF && (rb(p) & 0xC0) == 0xC0) {
		d->modrm = rb(p); p++;
		d->mod = 3;
		d->reg = (d->modrm >> 3) & 7;
		d->rm = d->modrm & 7;
		if (d->rex & 1) d->rm |= 8;
		d->len = p - off; return d->len;
	}

	/* rel32: E8 call, E9 jmp */
	if (op == 0xE8 || op == 0xE9) {
		d->imm = sext32(rb(p) | (rb(p+1)<<8) | (rb(p+2)<<16) | (rb(p+3)<<24));
		p += 4;
		d->len = p - off; return d->len;
	}

	/* B8+r: mov reg, imm32/imm64 */
	if (op >= 0xB8 && op <= 0xBF) {
		if (d->rex & 8) {
			d->imm = rb(p) | (rb(p+1)<<8) | (rb(p+2)<<16) | (rb(p+3)<<24) |
			         (rb(p+4)<<32) | (rb(p+5)<<40) |
			         (rb(p+6)<<48) | (rb(p+7)<<56);
			p += 8;
		} else {
			d->imm = rb(p) | (rb(p+1)<<8) | (rb(p+2)<<16) | (rb(p+3)<<24);
			p += 4;
		}
		d->len = p - off; return d->len;
	}

	/* 0F xx: two-byte opcode */
	if (op == 0x0F) {
		d->op2 = rb(p); p++;
		if (d->op2 >= 0x80 && d->op2 <= 0x8F) {
			d->imm = sext32(rb(p) | (rb(p+1)<<8) | (rb(p+2)<<16) | (rb(p+3)<<24));
			p += 4;
			d->len = p - off; return d->len;
		}
		if (d->op2 >= 0x90 && d->op2 <= 0x9F) {
			p = parse_modrm(p, d);
			d->len = p - off; return d->len;
		}
		if (d->op2 == 0xB6 || d->op2 == 0xB7 || d->op2 == 0xAF) {
			p = parse_modrm(p, d);
			d->len = p - off; return d->len;
		}
#ifdef DEBUG
		trap(0x0407, d->op2);
#endif
		d->len = p - off; return d->len;
	}

	/* C7 /0: mov r/m, imm32 */
	if (op == 0xC7) {
		p = parse_modrm(p, d);
		d->imm = sext32(rb(p) | (rb(p+1)<<8) | (rb(p+2)<<16) | (rb(p+3)<<24));
		p += 4;
		d->len = p - off; return d->len;
	}

	/* 81/83: ALU imm */
	if (op == 0x81) {
		p = parse_modrm(p, d);
		d->imm = sext32(rb(p) | (rb(p+1)<<8) | (rb(p+2)<<16) | (rb(p+3)<<24));
		p += 4;
		d->len = p - off; return d->len;
	}
	if (op == 0x83) {
		p = parse_modrm(p, d);
		d->imm = rb(p); if (d->imm & 0x80) d->imm |= -1L << 8;
		p++;
		d->len = p - off; return d->len;
	}

	/* instructions with ModRM */
	if (op == 0x89 || op == 0x8B || op == 0x8D ||
	    op == 0x63 || op == 0x88 ||
	    op == 0x01 || op == 0x29 || op == 0x21 ||
	    op == 0x09 || op == 0x31 || op == 0x39 ||
	    op == 0x85 || op == 0xD3 || op == 0xF7) {
		p = parse_modrm(p, d);
		d->len = p - off; return d->len;
	}

#ifdef DEBUG
	trap(0x0400, off);
#endif
	d->len = 1; return 1;
}

/* function table */

/* pass 0: find function prologues (push rbp; mov rbp,rsp), record frame size from sub rsp */
void scan_functions(void)
{
	long off;
	struct dec sd;

	nfuncs = 0;
	off = 0;
	while (off < code_len) {
		/* look for prologue: push rbp (55); mov rbp,rsp (48 89 E5) */
		if (rb(off) != 0x55 || off + 3 >= code_len ||
		    rb(off+1) != 0x48 || rb(off+2) != 0x89 || rb(off+3) != 0xE5) {
			x86_decode(off, &sd);
			off += sd.len;
			continue;
		}
#ifdef DEBUG
		if (nfuncs >= MAX_FUNC)
			trap(0x0401, nfuncs);
#endif
		if (nfuncs > 0)
			funcs[nfuncs - 1].end = off;
		funcs[nfuncs].off = off;
		funcs[nfuncs].end = code_len;
		funcs[nfuncs].npar = 0;
		funcs[nfuncs].frame = 0;
		/* sub rsp, imm follows prologue? */
		if (off + 4 < code_len) {
			x86_decode(off + 4, &sd);
			if (sd.opc == 0x81 && sd.reg == 5)
				funcs[nfuncs].frame = sd.imm;
			else if (sd.opc == 0x83 && sd.reg == 5)
				funcs[nfuncs].frame = sd.imm;
		}
		nfuncs++;
		off += 4;
	}
}

/* forward declarations for functions defined in c2_decompile.c */
long em(long kind, long a, long b_, long val);
void reset_locals(void);
long disp_to_vid(long disp);

/* vreg operand tables */

int s1_vreg_tab[64];
int s2_vreg_tab[64];

/* build lookup tables: which IR opcodes have vreg operands in s1/s2.
 * Must be called once before decode. No guard — caller ensures single call. */
void build_vreg_tabs(void)
{
	long i;
	for (i = 0; i < 64; i++) {
		s1_vreg_tab[i] = 0;
		s2_vreg_tab[i] = 0;
	}
	for (i = IR_ADD; i <= IR_XOR; i++) s1_vreg_tab[i] = 1;
	for (i = IR_EQ; i <= IR_GEU; i++) s1_vreg_tab[i] = 1;
	s1_vreg_tab[IR_NEG] = 1;
	s1_vreg_tab[IR_NOT] = 1;
	s1_vreg_tab[IR_LNOT] = 1;
	s1_vreg_tab[IR_SETNE] = 1;
	s1_vreg_tab[IR_DEREF_LD] = 1;
	s1_vreg_tab[IR_DEREF_ST] = 1;
	s1_vreg_tab[IR_RET] = 1;
	s1_vreg_tab[IR_ARG] = 1;
	s1_vreg_tab[IR_JZ] = 1;
	s1_vreg_tab[IR_JNZ] = 1;
	s1_vreg_tab[IR_CALL_IND] = 1;
	for (i = IR_ADD; i <= IR_XOR; i++) s2_vreg_tab[i] = 1;
	for (i = IR_EQ; i <= IR_GEU; i++) s2_vreg_tab[i] = 1;
	s2_vreg_tab[IR_STORE_LOC] = 1;
	s2_vreg_tab[IR_STORE_GLOB] = 1;
	s2_vreg_tab[IR_DEREF_ST] = 1;
}

long s1_is_vreg(long idx)
{
	long op;
	op = ir[idx].op;
#ifdef DEBUG
	if (op < 0 || op >= 64) trap(0x0420, op);
#endif
	return s1_vreg_tab[op];
}

long s2_is_vreg(long idx)
{
	long op;
	op = ir[idx].op;
#ifdef DEBUG
	if (op < 0 || op >= 64) trap(0x0420, op);
#endif
	return s2_vreg_tab[op];
}

/* map code offset to IR instruction index (binary search, insn_off[] is sorted) */
long off_to_ir(long code_off)
{
	long lo, hi, mid;
	lo = 0;
	hi = nir;
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (insn_off[mid] < code_off)
			lo = mid + 1;
		else
			hi = mid;
	}
	/* lo = first index where insn_off[lo] >= code_off */
	if (lo < nir && insn_off[lo] == code_off)
		return lo;
#ifdef DEBUG
	trap(0x0432, code_off); /* jump target not on IR boundary */
#endif
	if (lo > 0) return lo;
	return 0;
}

/* map code offset to function ID (binary search, funcs[] sorted by off) */
long off_to_fid(long code_off)
{
	long lo, hi, mid;
	lo = 0;
	hi = nfuncs;
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (funcs[mid].off < code_off)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < nfuncs && funcs[lo].off == code_off)
		return funcs[lo].fid;
	return 0;
}

/* convert code offsets in JMP/JZ/JNZ/CALL/ADDR_FUNC to IR indices / function IDs */
void normalize_targets(void)
{
	long i;
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_JZ || ir[i].op == IR_JNZ)
			ir[i].s2 = off_to_ir(ir[i].s2);
		if (ir[i].op == IR_JMP)
			ir[i].s1 = off_to_ir(ir[i].s1);
		if (ir[i].op == IR_CALL)
			ir[i].s1 = off_to_fid(ir[i].s1);
		if (ir[i].op == IR_ADDR_FUNC)
			ir[i].s1 = off_to_fid(ir[i].s1);
	}
}

long rax_explicit_ret;

/* emit IR instruction during decode, recording code offset for later target resolution */
long ir_emit_d(long op, long dst, long s1, long s2)
{
	long idx;
#ifdef DEBUG
	if (nir >= MAX_IR) trap(0x0300, nir);
#endif
	idx = nir;
	ir[nir].op = op;
	ir[nir].dst = dst;
	ir[nir].s1 = s1;
	ir[nir].s2 = s2;
	ir[nir].imm = 0;
	insn_off[nir] = cur_decode_off;
	nir++;
	rax_explicit_ret = 0;
	return idx;
}

/* emit IR with immediate value in imm field (for LOAD_IMM) */
long ir_emit_d_imm(long op, long dst, long s1, long imm_val)
{
	long idx;
	idx = ir_emit_d(op, dst, s1, 0);
	ir[idx].imm = imm_val;
	return idx;
}

long new_vreg_d(void)
{
	long v;
#ifdef DEBUG
	if (next_vreg >= MAX_VREG) trap(0x0301, next_vreg);
#endif
	v = next_vreg;
	next_vreg++;
	return v;
}

/* register tracking: reg_vreg[R] = vreg currently in physical reg R, or -1.
 * int arrays — vreg IDs and boolean flags fit in 32 bits. */
#define N_REGS 16
int reg_vreg[N_REGS];
int reg_set_after_call[N_REGS]; /* 1 if reg was set since last call */

/* last cmp/test operands for setcc */
long cmp_v1, cmp_v2; /* -1 = test (same reg), else cmp operands */
long last_cmp_vreg;  /* vreg holding comparison result (for JZ/JNZ) */

/* setcc clobber tracking: when setcc overwrites a register, save the
 * old vreg so it can be restored after test consumes the comparison. */
long setcc_reg;       /* physical register written by setcc, or -1 */
long setcc_old_vreg;  /* vreg that was in setcc_reg before overwrite */

/* push/pop save stack for caller-save tracking.
 * int array — stores vreg IDs which fit in 32 bits. */
int save_stack[32]; /* peak 11 */
long save_sp;

/* clear all register tracking state for a new function */
void reset_regs(void)
{
	long i;
	for (i = 0; i < N_REGS; i++) {
		reg_vreg[i] = -1;
		reg_set_after_call[i] = 0;
	}
	save_sp = 0;
	cmp_v1 = -1;
	cmp_v2 = -1;
	last_cmp_vreg = -1;
	setcc_reg = -1;
	setcc_old_vreg = -1;
	rax_explicit_ret = 0;
}

void mark_reg_set(long r)
{
#ifdef DEBUG
	if (r < 0 || r >= N_REGS) trap(0x0413, r);
#endif
	reg_set_after_call[r] = 1;
}

/* helper: decode width-aware load (global/local/deref) from struct dec.
 * Handles RIP-relative / RBP-relative / [reg] dispatch. */
long decode_load_w(long off, struct dec *d, long w)
{
	long v, idx;
	if (d->mod == 0 && (d->modrm & 7) == 5) {
		v = new_vreg_d();
		idx = ir_emit_d(IR_LOAD_GLOB, v, off + d->len + d->disp, 0);
		ir[idx].imm = w;
		reg_vreg[d->reg] = v;
		mark_reg_set(d->reg);
		return off + d->len;
	}
	if ((d->modrm & 7) == 5) {
		v = new_vreg_d();
		idx = ir_emit_d(IR_LOAD_LOC, v, d->disp, 0);
		ir[idx].imm = w;
		reg_vreg[d->reg] = v;
		mark_reg_set(d->reg);
		return off + d->len;
	}
	if (d->mod == 0) {
		v = new_vreg_d();
		ir_emit_d(IR_DEREF_LD, v, reg_vreg[d->rm], w);
		reg_vreg[d->reg] = v;
		mark_reg_set(d->reg);
		return off + d->len;
	}
#ifdef DEBUG
	trap(0x0430, d->mod);
#endif
	return off + d->len;
}

/* helper: decode width-aware store (global/local/deref) from struct dec. */
long decode_store_w(long off, struct dec *d, long w, long val_reg)
{
	long idx;
	if (d->mod == 0 && (d->modrm & 7) == 5) {
		idx = ir_emit_d(IR_STORE_GLOB, 0, off + d->len + d->disp, reg_vreg[val_reg]);
		ir[idx].imm = w;
		return off + d->len;
	}
	if ((d->modrm & 7) == 5) {
		idx = ir_emit_d(IR_STORE_LOC, 0, d->disp, reg_vreg[val_reg]);
		ir[idx].imm = w;
		return off + d->len;
	}
	if (d->mod == 0) {
		ir_emit_d(IR_DEREF_ST, w, reg_vreg[d->rm], reg_vreg[val_reg]);
		return off + d->len;
	}
#ifdef DEBUG
	trap(0x0431, d->mod);
#endif
	return off + d->len;
}

/* decode one x86 instruction at off into IR.
 * Returns offset past the instruction. */
long decode_insn(long off, long fn_start, long fn_end)
{
	struct dec d;
	long opc, r_dst, v, v1, v2, target;
	long nargs, ai, cc_kind;
	long param_r[MAX_PARAMS];
	long scan, found, pop_count;
	long store_w;
	long last_ri, last_real_op;

	/* SysV ABI: rdi=7, rsi=6, rdx=2, rcx=1, r8=8, r9=9 */
	param_r[0] = 7; param_r[1] = 6; param_r[2] = 2;
	param_r[3] = 1; param_r[4] = 8; param_r[5] = 9;

	cur_decode_off = off;
	x86_decode(off, &d);
	opc = d.opc;

	/* push rbp (55) — skip, part of prologue */
	if (opc == 0x55) return off + d.len;

	/* leave (C9) + ret (C3) */
	if (opc == 0xC9) return off + d.len; /* leave handled at ret */
	if (opc == 0xC3) {
		/* check if preceded by leave */
		if (off > fn_start && rb(off - 1) == 0xC9) {
			/* Heuristic: if last emitted IR is a statement (STORE
			 * or CALL), rax is a stale side-effect → RET_VOID.
			 * Exception: if there was a reg-to-rax move AFTER the
			 * last IR emission (explicit return value setup), the
			 * move doesn't emit IR but sets rax_explicit_ret. */
			/* find last real IR (skip past NOPs) */
			last_ri = nir - 1;
			while (last_ri >= 0 && ir[last_ri].op == IR_NOP)
				last_ri--;
			last_real_op = 0;
			if (last_ri >= 0) last_real_op = ir[last_ri].op;
			if (reg_vreg[0] >= 0 && rax_explicit_ret)
				ir_emit_d(IR_RET, 0, reg_vreg[0], 0);
			else if (reg_vreg[0] >= 0 && last_ri >= 0 &&
			    last_real_op != IR_STORE_LOC &&
			    last_real_op != IR_STORE_GLOB &&
			    last_real_op != IR_DEREF_ST &&
			    last_real_op != IR_CALL &&
			    last_real_op != IR_JMP &&
			    last_real_op != IR_JZ &&
			    last_real_op != IR_JNZ &&
			    last_real_op != IR_RET &&
			    last_real_op != IR_RET_VOID)
				ir_emit_d(IR_RET, 0, reg_vreg[0], 0);
			else
				ir_emit_d(IR_RET_VOID, 0, 0, 0);
		}
		return off + d.len;
	}

	/* push Rx (50+r, REX.B extends to r8-r15) — caller-save: track on stack */
	if (opc >= 0x50 && opc <= 0x57) {
		r_dst = opc - 0x50;
		if (d.rex & 1) r_dst |= 8;
#ifdef DEBUG
		if (save_sp >= 32) trap(0x0414, save_sp);
#endif
		save_stack[save_sp] = reg_vreg[r_dst];
		save_sp++;
#ifdef MEASURE
		{ extern long wm_save_sp; if (save_sp > wm_save_sp) wm_save_sp = save_sp; }
#endif
		return off + d.len;
	}

	/* pop Rx (58+r, REX.B extends to r8-r15) — caller-restore: restore from stack */
	if (opc >= 0x58 && opc <= 0x5F) {
#ifdef DEBUG
		if (save_sp <= 0)
			trap(0x0414, opc);
#endif
		save_sp--;
		r_dst = opc - 0x58;
		if (d.rex & 1) r_dst |= 8;
		reg_vreg[r_dst] = save_stack[save_sp];
		return off + d.len;
	}

	/* E8 rel32: call */
	if (opc == 0xE8) {
		target = off + d.len + d.imm;
		/* count args from callee's param count (most reliable) */
		nargs = 0;
		found = 0;
		for (ai = 0; ai < nfuncs; ai++) {
			if (funcs[ai].off == target) {
				nargs = funcs[ai].npar;
				found = 1;
				break;
			}
		}
		/* fallback: count pop-rax+mov-reg,rax sequences before call */
		if (!found) {
			scan = off - 1;
			pop_count = 0;
			while (scan >= 6) {
				if (rb(scan - 2) == 0x48 && rb(scan - 1) == 0x89 &&
				    (rb(scan) & 0xF8) == 0xC0 && rb(scan) != 0xC0) {
					if (scan >= 3 && rb(scan - 3) == 0x58) {
						pop_count++;
						scan -= 4;
						continue;
					}
				}
				break;
			}
			if (pop_count > 0)
				nargs = pop_count;
			else if (reg_vreg[7] >= 0 && reg_set_after_call[7])
				nargs = 1;
		}
		/* emit ARG insns for each arg */
		for (ai = 0; ai < nargs; ai++)
			ir_emit_d(IR_ARG, 0, reg_vreg[param_r[ai]], ai);
		v = new_vreg_d();
		ir_emit_d(IR_CALL, v, target, nargs);
		reg_vreg[0] = v;
		mark_reg_set(0);
		/* callee clobbers param regs + scratch */
		for (ai = 0; ai < N_REGS; ai++)
			reg_set_after_call[ai] = 0;
		return off + d.len;
	}

	/* FF /0 mod=3: INC, FF /1 mod=3: DEC.
	 * Emit as ADD/SUB with folded immediate 1, matching the compiler's
	 * O2 path (1 IR insn instead of LOAD_IMM + binop). */
	if (opc == 0xFF && d.mod == 3 && (d.reg == 0 || d.reg == 1)) {
		v1 = reg_vreg[d.rm];
		v = new_vreg_d();
		if (d.reg == 0)
			ir_emit_d_imm(IR_ADD, v, v1, 1);
		else
			ir_emit_d_imm(IR_SUB, v, v1, 1);
		reg_vreg[d.rm] = v;
		mark_reg_set(d.rm);
		return off + d.len;
	}

	/* FF /2 mod=3: call rax (indirect call) */
	if (opc == 0xFF && d.len >= 2 && rb(off + 1) == 0xD0) {
		/* count args: check which param regs were set since last call */
		nargs = 0;
		for (ai = 0; ai < MAX_PARAMS; ai++) {
			if (reg_vreg[param_r[ai]] >= 0 && reg_set_after_call[param_r[ai]])
				nargs = ai + 1;
			else
				break;
		}
		for (ai = 0; ai < nargs; ai++)
			ir_emit_d(IR_ARG, 0, reg_vreg[param_r[ai]], ai);
		v = new_vreg_d();
		ir_emit_d(IR_CALL_IND, v, reg_vreg[0], nargs);
		reg_vreg[0] = v;
		mark_reg_set(0);
		for (ai = 0; ai < N_REGS; ai++)
			reg_set_after_call[ai] = 0;
		return off + 2; /* FF D0 = 2 bytes */
	}

	/* E9 rel32: jmp */
	if (opc == 0xE9) {
		target = off + d.len + d.imm;
		ir_emit_d(IR_JMP, 0, target, 0); /* target resolved to block later */
		/* NOTE: reg_vreg reset is done in decode_function after
		 * the jump (linear decode, no register merging). */
		return off + d.len;
	}

	/* 0F xx: two-byte opcode */
	if (opc == 0x0F) {

		/* 0F 84 rel32: je */
		if (d.op2 == 0x84) {
			target = off + d.len + d.imm;
			ir_emit_d(IR_JZ, 0, last_cmp_vreg, target);
			return off + d.len;
		}
		/* 0F 85 rel32: jne */
		if (d.op2 == 0x85) {
			target = off + d.len + d.imm;
			ir_emit_d(IR_JNZ, 0, last_cmp_vreg, target);
			return off + d.len;
		}

		/* 0F 9x ModRM: setcc — emit comparison IR */
		if (d.op2 >= 0x90 && d.op2 <= 0x9F) {
			v = new_vreg_d();
			if (cmp_v1 >= 0 && cmp_v2 >= 0) {
				cc_kind = IR_EQ;
				if (d.op2 == 0x94) cc_kind = IR_EQ;
				if (d.op2 == 0x95) cc_kind = IR_NE;
				if (d.op2 == 0x9C) cc_kind = IR_LTS;
				if (d.op2 == 0x92) cc_kind = IR_LTU;
				if (d.op2 == 0x9F) cc_kind = IR_GTS;
				if (d.op2 == 0x97) cc_kind = IR_GTU;
				if (d.op2 == 0x9E) cc_kind = IR_LES;
				if (d.op2 == 0x96) cc_kind = IR_LEU;
				if (d.op2 == 0x9D) cc_kind = IR_GES;
				if (d.op2 == 0x93) cc_kind = IR_GEU;
				ir_emit_d(cc_kind, v, cmp_v1, cmp_v2);
				cmp_v1 = -1; cmp_v2 = -1;
			} else if (d.op2 == 0x94) {
				/* sete after test = LOG_NOT */
				ir_emit_d(IR_LNOT, v, last_cmp_vreg, 0);
			} else if (d.op2 == 0x95) {
				/* setne after test = TEST_SETNE */
				ir_emit_d(IR_SETNE, v, last_cmp_vreg, 0);
			}
			setcc_old_vreg = reg_vreg[d.rm];
			setcc_reg = d.rm;
			reg_vreg[d.rm] = v;
			mark_reg_set(d.rm);
			last_cmp_vreg = v;
			return off + d.len;
		}

		/* 0F B6: movzx byte — load width 1 */
		if (d.op2 == 0xB6) {
			if (d.mod == 3)
				return off + d.len; /* movzx Rd,Rd_8bit: setcc tail, skip */
			return decode_load_w(off, &d, 1);
		}

		/* 0F B7: movzx word — load width 2 */
		if (d.op2 == 0xB7)
			return decode_load_w(off, &d, 2);

		/* 0F AF ModRM: imul Rd, Rs */
		if (d.op2 == 0xAF) {
			v = new_vreg_d();
			ir_emit_d(IR_MUL, v, reg_vreg[d.reg], reg_vreg[d.rm]);
			reg_vreg[d.reg] = v;
			mark_reg_set(d.reg);
			return off + d.len;
		}

#ifdef DEBUG
		trap(0x0407, d.op2); /* unknown 0F xx opcode */
#endif
		return off + d.len;
	}

	/* 81/83: ALU r/m, imm32/imm8 */
	if (opc == 0x81 || opc == 0x83) {
		/* /5 on rsp = sub rsp,imm (prologue), skip */
		if (d.reg == 5 && d.rm == 4)
			return off + d.len;
		/* /7 = cmp reg, imm */
		if (d.reg == 7) {
			cmp_v1 = reg_vreg[d.rm];
			cmp_v2 = -1;
			/* emit LOAD_IMM for the immediate, then record cmp operands */
			v = new_vreg_d();
			ir_emit_d_imm(IR_LOAD_IMM, v, 0, d.imm);
			cmp_v2 = v;
			last_cmp_vreg = cmp_v1;
			return off + d.len;
		}
		/* /0=add /1=or /4=and /5=sub /6=xor */
		v1 = reg_vreg[d.rm];
		v2 = new_vreg_d();
		ir_emit_d_imm(IR_LOAD_IMM, v2, 0, d.imm);
		v = new_vreg_d();
		if (d.reg == 0) ir_emit_d(IR_ADD, v, v1, v2);
		else if (d.reg == 1) ir_emit_d(IR_OR, v, v1, v2);
		else if (d.reg == 4) ir_emit_d(IR_AND, v, v1, v2);
		else if (d.reg == 5) ir_emit_d(IR_SUB, v, v1, v2);
		else ir_emit_d(IR_XOR, v, v1, v2);
		reg_vreg[d.rm] = v;
		mark_reg_set(d.rm);
		return off + d.len;
	}

	/* 89 ModRM: mov r/m, reg (store direction) */
	if (opc == 0x89) {
		if (d.mod == 3) {
			/* mov Rd, Rs — register move */
			if (d.rm == setcc_reg)
				setcc_reg = -1;
			reg_vreg[d.rm] = reg_vreg[d.reg];
			mark_reg_set(d.rm);
			if (d.rm == 0)
				rax_explicit_ret = 1;
			return off + d.len;
		}
		/* local store: needs setcc cleanup after IR emit */
		if ((d.modrm & 7) == 5 && d.mod != 0) {
			store_w = mov_width(&d);
			target = ir_emit_d(IR_STORE_LOC, 0, d.disp, reg_vreg[d.reg]);
			ir[target].imm = store_w;
			if (d.reg == setcc_reg) {
				reg_vreg[setcc_reg] = setcc_old_vreg;
				setcc_reg = -1;
			}
			return off + d.len;
		}
		/* global store and deref store — common helper */
		return decode_store_w(off, &d, mov_width(&d), d.reg);
	}

	/* 8B ModRM: mov reg, r/m (load direction) */
	if (opc == 0x8B) {
		if (d.mod == 3) {
			reg_vreg[d.reg] = reg_vreg[d.rm];
			mark_reg_set(d.reg);
			return off + d.len;
		}
		return decode_load_w(off, &d, mov_width(&d));
	}

	/* 8D ModRM: lea reg, r/m */
	if (opc == 0x8D) {
		if (d.mod == 0 && (d.modrm & 7) == 5) {
			/* lea Rd, [rip+d32] — check if target is a function or a global.
			 * IR_ADDR_FUNC: lea of a known function address (e.g. passing
			 * a function pointer).  Resolved to fid by normalize_targets. */
			target = off + d.len + d.disp;
			v = new_vreg_d();
			if (off_to_fid(target) > 0 || target == funcs[0].off) {
				ir_emit_d(IR_ADDR_FUNC, v, target, 0);
			} else {
				ir_emit_d(IR_ADDR_GLOB, v, target, 0);
			}
			reg_vreg[d.reg] = v;
			mark_reg_set(d.reg);
			return off + d.len;
		}
		if ((d.modrm & 7) == 5) {
			/* lea Rd, [rbp+disp] — addr local */
			v = new_vreg_d();
			ir_emit_d(IR_ADDR_LOC, v, d.disp, 0);
			reg_vreg[d.reg] = v;
			mark_reg_set(d.reg);
			return off + d.len;
		}
		return off + d.len;
	}

	/* B8+r: mov reg, imm32 (or movabs with REX.W) */
	if (opc >= 0xB8 && opc <= 0xBF) {
		r_dst = opc - 0xB8;
		if (d.rex & 1) r_dst |= 8;
		v = new_vreg_d();
		mark_reg_set(r_dst);
		ir_emit_d_imm(IR_LOAD_IMM, v, 0, d.imm);
		reg_vreg[r_dst] = v;
		return off + d.len;
	}

	/* C7 /0: mov r/m64, imm32 (sign-extended) */
	if (opc == 0xC7) {
		v = new_vreg_d();
		ir_emit_d_imm(IR_LOAD_IMM, v, 0, d.imm);
		if (d.mod == 3) {
			reg_vreg[d.rm] = v;
			mark_reg_set(d.rm);
		}
		return off + d.len;
	}

	/* 31 ModRM: xor — check for xor Rd,Rd = zero */
	if (opc == 0x31) {
		if (d.mod == 3 && d.reg == d.rm) {
			v = new_vreg_d();
			ir_emit_d(IR_CLR, v, reg_vreg[d.reg], 0); /* s1=prior vreg for roundtrip */
			reg_vreg[d.reg] = v;
			mark_reg_set(d.reg);
			return off + d.len;
		}
		if (d.mod == 3) {
			v = new_vreg_d();
			ir_emit_d(IR_XOR, v, reg_vreg[d.rm], reg_vreg[d.reg]);
			reg_vreg[d.rm] = v;
			mark_reg_set(d.rm);
			return off + d.len;
		}
		return off + d.len;
	}

	/* ALU reg,reg: 01=add 29=sub 21=and 09=or 39=cmp 85=test */
	if (opc == 0x01 || opc == 0x29 || opc == 0x21 ||
	    opc == 0x09 || opc == 0x39 || opc == 0x85) {
		if (opc == 0x39) {
			/* cmp Rs, Rd — save operands for next setcc */
			cmp_v1 = reg_vreg[d.rm];
			cmp_v2 = reg_vreg[d.reg];
			return off + d.len;
		}
		if (opc == 0x85) {
			/* test Rd, Rd — save operand for jz/jnz */
			last_cmp_vreg = reg_vreg[d.rm];
			cmp_v1 = -1; cmp_v2 = -1; /* not a cmp */
			/* if test is consuming a setcc result in this register,
			 * restore the old vreg that lived here before setcc */
			if (d.rm == setcc_reg) {
				reg_vreg[setcc_reg] = setcc_old_vreg;
				setcc_reg = -1;
			}
			return off + d.len;
		}
		v = new_vreg_d();
		v1 = reg_vreg[d.rm];
		v2 = reg_vreg[d.reg];
		if (opc == 0x01) ir_emit_d(IR_ADD, v, v1, v2);
		else if (opc == 0x29) ir_emit_d(IR_SUB, v, v1, v2);
		else if (opc == 0x21) ir_emit_d(IR_AND, v, v1, v2);
		else ir_emit_d(IR_OR, v, v1, v2);
		reg_vreg[d.rm] = v;
		mark_reg_set(d.rm);
		return off + d.len;
	}

	/* D3 ModRM: shift by cl */
	if (opc == 0xD3) {
		v = new_vreg_d();
		v1 = reg_vreg[d.rm];
		v2 = reg_vreg[1]; /* rcx */
		if (d.reg == 4) ir_emit_d(IR_SHL, v, v1, v2);
		else if (d.reg == 7) ir_emit_d(IR_SHRS, v, v1, v2);
		else ir_emit_d(IR_SHRU, v, v1, v2);
		reg_vreg[d.rm] = v;
		mark_reg_set(d.rm);
		return off + d.len;
	}

	/* F7 ModRM: neg/not/idiv/div */
	if (opc == 0xF7) {
		if (d.reg == 3) { /* neg */
			v = new_vreg_d();
			ir_emit_d(IR_NEG, v, reg_vreg[d.rm], 0);
			reg_vreg[d.rm] = v;
			mark_reg_set(d.rm);
		} else if (d.reg == 2) { /* not */
			v = new_vreg_d();
			ir_emit_d(IR_NOT, v, reg_vreg[d.rm], 0);
			reg_vreg[d.rm] = v;
			mark_reg_set(d.rm);
		} else if (d.reg == 7) { /* idiv: rax=quotient, rdx=remainder */
			v1 = reg_vreg[0]; /* save dividend before overwrite */
			v = new_vreg_d();
			ir_emit_d(IR_DIVS, v, v1, reg_vreg[d.rm]);
			reg_vreg[0] = v;
			mark_reg_set(0);
			v = new_vreg_d();
			ir_emit_d(IR_MODS, v, v1, reg_vreg[d.rm]);
			reg_vreg[2] = v; /* rdx = remainder */
			mark_reg_set(2);
		} else if (d.reg == 6) { /* div: rax=quotient, rdx=remainder */
			v1 = reg_vreg[0]; /* save dividend before overwrite */
			v = new_vreg_d();
			ir_emit_d(IR_DIVU, v, v1, reg_vreg[d.rm]);
			reg_vreg[0] = v;
			mark_reg_set(0);
			v = new_vreg_d();
			ir_emit_d(IR_MODU, v, v1, reg_vreg[d.rm]);
			reg_vreg[2] = v; /* rdx = remainder */
			mark_reg_set(2);
		}
		return off + d.len;
	}

	/* 99: cqo — sign-extend rax to rdx:rax, skip (part of div sequence) */
	if (opc == 0x99) return off + d.len;

	/* 88 ModRM: mov byte [rm], reg — byte store */
	if (opc == 0x88)
		return decode_store_w(off, &d, 1, d.reg);

	/* 63 ModRM: movsxd — sign-extending 4-byte load */
	if (opc == 0x63)
		return decode_load_w(off, &d, 4);

#ifdef DEBUG
	trap(0x0408, opc); /* unknown opcode in decode_insn */
#endif
	return off + d.len;
}

/* linear decode: walk instructions, track register→vreg mappings, emit IR */
void decode_function(long fn_idx)
{
	struct dec pd;
	long off, fn_start, fn_end;
	long save_off;
	long ti, r;
	long prev_nir, last_op;

	fn_start = funcs[fn_idx].off;
	fn_end = funcs[fn_idx].end;
	funcs[fn_idx].npar = 0;

	nir = 0;
	next_vreg = 1;
	reset_regs();

	reg_vreg[7] = new_vreg_d();
	reg_vreg[6] = new_vreg_d();
	reg_vreg[2] = new_vreg_d();
	reg_vreg[1] = new_vreg_d();
	reg_vreg[8] = new_vreg_d();
	reg_vreg[9] = new_vreg_d();

	off = fn_start + 4;
	if (funcs[fn_idx].frame > 0) {
		if (rb(off) == 0x48 && rb(off+1) == 0x81)
			off += 7;
		else if (rb(off) == 0x48 && rb(off+1) == 0x83)
			off += 4;
	}

	reset_locals();
	save_off = off;
	while (save_off < fn_end) {
		x86_decode(save_off, &pd);
		/* param save: REX.W mov [rbp+disp], param_reg */
		if (pd.opc != 0x89 || !(pd.rex & 0x08)) break;
		if (pd.mod == 3 || pd.rm != 5) break;
		if (pd.reg != 7 && pd.reg != 6 && pd.reg != 2 &&
		    pd.reg != 1 && pd.reg != 8 && pd.reg != 9)
			break;
		disp_to_vid(pd.disp);
		funcs[fn_idx].npar++;
		save_off += pd.len;
	}

	off = save_off;
	reg_vreg[7] = -1; reg_vreg[6] = -1; reg_vreg[2] = -1;
	reg_vreg[1] = -1; reg_vreg[8] = -1; reg_vreg[9] = -1;

	while (off < fn_end) {
		prev_nir = nir;
		off = decode_insn(off, fn_start, fn_end);
		if (nir > prev_nir) {
			last_op = ir[nir - 1].op;
			if (last_op == IR_JMP) {
				/* after unconditional JMP, all register mappings are stale (no merge) */
				for (r = 0; r < 16; r++)
					reg_vreg[r] = -1;
				save_sp = 0;
				setcc_reg = -1;
			}
		}
	}

	/* clamp stale vreg refs (e.g. -2 from clobbered regs) to -1
	 * so the raise pass sees a clean "undefined" sentinel */
	for (ti = 0; ti < nir; ti++) {
		if (ir[ti].s1 <= -2 && s1_is_vreg(ti))
			ir[ti].s1 = -1;
		if (ir[ti].s2 <= -2 && s2_is_vreg(ti))
			ir[ti].s2 = -1;
	}
	normalize_targets();
#ifdef MEASURE
	{
		extern long wm_nir_decode, wm_next_vreg_dec, wm_save_sp;
		if (nir > wm_nir_decode) wm_nir_decode = nir;
		if (next_vreg > wm_next_vreg_dec) wm_next_vreg_dec = next_vreg;
	}
#endif
}
