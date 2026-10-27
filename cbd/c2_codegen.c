/*
 * c2_codegen.c — register allocator and IR→BIN code emitter
 *
 * Included by c2_compile.c. Uses globals from c2_compile.c:
 *   ir[], nir, next_vreg — IR instructions and vreg counter
 *   label_ir[], nlabels — label→IR index mapping
 *   out, pos, eb/ed/eq — output buffer and byte emitters
 *   emit_* — x86 encoding helpers
 *   gvars[], lvars[], frame_size — variable offset tables
 *   code_base, bss_base — memory layout constants
 */

/* register allocator */

#define N_PHYS 8 /* allocatable registers (rax excluded — scratch/return only) */

/* rng_state, rng() defined in cc_lib.c */

struct ra_vreg {
	int reg;    /* phys reg, or -1 = spilled, -2 = unassigned */
	int spill;  /* spill offset from rbp (negative), 0 = not spilled */
	int start;  /* first def (IR index) */
	int end;    /* last use (IR index) */
};
struct ra_vreg ra[MAX_VREG];

struct active_slot { int vreg; int end; };
struct active_slot active[N_PHYS]; /* phys slot → vreg + last use */
long spill_off;          /* next spill offset (grows negative) */

/* phys reg numbers in allocation order (least constrained first).
 * rax (0) excluded — reserved as scratch for spills and return value. */
int reg_pool[8];

void init_regalloc(void)
{
	long i, nv;
#ifdef MUT_REGALLOC
	long j, tmp;
#endif
	reg_pool[0] = 10; /* r10 */
	reg_pool[1] = 11; /* r11 */
	reg_pool[2] = 6;  /* rsi */
	reg_pool[3] = 7;  /* rdi */
	reg_pool[4] = 8;  /* r8 */
	reg_pool[5] = 9;  /* r9 */
	reg_pool[6] = 2;  /* rdx — div constraint */
	reg_pool[7] = 1;  /* rcx — shift constraint */
#ifdef MUT_REGALLOC
	/* Fisher-Yates shuffle on reg_pool[0..5] (r10,r11,rsi,rdi,r8,r9).
	 * reg_pool[6..7] (rdx,rcx) stay last — hardware constraints.
	 * 6! = 720 permutations per function. */
	if (rng_state) {
		for (i = 5; i > 0; i--) {
			j = (rng() & 0x7fffffff) % (i + 1);
			tmp = reg_pool[i];
			reg_pool[i] = reg_pool[j];
			reg_pool[j] = tmp;
		}
	}
#endif
	nv = next_vreg;
#ifdef DEBUG
	if (nv >= MAX_VREG) trap(0x030E, nv);
#endif
	for (i = 0; i < nv; i++) {
		ra[i].reg = -2;  /* unassigned */
		ra[i].spill = 0;
		ra[i].start = -1;
		ra[i].end = -1;
	}
	for (i = 0; i < N_PHYS; i++) {
		active[i].vreg = 0;
		active[i].end = -1;
	}
	spill_off = 0;
}

void mark_def(long v, long i)
{
	if (v <= 0) return;
#ifdef DEBUG
	if (v >= MAX_VREG) trap(0x030E, v);
#endif
	if (ra[v].start < 0) ra[v].start = i;
	if (i > ra[v].end) ra[v].end = i;
}

void mark_use(long v, long i)
{
	if (v <= 0) return;
#ifdef DEBUG
	if (v >= MAX_VREG) trap(0x030E, v);
#endif
	if (i > ra[v].end) ra[v].end = i;
}

/* liveness classification tables: populated once by init_liveness_tables().
 * ir_has_def[op]:  instruction defines dst vreg
 * ir_use_s1[op]:   instruction uses s1 as vreg
 * ir_use_s2[op]:   instruction uses s2 as vreg (only when imm==0 for binops)
 * All indexed by IR opcode (max ~50). */
long ir_has_def[64];
long ir_use_s1[64];
long ir_use_s2[64];       /* s2 is vreg only when imm==0 (binops/cmp) */
long ir_use_s2_always[64]; /* s2 is always vreg (stores, deref_st) */
void init_liveness_tables(void)
{
	long i;
	for (i = 0; i < 64; i++) {
		ir_has_def[i] = 0;
		ir_use_s1[i] = 0;
		ir_use_s2[i] = 0;
		ir_use_s2_always[i] = 0;
	}
	/* definitions (dst is a vreg) */
	ir_has_def[IR_LOAD_IMM] = 1;
	ir_has_def[IR_LOAD_LOC] = 1;
	ir_has_def[IR_LOAD_GLOB] = 1;
	ir_has_def[IR_ADDR_LOC] = 1;
	ir_has_def[IR_ADDR_GLOB] = 1;
	ir_has_def[IR_DEREF_LD] = 1;
	ir_has_def[IR_NEG] = 1;
	ir_has_def[IR_NOT] = 1;
	ir_has_def[IR_LNOT] = 1;
	ir_has_def[IR_SETNE] = 1;
	ir_has_def[IR_CLR] = 1;
	ir_has_def[IR_CALL] = 1;
	ir_has_def[IR_CALL_IND] = 1;
	ir_has_def[IR_ADDR_FUNC] = 1;
	for (i = IR_ADD; i <= IR_XOR; i++) ir_has_def[i] = 1;
	for (i = IR_EQ; i <= IR_GEU; i++) ir_has_def[i] = 1;
	for (i = IR_DIVS; i <= IR_MODU; i++) ir_has_def[i] = 1;

	/* s1 uses */
	ir_use_s1[IR_DEREF_LD] = 1;
	ir_use_s1[IR_DEREF_ST] = 1;
	ir_use_s1[IR_NEG] = 1;
	ir_use_s1[IR_NOT] = 1;
	ir_use_s1[IR_LNOT] = 1;
	ir_use_s1[IR_SETNE] = 1;
	ir_use_s1[IR_JZ] = 1;
	ir_use_s1[IR_JNZ] = 1;
	ir_use_s1[IR_RET] = 1;
	ir_use_s1[IR_ARG] = 1;
	ir_use_s1[IR_CALL_IND] = 1;
	for (i = IR_ADD; i <= IR_XOR; i++) ir_use_s1[i] = 1;
	for (i = IR_EQ; i <= IR_GEU; i++) ir_use_s1[i] = 1;
	for (i = IR_DIVS; i <= IR_MODU; i++) ir_use_s1[i] = 1;

	/* s2 uses: conditional (imm==0 only, for binops/cmp/div) */
	for (i = IR_ADD; i <= IR_XOR; i++) ir_use_s2[i] = 1;
	for (i = IR_EQ; i <= IR_GEU; i++) ir_use_s2[i] = 1;
	for (i = IR_DIVS; i <= IR_MODU; i++) ir_use_s2[i] = 1;

	/* s2 uses: unconditional (stores, deref) */
	ir_use_s2_always[IR_DEREF_ST] = 1;
	ir_use_s2_always[IR_STORE_LOC] = 1;
	ir_use_s2_always[IR_STORE_GLOB] = 1;

}

/* compute live ranges: ra[v].start = first def, ra[v].end = last use */
void compute_liveness(void)
{
	long i, op;
	for (i = 0; i < nir; i++) {
		op = ir[i].op;
		if (op <= 0 || op >= 64)
			op = 0;
		if (ir_has_def[op])
			mark_def(ir[i].dst, i);
		if (ir_use_s1[op])
			mark_use(ir[i].s1, i);
		if (ir_use_s2_always[op])
			mark_use(ir[i].s2, i);
		else if (ir_use_s2[op] && ir[i].imm == 0)
			mark_use(ir[i].s2, i);
	}
}

long alloc_reg(long v, long at)
{
	long j, best, best_end;
#ifdef DEBUG
	if (v <= 0 || v >= MAX_VREG)
		trap(0x030B, v);
#endif

	/* try free register */
	for (j = 0; j < N_PHYS; j++) {
		if (active[j].vreg == 0 || active[j].end < at) {
			active[j].vreg = v;
			active[j].end = ra[v].end;
			ra[v].reg = reg_pool[j];
			return reg_pool[j];
		}
	}

	/* evict vreg with farthest end */
	best = 0;
	best_end = active[0].end;
	for (j = 1; j < N_PHYS; j++) {
		if (active[j].end > best_end) {
			best = j;
			best_end = active[j].end;
		}
	}

	/* no free register: spill ourselves (never evict — eviction
	 * requires emitting a store which we can't do at alloc time) */
	spill_off -= 8;
	ra[v].spill = spill_off;
	ra[v].reg = -1;
	return -1;
}

/* linear scan: allocate physical registers in IR order */
void run_regalloc(void)
{
	long i, dst, hint_s1, j, hr, got_hint;
	i = 0;
	while (i < nir) {
		dst = ir[i].dst;
		/* dst is a vreg for most ops, but for IR_DEREF_ST it's the
		 * width, for IR_FUNC it's fid — skip non-vreg values */
		if (dst > 0 && dst < MAX_VREG && ra[dst].start == i
		    && ra[dst].reg == -2) {
/* [HINT_REUSE] s1 register hint in regalloc.
 * When dst is defined and s1 dies at the same instruction, reuses
 * s1's physical register for dst, avoiding a mov in codegen.
 * Disable: #if 0 always calls alloc_reg(). */
#if 1
			got_hint = 0;
			/* binop: reuse s1's register if s1 dies here */
			hint_s1 = ir[i].s1;
			if (hint_s1 > 0 && hint_s1 < MAX_VREG &&
			    ra[hint_s1].end <= i &&
			    ra[hint_s1].reg >= 0) {
				hr = ra[hint_s1].reg;
				for (j = 0; j < N_PHYS; j++) {
					if (reg_pool[j] == hr &&
					    active[j].vreg == hint_s1) {
						active[j].vreg = dst;
						active[j].end = ra[dst].end;
						ra[dst].reg = hr;
						got_hint = 1;
						break;
					}
				}
			}
			if (!got_hint)
				alloc_reg(dst, i);
#else
			alloc_reg(dst, i);
#endif
		}
		i++;
	}
}

/* return physical register assigned to vreg, or -1 if spilled */
long get_reg(long v)
{
#ifdef DEBUG
	if (v <= 0 || v >= MAX_VREG)
		trap(0x0306, v);
#endif
	return ra[v].reg;
}

long spill_disp(long v)
{
#ifdef DEBUG
	if (v <= 0 || v >= MAX_VREG)
		trap(0x0307, v);
#endif
	return ra[v].spill - frame_size;
}

/* load spilled vreg into tmp register */
long ensure_reg(long v, long tmp)
{
	long r;
	r = get_reg(v);
	if (r >= 0) return r;
	emit_load_rbp(tmp, spill_disp(v));
	return tmp;
}

/* store result to spill slot if vreg is spilled */
void maybe_spill(long v, long r)
{
	if (v <= 0) return;
#ifdef DEBUG
	if (v >= MAX_VREG)
		trap(0x0309, v);
#endif
	if (ra[v].reg < 0)
		emit_store_rbp(r, spill_disp(v));
}

/* pick a tmp register avoiding one conflict */
long pick_tmp(long avoid)
{
	if (avoid != 0) return 0; /* rax */
	return 1; /* rcx */
}

/* pick a tmp register avoiding two conflicts */
long pick_tmp2(long avoid1, long avoid2)
{
	if (avoid1 != 0 && avoid2 != 0) return 0; /* rax */
	if (avoid1 != 1 && avoid2 != 1) return 1; /* rcx */
	return 2; /* rdx — last resort */
}

/* ALU rd, imm: uses imm8 (83) when -128..127, else imm32 (81).
 * ext = ModRM reg field: add=0, or=1, and=4, sub=5, xor=6, cmp=7 */
void emit_alu_imm(long rd, long ext, long imm)
{
	long digit;
/* [O3] INC/DEC peephole: ADD/SUB rd, ±1 → INC/DEC rd (3 vs 4 bytes).
 * Disable: #if 0 skips the peephole. */
#if 1
	if ((imm == 1 || imm == -1) && (ext == 0 || ext == 5)) {
		/* digit 0=INC, 1=DEC:
		 * add+1 → INC(0), sub+1 → DEC(1), add-1 → DEC(1), sub-1 → INC(0) */
		digit = 0;
		if ((imm == 1 && ext == 5) || (imm == -1 && ext == 0))
			digit = 1;
		emit_rex(1, 0, rd);
		eb(0xff);
		emit_modrm_rr(digit, rd & 7);
		return;
	}
#endif
	if (imm >= -128 && imm <= 127) {
		emit_rex(1, 0, rd);
		eb(0x83);
		emit_modrm_rr(ext, rd & 7);
		eb(imm & 0xff);
	} else {
		emit_rex(1, 0, rd);
		eb(0x81);
		emit_modrm_rr(ext, rd & 7);
		ed(imm);
	}
}

/* code emit */

struct fixup { int off; int fid; };
struct fixup fixes[MAX_FIXUPS];
long nfix;
int func_off[MAX_FUNC];
int ir_code_off[MAX_IR]; /* IR index → code offset */
struct jmp_fixup { int pos; int label; };
struct jmp_fixup jmp_fixes[MAX_JMP_FIX];
long njmp_fix;
long total_frame;   /* frame_size + spill slots */

/* Sys V ABI parameter registers: rdi, rsi, rdx, rcx, r8, r9 */
/* SysV ABI parameter registers: rdi, rsi, rdx, rcx, r8, r9 */
long param_regs[6];

void init_param_regs(void)
{
	param_regs[0] = 7; param_regs[1] = 6; param_regs[2] = 2;
	param_regs[3] = 1; param_regs[4] = 8; param_regs[5] = 9;
}

long param_reg(long i)
{
#ifdef DEBUG
	if (i < 0 || i > 5) trap(0x030C, i);
#endif
	return param_regs[i];
}

/* call helpers: expire dead vregs, save/restore live registers around calls */
void call_expire_save(long i, long dst)
{
	long sj, sr;
	for (sj = 0; sj < N_PHYS; sj++) {
		if (active[sj].vreg > 0 && ra[active[sj].vreg].end <= i)
			active[sj].vreg = 0;
	}
	for (sj = 0; sj < N_PHYS; sj++) {
		sr = reg_pool[sj];
		if (active[sj].vreg > 0 && active[sj].vreg != dst) {
			if (sr >= 8) { eb(0x41); eb(0x50 + (sr & 7)); }
			else eb(0x50 + sr);
		}
	}
}

void call_setup_args(long i, long nargs)
{
	long sj, sr, r1;
	if (nargs == 1) {
		sr = i - 1;
#ifdef DEBUG
		if (sr < 0 || ir[sr].op != IR_ARG)
			trap(0x0312, sr);
#endif
		r1 = ensure_reg(ir[sr].s1, 0);
		emit_mov_rr(param_reg(0), r1);
	} else if (nargs > 1) {
		for (sj = nargs - 1; sj >= 0; sj--) {
			sr = i - nargs + sj;
#ifdef DEBUG
			if (sr < 0 || sr >= nir)
				trap(0x0311, sr);
			if (ir[sr].op != IR_ARG)
				trap(0x0312, ir[sr].op);
#endif
			r1 = ensure_reg(ir[sr].s1, 0);
			if (r1 >= 8) { eb(0x41); eb(0x50 + (r1 & 7)); }
			else eb(0x50 + r1);
		}
		for (sj = 0; sj < nargs; sj++) {
			sr = param_reg(sj);
			eb(0x58);
			emit_mov_rr(sr, 0);
		}
	}
}

void call_restore_capture(long dst)
{
	long sj, sr, rd;
	for (sj = N_PHYS - 1; sj >= 0; sj--) {
		sr = reg_pool[sj];
		if (active[sj].vreg > 0 && active[sj].vreg != dst) {
			if (sr >= 8) { eb(0x41); eb(0x58 + (sr & 7)); }
			else eb(0x58 + sr);
		}
	}
	rd = get_reg(dst);
	if (rd >= 0 && rd != 0)
		emit_mov_rr(rd, 0);
	maybe_spill(dst, 0);
}

long setcc_for(long kind)
{
	if (kind == N_EQ)   return 0x94;
	if (kind == N_NE)   return 0x95;
	if (kind == N_LT_S) return 0x9c;
	if (kind == N_LT_U) return 0x92;
	if (kind == N_GT_S) return 0x9f;
	if (kind == N_GT_U) return 0x97;
	if (kind == N_LE_S) return 0x9e;
	if (kind == N_LE_U) return 0x96;
	if (kind == N_GE_S) return 0x9d;
	if (kind == N_GE_U) return 0x93;
#ifdef DEBUG
	trap(0x0101, kind);
#endif
	return 0x94;
}

/* compute RIP-relative displacement: bss_addr - (rip_at_end_of_insn) */
long rip_disp_raw(long gid, long ilen)
{
#ifdef DEBUG
	if (gid - GLOBAL_ID_BASE < 0 || gid - GLOBAL_ID_BASE >= MAX_GLOBALS)
		trap(0x0310, gid);
#endif
	return bss_base + gvars[gid - GLOBAL_ID_BASE].off - (code_base + pos + ilen);
}

/* rip_disp for qword (REX.W + opcode + modrm + d32 = 7 bytes) */
long rip_disp(long gid)
{
	return rip_disp_raw(gid, 7);
}

/* width-aware rip displacement for loads (MOVZX byte/word = 7-8, MOV dword/qword = 6-7) */
long rip_disp_w(long gid, long w, long reg)
{
	long ilen;
	if (w == 1 || w == 2) {
		ilen = 7;
		if (reg >= 8) ilen = 8;
	} else if (w == 4) {
		ilen = 6;
		if (reg >= 8) ilen = 7;
	} else {
		ilen = 7;
	}
	return rip_disp_raw(gid, ilen);
}

/* width-aware rip displacement for stores */
long rip_disp_store_w(long gid, long w, long reg)
{
	long ilen;
	if (w == 1) {
		ilen = 6;
		if (reg >= 4) ilen = 7;
	} else if (w == 2) {
		ilen = 7;
		if (reg >= 8) ilen = 8;
	} else if (w == 4) {
		ilen = 6;
		if (reg >= 8) ilen = 7;
	} else {
		ilen = 7;
	}
	return rip_disp_raw(gid, ilen);
}

void add_jmp_fix(long target_label)
{
#ifdef DEBUG
	if (njmp_fix >= MAX_JMP_FIX) trap(0x0303, njmp_fix);
	if (target_label < 0 || target_label >= MAX_LABELS) trap(0x0318, target_label);
#endif
	jmp_fixes[njmp_fix].pos = pos;
	jmp_fixes[njmp_fix].label = target_label;
	njmp_fix++;
}

/* emit x86-64 machine code for the IR array.
 * Dispatch order: LOAD_IMM, LOAD_LOC, STORE_LOC, ADDR_LOC,
 * LOAD_GLOB, STORE_GLOB, ADDR_GLOB, DEREF_LD, DEREF_ST,
 * ADD..XOR (non-div/shift/mul), MUL, SHL/SHR, DIV/MOD,
 * NEG, NOT, LNOT, SETNE, EQ..GEU, CLR,
 * JZ, JNZ, JMP, ARG, CALL, CALL_IND, RET, RET_VOID,
 * FUNC, PARAM, ADDR_FUNC. */
void emit_ir(void)
{
	long i, op, dst, s1, s2, w;
	long rd, r1, r2, cc, alu_ext, tmp_r;

	for (i = 0; i < nir; i++) {
#ifdef DEBUG
		if (i >= MAX_IR) trap(0x0317, i);
#endif
		ir_code_off[i] = pos;
		op = ir[i].op;
		dst = ir[i].dst;
		s1 = ir[i].s1;
		s2 = ir[i].s2;

		if (op == IR_LOAD_IMM) {
			rd = get_reg(dst);
			if (rd < 0) rd = 0;
			emit_mov_imm(rd, ir[i].imm);
			maybe_spill(dst, rd);
		}
		else if (op == IR_LOAD_LOC) {
#ifdef DEBUG
			if (s1 < 0 || s1 >= MAX_LOCALS) trap(0x030D, s1);
#endif
			rd = get_reg(dst);
			if (rd < 0) rd = 0;
			w = ir[i].imm;
			if (w > 0 && w < 8)
				emit_load_rbp_w(rd, lvars[s1].off, w);
				else
				emit_load_rbp(rd, lvars[s1].off);
			maybe_spill(dst, rd);
		}
		else if (op == IR_STORE_LOC) {
#ifdef DEBUG
			if (s1 < 0 || s1 >= MAX_LOCALS) trap(0x030D, s1);
#endif
			r1 = ensure_reg(s2, 0);
			w = ir[i].imm;
			if (w > 0 && w < 8)
				emit_store_rbp_w(r1, lvars[s1].off, w);
				else
				emit_store_rbp(r1, lvars[s1].off);
		}
		else if (op == IR_ADDR_LOC) {
#ifdef DEBUG
			if (s1 < 0 || s1 >= MAX_LOCALS) trap(0x030D, s1);
#endif
			rd = get_reg(dst);
			if (rd < 0) rd = 0;
			emit_lea_rbp(rd, lvars[s1].off);
			maybe_spill(dst, rd);
		}
		else if (op == IR_LOAD_GLOB) {
			rd = get_reg(dst);
			if (rd < 0) rd = 0;
			w = ir[i].imm;
			if (w > 0 && w < 8) {
				emit_load_rip_w(rd, rip_disp_w(s1, w, rd), w);
			} else {
				emit_load_rip(rd, rip_disp(s1));
			}
			maybe_spill(dst, rd);
		}
		else if (op == IR_STORE_GLOB) {
			r1 = ensure_reg(s2, 0);
			w = ir[i].imm;
			if (w > 0 && w < 8) {
				emit_store_rip_w(r1, rip_disp_store_w(s1, w, r1), w);
			} else {
				emit_store_rip(r1, rip_disp(s1));
			}
		}
		else if (op == IR_ADDR_GLOB) {
			rd = get_reg(dst);
			if (rd < 0) rd = 0;
			emit_lea_rip(rd, rip_disp(s1));
			maybe_spill(dst, rd);
		}
		else if (op == IR_DEREF_LD) {
			r1 = ensure_reg(s1, 0);
			rd = get_reg(dst);
			if (rd < 0) rd = pick_tmp(r1);
				if (s2 == 1) {
				emit_rex(1, rd, r1); eb(0x0f); eb(0xb6);
			} else if (s2 == 2) {
				emit_rex(1, rd, r1); eb(0x0f); eb(0xb7);
			} else if (s2 == 4) {
				emit_rex(1, rd, r1); eb(0x63);
			} else {
				emit_rex(1, rd, r1); eb(0x8b);
			}
			emit_modrm_deref(rd, r1);
			maybe_spill(dst, rd);
		}
		else if (op == IR_DEREF_ST) {
			/* dst=width, s1=addr, s2=val */
			r1 = ensure_reg(s1, 0);
			r2 = ensure_reg(s2, pick_tmp(r1));
			if (dst == 1) {
				/* byte store: REX required when r2 in 4-7 (spl/bpl/sil/dil)
				 * or r1 >= 8. Without REX, regs 4-7 encode ah/ch/dh/bh
				 * instead of spl/bpl/sil/dil — silent corruption on
				 * real x86 (emulator doesn't distinguish). */
				if (r2 >= 4 || r1 >= 8)
					emit_rex_byte(r2, r1);
				eb(0x88);
			} else if (dst == 2) {
				eb(0x66);
				emit_rex(0, r2, r1);
				eb(0x89);
			} else if (dst == 4) {
				emit_rex(0, r2, r1);
				eb(0x89);
			} else {
				emit_rex(1, r2, r1);
				eb(0x89);
			}
			emit_modrm_deref(r2, r1);
		}
		else if (op >= IR_ADD && op <= IR_XOR &&
		         op != IR_DIVS && op != IR_DIVU &&
		         op != IR_MODS && op != IR_MODU &&
		         op != IR_SHL && op != IR_SHRS && op != IR_SHRU &&
		         op != IR_MUL) {
			if (ir[i].imm != 0) {
				/* immediate fold: op rd, imm */
				rd = get_reg(dst); if (rd < 0) rd = 0;
				r1 = ensure_reg(s1, rd);
				emit_mov_rr(rd, r1);
				alu_ext = 0; /* add=0, or=1, and=4, sub=5, xor=6 */
				if (op == IR_OR)  alu_ext = 1;
				if (op == IR_AND) alu_ext = 4;
				if (op == IR_SUB) alu_ext = 5;
				if (op == IR_XOR) alu_ext = 6;
				emit_alu_imm(rd, alu_ext, ir[i].imm);
			} else {
				rd = get_reg(dst); if (rd < 0) rd = 0;
				r1 = ensure_reg(s1, rd);
				tmp_r = pick_tmp2(r1, rd);
				r2 = ensure_reg(s2, tmp_r);
/* [COMMUTE_SWAP] commutative operand swap in ALU binops.
 * If rd == r2 && rd != r1, swaps r1/r2 to avoid an extra mov.
 * Only for commutative ops (not SUB).
 * Disable: #if 0 skips the swap. */
#if 1
				if (rd == r2 && rd != r1 && op != IR_SUB) {
					tmp_r = r1; r1 = r2; r2 = tmp_r;
				}
#endif
				emit_mov_rr(rd, r1);
				if (op == IR_ADD)      emit_alu_rr(0x01, rd, r2);
				else if (op == IR_SUB) emit_alu_rr(0x29, rd, r2);
				else if (op == IR_AND) emit_alu_rr(0x21, rd, r2);
				else if (op == IR_OR)  emit_alu_rr(0x09, rd, r2);
				else                   emit_alu_rr(0x31, rd, r2);
			}
			maybe_spill(dst, rd);
		}
		else if (op == IR_MUL) {
			rd = get_reg(dst); if (rd < 0) rd = 0;
			r1 = ensure_reg(s1, rd);
			r2 = ensure_reg(s2, pick_tmp(r1));
/* [COMMUTE_SWAP] commutative operand swap in MUL.
 * If rd == r2 && rd != r1, swaps r1/r2 to avoid an extra mov.
 * Disable: #if 0 skips the swap. */
#if 1
			/* commutative: swap to avoid mov clobbering r2 when rd==r2 */
			if (rd == r2 && rd != r1) {
				tmp_r = r1; r1 = r2; r2 = tmp_r;
			}
#endif
			emit_mov_rr(rd, r1);
			emit_rex(1, rd, r2); eb(0x0f); eb(0xaf);
			emit_modrm_rr(rd & 7, r2 & 7);
			maybe_spill(dst, rd);
		}
		else if (op == IR_SHL || op == IR_SHRS || op == IR_SHRU) {
			/* shift: result in rd, s1 = value, s2 = shift amount.
			 * x86 constraint: shift amount must be in rcx (reg 1).
			 * Same pattern as div: materialise both, then route
			 * to hw regs.  If rd == rcx, shift in rax to avoid
			 * conflict between operand and amount registers. */
			rd = get_reg(dst); if (rd < 0) rd = 0;
			r1 = get_reg(s1);
			r2 = get_reg(s2);
			/* step 1: materialise, ensuring s1 not in rcx */
			if (r1 == 1) {
				emit_mov_rr(0, 1);     /* s1: rcx → rax */
				r1 = 0;
				r2 = ensure_reg(s2, 1);
			} else {
				r1 = ensure_reg(s1, rd);
				r2 = ensure_reg(s2, 1);
			}
			/* step 2: route to hw registers */
			if (rd == 1) {
				/* rd is rcx — shift in rax, move result back */
				emit_mov_rr(0, r1);
				emit_mov_rr(1, r2);
				emit_rex(1, 0, 0); eb(0xd3);
				if (op == IR_SHL)       emit_modrm_rr(4, 0);
				else if (op == IR_SHRS) emit_modrm_rr(7, 0);
				else                    emit_modrm_rr(5, 0);
				emit_mov_rr(1, 0);     /* rax → rcx(=rd) */
			} else {
				/* normal: no conflict between rd and rcx */
				emit_mov_rr(1, r2);    /* amount → rcx */
				emit_mov_rr(rd, r1);   /* s1 → rd */
				emit_rex(1, 0, rd); eb(0xd3);
				if (op == IR_SHL)       emit_modrm_rr(4, rd & 7);
				else if (op == IR_SHRS) emit_modrm_rr(7, rd & 7);
				else                    emit_modrm_rr(5, rd & 7);
			}
			maybe_spill(dst, rd);
		}
		else if (op == IR_DIVS || op == IR_DIVU ||
		         op == IR_MODS || op == IR_MODU) {
			/* dividend → rax, divisor → rcx (must avoid rax and rdx).
			 * Careful ordering to prevent clobbering:
			 * 1. get s1 into rax first (rax is scratch, not in pool)
			 * 2. get s2 into rcx — if s2 was in rax, it's already
			 *    been moved out by step 1's ensure_reg */
			r1 = get_reg(s1);
			r2 = get_reg(s2);
			/* if s1 in rcx and s2 needs rcx as tmp: save s1 to rax first */
			if (r1 == 1 && r2 < 0) {
				emit_mov_rr(0, 1); /* save s1 from rcx to rax */
				r2 = ensure_reg(s2, 1); /* load s2, may use rcx */
				r1 = 0; /* s1 is now in rax */
			} else {
				r1 = ensure_reg(s1, 0);
				r2 = ensure_reg(s2, 1);
			}
			emit_mov_rr(0, r1); /* dividend → rax */
			/* divisor must not be rax or rdx */
			if (r2 == 0 || r2 == 2) {
				emit_mov_rr(1, r2);
				r2 = 1;
			}
			if (op == IR_DIVS || op == IR_MODS) {
				/* cqo: sign-extend rax into rdx:rax */
				eb(0x48); eb(0x99);
			} else {
				/* xor rdx, rdx: zero-extend for unsigned div */
				emit_rex(1, 2, 2); eb(0x31);
				emit_modrm_rr(2, 2);
			}
			emit_rex(1, 0, r2); eb(0xf7);
			/* idiv (signed) or div (unsigned) */
			if (op == IR_DIVS || op == IR_MODS)
				emit_modrm_rr(7, r2 & 7);
			else
				emit_modrm_rr(6, r2 & 7);
			rd = get_reg(dst); if (rd < 0) rd = 0;
			if (op == IR_MODS || op == IR_MODU)
				emit_mov_rr(rd, 2); /* remainder in rdx */
			else
				emit_mov_rr(rd, 0); /* quotient in rax */
			maybe_spill(dst, rd);
		}
		else if (op == IR_NEG) {
			rd = get_reg(dst); if (rd < 0) rd = 0;
			r1 = ensure_reg(s1, rd);
			emit_mov_rr(rd, r1);
			/* neg rd */
			emit_rex(1, 0, rd); eb(0xf7);
			emit_modrm_rr(3, rd & 7);
			maybe_spill(dst, rd);
		}
		else if (op == IR_NOT) {
			rd = get_reg(dst); if (rd < 0) rd = 0;
			r1 = ensure_reg(s1, rd);
			emit_mov_rr(rd, r1);
			/* not rd */
			emit_rex(1, 0, rd); eb(0xf7);
			emit_modrm_rr(2, rd & 7);
			maybe_spill(dst, rd);
		}
		else if (op == IR_LNOT) {
			r1 = ensure_reg(s1, 0);
			rd = get_reg(dst); if (rd < 0) rd = 0;
			emit_test(r1);
			/* sete: dst = (s1 == 0) */
			emit_setcc(0x94, rd);
			emit_movzx_byte(rd);
			maybe_spill(dst, rd);
		}
		else if (op == IR_SETNE) {
			r1 = ensure_reg(s1, 0);
			rd = get_reg(dst); if (rd < 0) rd = 0;
			emit_test(r1);
			/* setne: dst = (s1 != 0) */
			emit_setcc(0x95, rd);
			emit_movzx_byte(rd);
			maybe_spill(dst, rd);
		}
		else if (op >= IR_EQ && op <= IR_GEU) {
			r1 = ensure_reg(s1, 0);
			rd = get_reg(dst); if (rd < 0) rd = 0;
			if (ir[i].imm != 0)
				emit_alu_imm(r1, 7, ir[i].imm); /* cmp r1, imm */
			else {
				r2 = ensure_reg(s2, pick_tmp(r1));
				emit_alu_rr(0x39, r1, r2); /* cmp r1, r2 */
			}
			cc = setcc_for(N_EQ + (op - IR_EQ));
			emit_setcc(cc, rd);
			emit_movzx_byte(rd);
			maybe_spill(dst, rd);
		}
		else if (op == IR_CLR) {
			rd = get_reg(dst); if (rd < 0) rd = 0;
			emit_rex(1, rd, rd); eb(0x31);
			emit_modrm_rr(rd & 7, rd & 7);
			maybe_spill(dst, rd);
		}
		else if (op == IR_JZ) {
			r1 = ensure_reg(s1, 0);
			emit_test(r1);
			eb(0x0f); eb(0x84);
			add_jmp_fix(s2);
			ed(0);
		}
		else if (op == IR_JNZ) {
			r1 = ensure_reg(s1, 0);
			emit_test(r1);
			eb(0x0f); eb(0x85);
			add_jmp_fix(s2);
			ed(0);
		}
		else if (op == IR_JMP) {
/* [JMP_ELIM] fallthrough JMP elimination.
 * If JMP target is the very next IR instruction, skip emitting
 * the JMP — execution falls through naturally.
 * Saves 5 bytes per eliminated jump.
 * Disable: #if 0 always emits E9 rel32. */
#if 1
			/* skip JMP if target is next IR insn (fallthrough) */
			if (label_ir[s1] != i + 1) {
				eb(0xe9);
				add_jmp_fix(s1);
				ed(0);
			}
#else
			eb(0xe9);
			add_jmp_fix(s1);
			ed(0);
#endif
		}
		else if (op == IR_ARG) {
			/* record arg vreg — actual move emitted at IR_CALL */
		}
		else if (op == IR_CALL) {
#ifdef DEBUG
			if (nfix >= MAX_FIXUPS)
				trap(0x0104, nfix);
#endif
			call_expire_save(i, dst);
			call_setup_args(i, s2);
			eb(0xe8);
			fixes[nfix].off = pos;
			fixes[nfix].fid = s1;
			nfix++;
			ed(0);
			call_restore_capture(dst);
		}
		else if (op == IR_CALL_IND) {
			/* indirect call: s1=vreg(addr), s2=nargs */
			call_expire_save(i, dst);
			/* save callee into rax, push to protect from arg setup */
			r1 = ensure_reg(s1, 0);
			if (r1 != 0)
				emit_mov_rr(0, r1);
			eb(0x50); /* push rax (callee addr) */
			call_setup_args(i, s2);
			/* pop callee addr back into rax, then call rax */
			eb(0x58);
			eb(0xff); eb(0xd0);
			call_restore_capture(dst);
		}
		else if (op == IR_RET) {
			r1 = ensure_reg(s1, 0);
			emit_mov_rr(0, r1);
			/* leave; ret */
			eb(0xc9); eb(0xc3);
		}
		else if (op == IR_RET_VOID) {
			/* leave; ret */
			eb(0xc9); eb(0xc3);
		}
		else if (op == IR_FUNC) {
#ifdef DEBUG
			if (dst >= MAX_FUNC)
				trap(0x0103, dst);
#endif
			func_off[dst] = pos;
			/* push rbp */
			eb(0x55);
			/* mov rbp, rsp */
			emit_rex(1, 0, 0); eb(0x89);
			emit_modrm_rr(4, 5);
			if (total_frame > 0 && total_frame <= 127) {
				/* sub rsp, imm8 */
				emit_rex(1, 0, 0); eb(0x83);
				emit_modrm_rr(5, 4);
				eb(total_frame & 0xff);
			} else if (total_frame > 0) {
				/* sub rsp, imm32 */
				emit_rex(1, 0, 0); eb(0x81);
				emit_modrm_rr(5, 4);
				ed(total_frame);
			}

		}
		else if (op == IR_PARAM) {
#ifdef DEBUG
			if (dst < 0 || dst >= MAX_LOCALS) trap(0x030F, dst);
#endif
			emit_store_rbp(param_reg(s2), lvars[dst].off);
		}
		else if (op == IR_ADDR_FUNC) {
			/* IR_ADDR_FUNC: address-of-function (function pointer).
			 * Emits lea rd, [rip+disp32]; the rel32 is patched
			 * at link time via the fixup table, same as CALL. */
			rd = get_reg(dst);
			if (rd < 0) rd = 0;
			emit_rex(1, rd, 0);
			eb(0x8d); /* lea */
			eb(0x05 | ((rd & 7) << 3)); /* modrm: rip-relative */
#ifdef DEBUG
			if (nfix >= MAX_FIXUPS) trap(0x0104, nfix);
#endif
			fixes[nfix].off = pos;
			fixes[nfix].fid = s1;
			nfix++;
			ed(0); /* placeholder rel32 */
			maybe_spill(dst, rd);
		}
#ifdef DEBUG
		else if (op != IR_NOP && op != IR_ARG)
			trap(0x030A, op);
#endif
	} /* end for i */
	ir_code_off[nir] = pos;
}

/* patch JMP/Jcc rel32 placeholders */
void resolve_jumps(void)
{
	long i, fixpos, lbl, target;
	for (i = 0; i < njmp_fix; i++) {
		fixpos = jmp_fixes[i].pos;
		lbl = jmp_fixes[i].label;
#ifdef DEBUG
		if (lbl < 0 || lbl >= MAX_LABELS)
			trap(0x0305, lbl);
#endif
		target = ir_code_off[label_ir[lbl]];
		write32(fixpos, target - (fixpos + 4));
	}
}

/* patch CALL and IR_ADDR_FUNC (lea) rel32 placeholders */
void resolve_fixups(void)
{
	long i;
	for (i = 0; i < nfix; i++) {
#ifdef DEBUG
		if (fixes[i].fid < 0 || fixes[i].fid >= MAX_FUNC)
			trap(0x0313, fixes[i].fid);
#endif
		write32(fixes[i].off,
			func_off[fixes[i].fid] - (fixes[i].off + 4));
	}
}
