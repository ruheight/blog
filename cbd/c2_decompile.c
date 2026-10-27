/*
 * c2_decompile.c — IR→AST raise pass
 *
 * Linear: raise_stmt and raise_expr parse IR sequentially.
 * N_LOG_AND/N_LOG_OR recognised via XOR_SELF+JZ/JNZ pattern in raise_expr.
 *
 * Control flow:
 *   WHILE:    prescan_loops provides loops[].top (JZ), .exit, .back
 *   DO_WHILE: prescan_loops provides dws[].top, .cond (JNZ), .exit,
 *             .cond_start
 *   IF/ELSE:  JZ forward without loop match → IF.
 *   BREAK:    forward JMP to enclosing loop exit
 *   CONTINUE: backward JMP (while) or forward JMP to dw condition range
 */

/* check if target is a known loop exit for a loop enclosing from_ip */
long is_enclosing_exit(long target, long from_ip)
{
	long i;
	for (i = 0; i < nloops; i++) {
		if (loops[i].exit == target &&
		    from_ip >= loops[i].cond && from_ip <= loops[i].back)
			return 1;
	}
	for (i = 0; i < ndowhiles; i++) {
		if (dws[i].exit == target &&
		    from_ip >= dws[i].top && from_ip < dws[i].exit)
			return 1;
	}
	return 0;
}

/* look up do-while by top IR index, return cond and exit (-1 if not found) */
long get_dw(long top_idx, long *out_exit)
{
	long i;
	for (i = 0; i < ndowhiles; i++) {
		if (dws[i].top == top_idx) {
			*out_exit = dws[i].exit;
			return dws[i].cond;
		}
	}
	*out_exit = -1;
	return -1;
}

long get_loop_exit(long header_idx)
{
	long i;
	for (i = 0; i < nloops; i++) {
		if (loops[i].top == header_idx)
			return loops[i].exit;
	}
	return -1;
}

/* detect while/do-while loops from backward jumps.
 * WHILE: backward JMP whose target leads to JZ(forward) = loop exit.
 * DO_WHILE: backward JNZ = backedge, cond_start heuristic from last store/jump. */
void prescan_loops(void)
{
	long i, j, target;
	long dup_lk, found;
	long skip_jmp, jk;

	nloops = 0;
	ndowhiles = 0;

	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_JMP && ir[i].s1 < i) {
			target = ir[i].s1;
			for (j = target; j < i; j++) {
				if (ir[j].op == IR_JZ && ir[j].s2 > i) {
					found = 0;
					for (dup_lk = 0; dup_lk < nloops; dup_lk++) {
						if (loops[dup_lk].top == j) {
							loops[dup_lk].back = i;
							found = 1;
							break;
						}
					}
					if (!found) {
						loops[nloops].cond = target;
						loops[nloops].top = j;
						loops[nloops].exit = ir[j].s2;
						loops[nloops].back = i;
#ifdef DEBUG
						if (nloops >= MAX_LOOPS) trap(0x040B, nloops);
#endif
						nloops++;
					}
					break;
				}
			}
		}

		if (ir[i].op == IR_JNZ && ir[i].s2 < i) {
			dws[ndowhiles].top = ir[i].s2;
			dws[ndowhiles].cond = i;
			dws[ndowhiles].exit = i + 1;
			dws[ndowhiles].cond_start = i;
			for (j = i - 1; j >= ir[i].s2; j--) {
				if (ir[j].op == IR_JNZ && ir[j].s2 < j) {
					dws[ndowhiles].cond_start = j + 1;
					break;
				}
				if (ir[j].op == IR_JMP && ir[j].s1 < j) {
					dws[ndowhiles].cond_start = j + 1;
					break;
				}
				/* forward JMP inside do-while: may mark cond region start.
				 * Skip if part of LOG_AND/LOG_OR (target-1 is XOR_SELF).
				 * Skip if this is an if-else separator: a JZ exists
				 * earlier that jumps to j+1 (the else label). */
				if (ir[j].op == IR_JMP && ir[j].s1 > j && ir[j].s1 <= i) {
					skip_jmp = 0;
					if (ir[j].s1 > 0 && ir[ir[j].s1 - 1].op == IR_CLR)
						skip_jmp = 1;
					/* check for if-else pattern: JZ → j+1 before this JMP */
					if (!skip_jmp) {
						for (jk = j - 1; jk >= ir[i].s2; jk--) {
							if (ir[jk].op == IR_JZ && ir[jk].s2 == j + 1) {
								skip_jmp = 1;
								break;
							}
						}
					}
					if (!skip_jmp) {
						dws[ndowhiles].cond_start = ir[j].s1;
						break;
					}
				}
				if (ir[j].op == IR_STORE_LOC ||
				    ir[j].op == IR_STORE_GLOB ||
				    ir[j].op == IR_DEREF_ST) {
					dws[ndowhiles].cond_start = j + 1;
					break;
				}
			}
#ifdef DEBUG
			if (ndowhiles >= MAX_LOOPS) trap(0x040C, ndowhiles);
#endif
			ndowhiles++;
		}
	}
}

/* global variable tracking: BSS address → gid mapping.
 * All fields are int (addr/gid/width/_pad). */
struct global {
	int addr;
	int gid;
	int width;     /* element width observed from IR (1/2/4/8), 0 = unknown */
	int has_store; /* set if any STORE_GLOB to this addr exists in current func */
};

struct global globals[MAX_GLOBALS];
long nglobals;

void add_global(long addr)
{
	long i;
	for (i = 0; i < nglobals; i++)
		if (globals[i].addr == addr) return;
#ifdef DEBUG
	if (nglobals >= MAX_GLOBALS) trap(0x0500, nglobals);
#endif
	globals[nglobals].addr = addr;
	globals[nglobals].gid = GLOBAL_ID_BASE + nglobals;
	globals[nglobals].width = 0;
	nglobals++;
}

void copy_global(long dst, long src)
{
	globals[dst].addr = globals[src].addr;
	globals[dst].gid = globals[src].gid;
	globals[dst].width = globals[src].width;
	globals[dst].has_store = globals[src].has_store;
}

void sort_globals(void)
{
	long i, j;
	struct global tmp;
	for (i = 1; i < nglobals; i++) {
		tmp.addr = globals[i].addr;
		tmp.gid = globals[i].gid;
		tmp.width = globals[i].width;
		tmp.has_store = globals[i].has_store;
		j = i - 1;
		while (j >= 0 && globals[j].addr > tmp.addr) {
			copy_global(j + 1, j);
			j--;
		}
		globals[j + 1].addr = tmp.addr;
		globals[j + 1].gid = tmp.gid;
		globals[j + 1].width = tmp.width;
		globals[j + 1].has_store = tmp.has_store;
	}
	/* reassign gids in address order */
	for (i = 0; i < nglobals; i++)
		globals[i].gid = GLOBAL_ID_BASE + i;
}

/* binary search globals[] by addr, return index or -1 */
long addr_to_glob_idx(long addr)
{
	long lo, hi, mid;
	lo = 0;
	hi = nglobals - 1;
	while (lo <= hi) {
		mid = (lo + hi) / 2;
		if (globals[mid].addr == addr)
			return mid;
		if (globals[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

long addr_to_gid(long addr)
{
	long idx;
	idx = addr_to_glob_idx(addr);
	if (idx >= 0)
		return globals[idx].gid;
#ifdef DEBUG
	trap(0x0501, addr);
#endif
	return GLOBAL_ID_BASE;
}

/* local variable tracking: stack displacement → variable ID mapping.
 * All fields are int (disp/vid/width/_pad). */
struct local {
	int disp;
	int vid;
	int width;      /* element width observed from IR (1/2/4/8), 0 = unknown */
	int has_direct; /* set by pass 1 if LOAD_LOC/STORE_LOC observed */
};

struct local locals_tab[MAX_LOCALS];
long nlocals;
long next_vid;

void reset_locals(void)
{
	long i;
	nlocals = 0;
	next_vid = 0;
	for (i = 0; i < MAX_LOCALS; i++) {
		locals_tab[i].width = 0;
		locals_tab[i].has_direct = 0;
	}
}

long disp_to_vid(long disp)
{
	long i;
	for (i = 0; i < nlocals; i++) {
		if (locals_tab[i].disp == disp)
			return locals_tab[i].vid;
	}
#ifdef DEBUG
	if (nlocals >= MAX_LOCALS) trap(0x0502, nlocals);
#endif
	locals_tab[nlocals].disp = disp;
	locals_tab[nlocals].vid = next_vid;
	nlocals++;
	next_vid++;
	return next_vid - 1;
}

/* collect BSS addresses from IR for global variable table.
 * Also records observed width from ir[].imm (first nonzero wins). */
void scan_globals_in_ir(void)
{
	long i, j, addr, w;
	for (i = 0; i < nir; i++) {
		addr = -1;
		w = 0;
		if (ir[i].op == IR_LOAD_GLOB || ir[i].op == IR_ADDR_GLOB) {
			addr = ir[i].s1;
			w = ir[i].imm;
		}
		if (ir[i].op == IR_STORE_GLOB) {
			addr = ir[i].s1;
			w = ir[i].imm;
		}
		if (addr >= 0) {
			add_global(addr);
			if (w > 0) {
				for (j = 0; j < nglobals; j++) {
					if (globals[j].addr == addr && globals[j].width == 0) {
						globals[j].width = w;
						break;
					}
				}
			}
		}
	}
}

/* scan IR to determine observed width for each local variable.
 * Pass 0: register ADDR_LOC displacements so ADDR-only vars (arrays) exist.
 * Pass 1: direct width from LOAD_LOC/STORE_LOC ir[].imm.
 * Pass 2: ADDR_LOC-only vars: trace vreg to DEREF_LD/ST for element width.
 * Pass 3: default unobserved to 8. */
long vreg_deref_w[MAX_VREG]; /* min deref width per vreg (temp, pass 2) */

void scan_local_widths(void)
{
	long i, j, k, disp, w, v, dw, av;

	/* pass 0: register ADDR_LOC displacements */
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_ADDR_LOC)
			disp_to_vid(ir[i].s1);
	}

	/* pass 1: direct width from LOAD_LOC / STORE_LOC */
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_LOAD_LOC || ir[i].op == IR_STORE_LOC) {
			disp = ir[i].s1;
			w = ir[i].imm;
			if (w <= 0) w = 8;
			for (j = 0; j < nlocals; j++) {
				if (locals_tab[j].disp == disp) {
					locals_tab[j].has_direct = 1;
					if (locals_tab[j].width == 0)
						locals_tab[j].width = w;
					break;
				}
			}
		}
	}

	/* pass 2: ADDR_LOC-only vars — determine element width from DEREF consumers.
	 * Skip locals that got width from LOAD_LOC/STORE_LOC in pass 1 (has_direct).
	 *
	 * 2a: collect min deref width per vreg (direct DEREF_LD/ST consumers).
	 * 2b: backward-propagate through ADD/SUB chains (2 iterations for depth-2).
	 * 2c: apply to ADDR_LOC locals. */

	/* 2a: direct deref widths */
	for (i = 0; i < next_vreg && i < MAX_VREG; i++)
		vreg_deref_w[i] = 0;
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_DEREF_LD) {
			v = ir[i].s1;
			dw = ir[i].s2;
			if (v > 0 && v < MAX_VREG && dw > 0 && dw <= 8)
				if (vreg_deref_w[v] == 0 || dw < vreg_deref_w[v])
					vreg_deref_w[v] = dw;
		}
		if (ir[i].op == IR_DEREF_ST) {
			v = ir[i].s1;
			dw = ir[i].dst;
			if (v > 0 && v < MAX_VREG && dw > 0 && dw <= 8)
				if (vreg_deref_w[v] == 0 || dw < vreg_deref_w[v])
					vreg_deref_w[v] = dw;
		}
	}

	/* 2b: backward-propagate: if ADD/SUB dst has deref_w, propagate to s1/s2.
	 * Two iterations handle chains of depth 2 (ADDR_LOC -> ADD -> ADD -> DEREF). */
	for (k = 0; k < 2; k++) {
		for (i = nir - 1; i >= 0; i--) {
			if (ir[i].op == IR_ADD || ir[i].op == IR_SUB) {
				v = ir[i].dst;
				if (v > 0 && v < MAX_VREG && vreg_deref_w[v] > 0) {
					dw = vreg_deref_w[v];
					av = ir[i].s1;
					if (av > 0 && av < MAX_VREG)
						if (vreg_deref_w[av] == 0 || dw < vreg_deref_w[av])
							vreg_deref_w[av] = dw;
					av = ir[i].s2;
					if (av > 0 && av < MAX_VREG)
						if (vreg_deref_w[av] == 0 || dw < vreg_deref_w[av])
							vreg_deref_w[av] = dw;
				}
			}
		}
	}

	/* 2c: apply to ADDR_LOC locals (skip has_direct) */
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_ADDR_LOC) {
			disp = ir[i].s1;
			v = ir[i].dst;
			if (v > 0 && v < MAX_VREG) {
				dw = vreg_deref_w[v];
				if (dw > 0 && dw <= 8) {
					for (j = 0; j < nlocals; j++) {
						if (locals_tab[j].disp == disp) {
							if (!locals_tab[j].has_direct) {
								if (locals_tab[j].width == 0 || dw < locals_tab[j].width)
									locals_tab[j].width = dw;
							}
							break;
						}
					}
				}
			}
		}
	}

	/* pass 3: default unobserved locals to width 8 */
	for (j = 0; j < nlocals; j++) {
		if (locals_tab[j].width == 0)
			locals_tab[j].width = 8;
	}
}

/* AST emit */

long em(long kind, long a, long b_, long val)
{
#ifdef DEBUG
	if (nn >= 131072) trap(0x0511, nn);
#endif
	ast_out[nn].kind = kind;
	ast_out[nn].a = a;
	ast_out[nn].b = b_;
	ast_out[nn].val = val;
	nn++;
	return nn - 1;
}

/* copy existing AST node to output, return its new index */
void emit_copy(long src)
{
#ifdef DEBUG
	if (nn >= 131072) trap(0x0512, nn);
#endif
	ast_out[nn].kind = ast_out[src].kind;
	ast_out[nn].a = ast_out[src].a;
	ast_out[nn].b = ast_out[src].b;
	ast_out[nn].val = ast_out[src].val;
	nn++;
}

/* raise */

long raise_ip;

/* per-vreg metadata for the raise pass.
 * All fields are int.  .used (use-count) was previously a separate
 * vreg_used[] array; now merged here for locality. */
struct vreg_info {
	int def;      /* IR index where this vreg is defined (-1 = param/unknown) */
	int is_var;   /* local variable ID if stored to local, else -1 */
	int store_at; /* IR index of STORE_LOCAL for this vreg, else -1 */
	int emitted;  /* 1 if this vreg's expression was already emitted as assignment */
	int used;     /* how many times this vreg is used as an operand */
	int _pad;
};
struct vreg_info vregs[MAX_VREG];

/* count how many times each vreg is used as an operand.
 * Populates vregs[v].used.  Placed here (after struct vreg_info) because
 * it writes into the .used field.  Called after build_vreg_defs(). */
void build_vreg_used(void)
{
	long i;
#ifdef DEBUG
	if (next_vreg >= MAX_VREG) trap(0x0421, next_vreg);
#endif
	for (i = 0; i < next_vreg; i++)
		vregs[i].used = 0;
	for (i = 0; i < nir; i++) {
		if (s1_is_vreg(i) && ir[i].s1 > 0) {
#ifdef DEBUG
			if (ir[i].s1 >= MAX_VREG) trap(0x0422, ir[i].s1);
#endif
			vregs[ir[i].s1].used++;
		}
		if (s2_is_vreg(i) && ir[i].s2 > 0) {
#ifdef DEBUG
			if (ir[i].s2 >= MAX_VREG) trap(0x0422, ir[i].s2);
#endif
			vregs[ir[i].s2].used++;
		}
	}
}

/* build vreg→definition-index table (skip stores, jumps, params) */
void build_vreg_defs(void)
{
	long i;
#ifdef DEBUG
	if (next_vreg >= MAX_VREG) trap(0x0421, next_vreg);
#endif
	for (i = 0; i < next_vreg; i++) {
		vregs[i].def = -1;
		vregs[i].is_var = -1;
		vregs[i].store_at = -1;
		vregs[i].emitted = 0;
		vregs[i].used = 0;
	}
	for (i = 0; i < nir; i++) {
		/* dst is a vreg for expression ops, but fid/width/varid for
		 * FUNC/DEREF_ST/PARAM/etc — only touch vreg-dst instructions */
		if (ir[i].dst > 0 &&
		    ir[i].op != IR_STORE_LOC && ir[i].op != IR_STORE_GLOB &&
		    ir[i].op != IR_DEREF_ST && ir[i].op != IR_PARAM &&
		    ir[i].op != IR_FUNC &&
		    ir[i].op != IR_JMP && ir[i].op != IR_JZ && ir[i].op != IR_JNZ &&
		    ir[i].op != IR_RET && ir[i].op != IR_RET_VOID &&
		    ir[i].op != IR_ARG && ir[i].op != IR_NOP) {
#ifdef DEBUG
			if (ir[i].dst >= MAX_VREG) trap(0x0423, ir[i].dst);
#endif
			vregs[ir[i].dst].def = i;
		}
	}
}

/* link vregs to local variables via STORE_LOCAL (first store wins) */
void build_vreg_vars(void)
{
	long i, v, d;
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_STORE_LOC) {
			v = ir[i].s2;
			d = ir[i].s1;
#ifdef DEBUG
			if (v >= MAX_VREG) trap(0x0424, v);
#endif
			if (v > 0 && vregs[v].is_var < 0) {
				vregs[v].is_var = disp_to_vid(d);
				vregs[v].store_at = i;
			}
		}
	}
}

/* raise_expr: expression tree from vreg */

#define MAX_CALL_DEPTH 8
int call_args[48]; /* MAX_CALL_DEPTH * MAX_PARAMS */
long call_depth;

long raise_expr(long v);

/* collect IR_ARG operands for a CALL at ir[di], raise each arg,
 * emit copies into ast, return arg_start index in ast_out[]. */
long collect_call_args(long di, long argc)
{
	long ci, ai2, arg_start;
	call_depth++;
#ifdef DEBUG
	if (call_depth >= MAX_CALL_DEPTH) trap(0x041A, call_depth);
#endif
#ifdef MEASURE
	{ extern long wm_call_depth; if (call_depth > wm_call_depth) wm_call_depth = call_depth; }
#endif
	ai2 = 0;
	for (ci = di - argc; ci < di; ci++) {
		if (ir[ci].op == IR_ARG && ai2 < MAX_PARAMS) {
			call_args[(call_depth - 1) * MAX_PARAMS + ai2] = raise_expr(ir[ci].s1);
			ai2++;
		}
	}
	call_depth--;
	arg_start = nn;
	for (ai2 = 0; ai2 < argc && ai2 < MAX_PARAMS; ai2++)
		emit_copy(call_args[call_depth * MAX_PARAMS + ai2]);
	return arg_start;
}

long raise_expr(long v)
{
	long di, op, s1, s2, a, b_;
	long ci, arg_start;

	long xi, ji, ji2, right_v;

	if (v <= 0) {
#ifdef DEBUG
		if (v < 0) trap(0x1400, v);
#endif
		return em(N_NUM, 0, 0, 0);
	}
#ifdef DEBUG
	if (v >= MAX_VREG) trap(0x140E, v);
#endif
	if (vregs[v].def < 0) {
		if (v >= 1 && v <= 6)
			return em(N_VAR, 0, 0, v - 1);
#ifdef DEBUG
		trap(0x1401, v);
#endif
		return em(N_NUM, 0, 0, 0);
	}
	di = vregs[v].def;
	op = ir[di].op;
	s1 = ir[di].s1;
	s2 = ir[di].s2;

	/* var reversal: if vreg stored to local before current use, emit VAR */
	if (vregs[v].is_var >= 0 && vregs[v].store_at >= 0 &&
	    vregs[v].store_at < raise_ip)
		return em(N_VAR, 0, 0, vregs[v].is_var);
	if (vregs[v].emitted && vregs[v].is_var >= 0)
		return em(N_VAR, 0, 0, vregs[v].is_var);

	if (op == IR_LOAD_IMM) return em(N_NUM, 0, 0, ir[di].imm);
	if (op == IR_CLR) {
		/*
		 * LOG_AND pattern:
		 *   ji: JZ left_v -> di
		 *   ...(right eval)...
		 *   di-2: TEST_SETNE right_v
		 *   di-1: JMP -> di+1
		 *   di:   XOR_SELF (this insn)
		 *
		 * LOG_OR pattern:
		 *   ji: JNZ left_v -> di-2
		 *   ...(right eval)...
		 *   ki: JZ right_v -> di
		 *   di-2: LOAD_IMM 1
		 *   di-1: JMP -> di+1
		 *   di:   XOR_SELF (this insn)
		 */
		xi = di;
		if (xi >= 2 && ir[xi - 1].op == IR_JMP && ir[xi - 1].s1 == xi + 1) {
			/* LOG_AND? */
			if (ir[xi - 2].op == IR_SETNE) {
				for (ji = xi - 3; ji >= 0; ji--) {
					if (ir[ji].op == IR_JZ && ir[ji].s2 == xi) {
						a = raise_expr(ir[ji].s1);
						b_ = raise_expr(ir[xi - 2].s1);
						return em(N_LOG_AND, a, b_, 0);
					}
				}
			}
			/* LOG_OR?
			 * xi-1: JMP → xi+1  (already checked above)
			 * xi-2: LOAD_IMM 1  (true label)
			 * ji:   JZ right_v → xi  (false branch)
			 * ji2:  JNZ left_v → xi-2  (short-circuit to true)
			 */
			if (xi >= 3 && ir[xi - 2].op == IR_LOAD_IMM && ir[xi - 2].imm == 1) {
				for (ji = xi - 3; ji >= 0; ji--) {
					if (ir[ji].op == IR_JZ && ir[ji].s2 == xi) {
						right_v = ir[ji].s1;
						for (ji2 = ji - 1; ji2 >= 0; ji2--) {
							if (ir[ji2].op == IR_JNZ && ir[ji2].s2 == xi - 2) {
								a = raise_expr(ir[ji2].s1);
								b_ = raise_expr(right_v);
								return em(N_LOG_OR, a, b_, 0);
							}
						}
					}
				}
			}
		}
		/* plain XOR self (e.g. x ^ x) */
		if (s1 > 0 && s1 < MAX_VREG && vregs[s1].def >= 0 &&
		    (ir[vregs[s1].def].op == IR_LOAD_LOC ||
		     ir[vregs[s1].def].op == IR_LOAD_GLOB)) {
			a = raise_expr(s1);
			b_ = raise_expr(s1);
			return em(N_XOR, a, b_, 0);
		}
#ifdef DEBUG
		trap(0x1403, di);
#endif
		return em(N_NUM, 0, 0, 0);
	}
	if (op == IR_LOAD_LOC) return em(N_VAR, 0, 0, disp_to_vid(s1));
	if (op == IR_LOAD_GLOB) {
		if (vregs[v].is_var >= 0 && vregs[v].store_at >= 0 &&
		    (vregs[v].store_at < raise_ip || vregs[v].emitted)) {
			/* O(1) check: does this global have any store in current func? */
			ci = addr_to_glob_idx(s1);
			if (ci >= 0 && globals[ci].has_store)
				return em(N_VAR, 0, 0, vregs[v].is_var);
		}
		return em(N_VAR, 0, 0, addr_to_gid(s1));
	}
	if (op == IR_ADDR_LOC) {
		a = em(N_VAR, 0, 0, disp_to_vid(s1));
		return em(N_ADDR, a, 0, 0);
	}
	if (op == IR_ADDR_GLOB) {
		a = em(N_VAR, 0, 0, addr_to_gid(s1));
		return em(N_ADDR, a, 0, 0);
	}
	if (op == IR_DEREF_LD) {
		a = raise_expr(s1);
		return em(N_DEREF, a, s2, 0);
	}

	if (op == IR_NEG) { a = raise_expr(s1); return em(N_NEG, a, 0, 0); }
	if (op == IR_NOT) { a = raise_expr(s1); return em(N_NOT, a, 0, 0); }
	if (op >= IR_ADD && op <= IR_XOR) {
		a = raise_expr(s1);
		if (ir[di].imm != 0)
			b_ = em(N_NUM, 0, 0, ir[di].imm);
		else
			b_ = raise_expr(s2);
		return em(N_ADD + (op - IR_ADD), a, b_, 0);
	}
	if (op >= IR_EQ && op <= IR_GEU) {
		a = raise_expr(s1);
		if (ir[di].imm != 0)
			b_ = em(N_NUM, 0, 0, ir[di].imm);
		else
			b_ = raise_expr(s2);
		return em(N_EQ + (op - IR_EQ), a, b_, 0);
	}
	if (op == IR_LNOT) {
		a = raise_expr(s1);
		return em(N_LOG_NOT, a, 0, 0);
	}
	if (op == IR_SETNE) {
		a = raise_expr(s1);
		b_ = em(N_NUM, 0, 0, 0);
		return em(N_NE, a, b_, 0);
	}
	/* IR_ADDR_FUNC → N_FUNC_ADDR: function pointer (lea [rip+disp] targeting
	 * a known function, decoded via IR_ADDR_FUNC in c2_decode.c) */
	if (op == IR_ADDR_FUNC) {
		return em(N_FUNC_ADDR, 0, 0, s1);
	}
	if (op == IR_CALL) {
		arg_start = collect_call_args(di, s2);
		return em(N_CALL, arg_start, s2, s1);
	}
	if (op == IR_CALL_IND) {
		/* callee address must be raised before arg emit (original order) */
		arg_start = collect_call_args(di, s2);
		a = raise_expr(s1);
		return em(N_CALL_IND, a, s2, arg_start);
	}
#ifdef DEBUG
	trap(0x1402, op);
#endif
	return em(N_NUM, 0, 0, 0);
}

/* raise_stmt / raise_block */

long raise_stmt(long end_ir);

long raise_block(long end_ir)
{
	long start, count, sn, ci;
	long stmts[MAX_STMTS];
	count = 0;
	while (raise_ip < end_ir) {
		sn = raise_stmt(end_ir);
		if (sn < 0) continue;
#ifdef DEBUG
		if (count >= MAX_STMTS) trap(0x0510, count);
#endif
		stmts[count] = sn;
		count++;
	}
#ifdef MEASURE
	{ extern long wm_stmts; if (count > wm_stmts) wm_stmts = count; }
#endif
	start = nn;
	for (ci = 0; ci < count; ci++)
		emit_copy(stmts[ci]);
	return em(N_BLOCK, start, count, 0);
}

long raise_stmt(long end_ir)
{
	long op, s1, s2, i;
	long vid, gid, lhs, rhs, width;
	long cond, then_nd, else_nd, body_nd;
	long target_ir, jmp_ir, jmp_target;
	long exit_ir;
	long dw_c, dw_e;
	long lk, is_back, is_cont, used;

	if (raise_ip >= end_ir) return -1;

	i = raise_ip;
	op = ir[i].op;
	s1 = ir[i].s1;
	s2 = ir[i].s2;

	/* skip NOP */
	if (op == IR_NOP) {
		raise_ip++;
		return -1;
	}

	/* DO_WHILE body starts at current IR position, runs to dw_cond */
	dw_c = get_dw(i, &dw_e);
	if (dw_c >= 0 && end_ir != dw_c) {
		body_nd = raise_block(dw_c);
		/* raise condition from JNZ */
		if (raise_ip < nir && ir[raise_ip].op == IR_JNZ)
			cond = raise_expr(ir[raise_ip].s1);
		else
			cond = em(N_NUM, 0, 0, 1);
		raise_ip = dw_e;
		return em(N_DO_WHILE, body_nd, cond, 0);
	}

	if (op == IR_STORE_LOC) {
		/* skip redundant stores (vreg already emitted as assignment) */
		if (s2 > 0 && s2 < MAX_VREG && vregs[s2].emitted) {
			raise_ip++;
			return -1;
		}
		vid = disp_to_vid(s1);
		rhs = raise_expr(s2);
		lhs = em(N_VAR, 0, 0, vid);
		raise_ip++;
		return em(N_ASSIGN, lhs, rhs, 0);
	}
	if (op == IR_STORE_GLOB) {
		gid = addr_to_gid(s1);
		rhs = raise_expr(s2);
		lhs = em(N_VAR, 0, 0, gid);
		raise_ip++;
		return em(N_ASSIGN, lhs, rhs, 0);
	}
	if (op == IR_DEREF_ST) {
		width = ir[i].dst;
		lhs = raise_expr(s1);
		lhs = em(N_DEREF, lhs, width, 0);
		rhs = raise_expr(s2);
		raise_ip++;
		return em(N_ASSIGN, lhs, rhs, 0);
	}
	if (op == IR_RET) {
		rhs = raise_expr(s1);
		raise_ip++;
		return em(N_RETURN, rhs, 0, 0);
	}
	if (op == IR_RET_VOID) {
		if (end_ir >= nir && i + 1 >= nir) {
			raise_ip++;
			return -1;
		}
		raise_ip++;
		return em(N_RETURN, 0, 0, 0);
	}

	/* JZ: WHILE or IF */
	if (op == IR_JZ) {
		target_ir = s2;

		/* skip JZ that is part of LOG_AND/LOG_OR pattern */
		if (target_ir >= 0 && target_ir < nir &&
		    ir[target_ir].op == IR_CLR) {
			raise_ip++;
			return -1;
		}

		cond = raise_expr(s1);
		raise_ip++;

		/* WHILE: this JZ is a known loop header */
		exit_ir = get_loop_exit(i);
		if (exit_ir >= 0) {
			body_nd = raise_block(exit_ir);
			if (raise_ip < exit_ir)
				raise_ip = exit_ir;
			return em(N_WHILE, cond, body_nd, 0);
		}

		/* IF: parse then-body up to target */
		then_nd = raise_block(target_ir);

		/* check for else separator */
		jmp_ir = raise_ip - 1;
		if (jmp_ir >= 0 && ir[jmp_ir].op == IR_JMP) {
			jmp_target = ir[jmp_ir].s1;
			if (jmp_target > target_ir && !is_enclosing_exit(jmp_target, jmp_ir)) {
				/* check if it's a continue, not else-separator */
				is_cont = 0;
				for (lk = 0; lk < ndowhiles; lk++) {
					if (jmp_target >= dws[lk].cond_start && jmp_target <= dws[lk].cond) {
						is_cont = 1;
						break;
					}
				}
				for (lk = 0; lk < nloops; lk++) {
					if (jmp_target >= loops[lk].cond && jmp_target <= loops[lk].top) {
						is_cont = 1;
						break;
					}
				}
				if (!is_cont) {
					else_nd = raise_block(jmp_target);
					return em(N_IF, cond, then_nd, else_nd);
				}
			}
		}
		return em(N_IF, cond, then_nd, 0);
	}

	/* JNZ: DO_WHILE condition or part of LOG_OR */
	if (op == IR_JNZ) {
		/* skip JNZ that is part of LOG_OR pattern:
		 * JNZ jumps to LOAD_IMM 1 followed by JMP */
		target_ir = s2;
		if (target_ir >= 0 && target_ir < nir &&
		    ir[target_ir].op == IR_LOAD_IMM && ir[target_ir].imm == 1 &&
		    target_ir + 1 < nir && ir[target_ir + 1].op == IR_JMP) {
			raise_ip++;
			return -1;
		}
		raise_ip++;
		return -1;
	}

	/* JMP */
	if (op == IR_JMP) {
		target_ir = s1;

		/* backward: backedge or continue */
		if (target_ir <= i) {
			is_back = 0;
			for (lk = 0; lk < nloops; lk++) {
				if (target_ir >= loops[lk].cond && target_ir <= loops[lk].top &&
				    i == loops[lk].back) {
					if (i + 1 >= loops[lk].exit)
						is_back = 1;
					break;
				}
			}
			if (is_back) {
				raise_ip++;
				return -1;
			}
			raise_ip++;
			return em(N_CONTINUE, 0, 0, 0);
		}

		/* BREAK vs CONTINUE disambiguation:
		 * target can be both a dw exit (break inner) and
		 * a dw cond_start (continue outer).  Check break first
		 * for the innermost enclosing loop. */
		if (is_enclosing_exit(target_ir, i)) {
			raise_ip++;
			return em(N_BREAK, 0, 0, 0);
		}

		/* forward to do_while condition range = CONTINUE or body-end suppress */
		for (lk = 0; lk < ndowhiles; lk++) {
			if (target_ir >= dws[lk].cond_start && target_ir <= dws[lk].cond) {
				if (end_ir == dws[lk].cond) {
					raise_ip++;
					return -1; /* body-end structural JMP */
				}
				raise_ip++;
				return em(N_CONTINUE, 0, 0, 0);
			}
		}

		/* forward JMP within range — skip (fallthrough/glue) */
		raise_ip++;
		return -1;
	}

	/* void call: result vreg not used, emit as expression-statement */
	if (op == IR_CALL) {
		used = 0;
		if (ir[i].dst > 0) {
#ifdef DEBUG
			if (ir[i].dst >= MAX_VREG) trap(0x0425, ir[i].dst);
#endif
			used = vregs[ir[i].dst].used > 0;
		}
		if (used) {
			raise_ip++;
			return -1;
		}
		rhs = raise_expr(ir[i].dst);
		raise_ip++;
		return rhs;
	}

	/* void indirect call */
	if (op == IR_CALL_IND) {
		used = 0;
		if (ir[i].dst > 0) {
#ifdef DEBUG
			if (ir[i].dst >= MAX_VREG) trap(0x0425, ir[i].dst);
#endif
			used = vregs[ir[i].dst].used > 0;
		}
		if (used) {
			raise_ip++;
			return -1;
		}
		rhs = raise_expr(ir[i].dst);
		raise_ip++;
		return rhs;
	}

	/* skip anything else */
	raise_ip++;
	return -1;
}

int vid_map[MAX_LOCALS];

/* raise IR to AST for one function: build expr trees, detect control flow,
 * sort and renumber locals in displacement order for deterministic output */
void raise_function(long fn_idx)
{
	long func_slot, body_nd, npar, i, k, start;
	long prev_disp, cur_disp, slots, ew, al, data_end;
	long old_vid, new_vid, body_start;
	long j, td, tv;

	func_slot = 1 + nglobals + fn_idx;
	npar = funcs[fn_idx].npar;

	/* locals_tab already populated by decode_function (param saves) */

	prescan_loops();
	build_vreg_defs();
	build_vreg_used();
	build_vreg_vars();
	scan_local_widths();

	/* precompute has_store per global for this function (O(1) lookup in raise_expr) */
	for (i = 0; i < nglobals; i++)
		globals[i].has_store = 0;
	for (i = 0; i < nir; i++) {
		if (ir[i].op == IR_STORE_GLOB) {
			j = addr_to_glob_idx(ir[i].s1);
			if (j >= 0)
				globals[j].has_store = 1;
		}
	}

	call_depth = 0;
	raise_ip = 0;

	body_start = nn;
	body_nd = raise_block(nir);

	/* sort non-param locals by displacement (descending = most negative first) */
	for (i = npar + 1; i < nlocals; i++) {
		for (j = i; j > npar && locals_tab[j-1].disp < locals_tab[j].disp; j--) {
			td = locals_tab[j].disp; tv = locals_tab[j].vid; ew = locals_tab[j].width;
			locals_tab[j].disp = locals_tab[j-1].disp;
			locals_tab[j].vid = locals_tab[j-1].vid;
			locals_tab[j].width = locals_tab[j-1].width;
			locals_tab[j-1].disp = td;
			locals_tab[j-1].vid = tv;
			locals_tab[j-1].width = ew;
		}
	}

	/* renumber non-param locals to sequential IDs in displacement order */
	for (i = 0; i < MAX_LOCALS; i++)
		vid_map[i] = i;
	for (i = npar; i < nlocals; i++) {
		old_vid = locals_tab[i].vid;
		new_vid = i;
		if (old_vid != new_vid && old_vid < MAX_LOCALS)
			vid_map[old_vid] = new_vid;
		locals_tab[i].vid = new_vid;
	}
	for (k = body_start; k < nn; k++) {
		if ((ast_out[k].kind == N_VAR || ast_out[k].kind == N_DECL) &&
		    ast_out[k].val >= 0 && ast_out[k].val < GLOBAL_ID_BASE &&
		    ast_out[k].val < MAX_LOCALS)
			ast_out[k].val = vid_map[ast_out[k].val];
	}

	/* emit DECL nodes: params first (always width=8), then locals */
	start = nn;
	for (i = 0; i < npar; i++)
		em(N_DECL, 1, 8, locals_tab[i].vid);
	for (i = npar; i < nlocals; i++) {
		/* slot count from displacement gap, accounting for alignment.
		 * compiler: off += count*ew; off = align_up(off, al); disp=-off.
		 * find min slots where align_up(prev+slots*ew, al) >= cur. */
		cur_disp = -locals_tab[i].disp;
		if (i > 0)
			prev_disp = -locals_tab[i - 1].disp;
		else
			prev_disp = 0;
		ew = locals_tab[i].width;
		if (ew <= 0) ew = 8;
		al = ew;
		if (al > 8) al = 8;
		slots = 1;
		data_end = prev_disp + ew;
		while (((data_end + al - 1) & ~(al - 1)) < cur_disp) {
			slots++;
			data_end = prev_disp + slots * ew;
		}
		if (slots < 1) slots = 1;
		em(N_DECL, slots, ew, locals_tab[i].vid);
	}

	/* copy body stmts after decls, build combined block */
	for (i = 0; i < ast_out[body_nd].b; i++)
		emit_copy(ast_out[body_nd].a + i);

	body_nd = em(N_BLOCK, start, nlocals + ast_out[body_nd].b, 0);

	ast_out[func_slot].kind = N_FUNC;
	ast_out[func_slot].a = body_nd;
	ast_out[func_slot].b = npar;
	ast_out[func_slot].val = funcs[fn_idx].fid;
#ifdef MEASURE
	{
		extern long wm_nn, wm_nlocals;
		if (nn > wm_nn) wm_nn = nn;
		if (nlocals > wm_nlocals) wm_nlocals = nlocals;
	}
#endif
}

/* top-level: scan functions → decode all → collect globals → raise each →
 * build top-level BLOCK.  Returns total AST size in bytes (nn * 32). */
long decompile(long in, long in_len, long out_buf)
{
	long fi, main_off, main_idx, next_fid;
	long slots, ew, next_ew, next_al, data_end;

	code = (unsigned char *)in;
	code_len = in_len;
	ast_out = (struct node *)out_buf;
	nn = 0;
	build_vreg_tabs();

	main_off = 0;
	if (code_len > 5 && rb(0) == 0xE8) {
		main_off = 5 + sext32(rb(1) | (rb(2) << 8) | (rb(3) << 16) | (rb(4) << 24));
	}

	scan_functions();

	main_idx = -1;
	for (fi = 0; fi < nfuncs; fi++) {
		if (funcs[fi].off == main_off)
			main_idx = fi;
	}
	next_fid = 1;
	for (fi = 0; fi < nfuncs; fi++) {
		if (fi == main_idx)
			funcs[fi].fid = 0;
		else {
			funcs[fi].fid = next_fid;
			next_fid++;
		}
	}

	nn = 1; /* reserve node 0 for top-level BLOCK */

	/* pass 1: decode all functions, collect global addresses */
	nglobals = 0;
	for (fi = 0; fi < nfuncs; fi++) {
		decode_function(fi);
		scan_globals_in_ir();
	}
	sort_globals();

	/* emit global DECLs with observed widths.
	 * compiler: off = align_up(off, al); gvars[gid].off = off; off += count*ew.
	 * gap includes alignment of the NEXT variable.
	 * find min slots where align_up(addr+slots*ew, next_al) >= next_addr. */
	for (fi = 0; fi < nglobals; fi++) {
		ew = globals[fi].width;
		if (ew <= 0) ew = 8;
		if (fi + 1 < nglobals) {
			next_ew = globals[fi + 1].width;
			if (next_ew <= 0) next_ew = 8;
			next_al = next_ew;
			if (next_al > 8) next_al = 8;
			slots = 1;
			data_end = globals[fi].addr + ew;
			while (((data_end + next_al - 1) & ~(next_al - 1)) < globals[fi + 1].addr) {
				slots++;
				data_end = globals[fi].addr + slots * ew;
			}
		} else {
			slots = 1;
		}
		if (slots < 1) slots = 1;
		em(N_DECL, slots, ew, GLOBAL_ID_BASE + fi);
	}

	/* reserve FUNC slots */
	for (fi = 0; fi < nfuncs; fi++) {
		ast_out[nn].kind = N_FUNC;
		nn++;
	}

	/* pass 2: decode + raise each function */
	for (fi = 0; fi < nfuncs; fi++) {
		decode_function(fi);
		raise_function(fi);
	}

#ifdef MEASURE
	{
		extern long wm_nfuncs, wm_nglobals, wm_nloops, wm_ndowhiles;
		if (nfuncs > wm_nfuncs) wm_nfuncs = nfuncs;
		if (nglobals > wm_nglobals) wm_nglobals = nglobals;
		if (nloops > wm_nloops) wm_nloops = nloops;
		if (ndowhiles > wm_ndowhiles) wm_ndowhiles = ndowhiles;
	}
#endif
	/* fill top-level BLOCK */
	ast_out[0].kind = N_BLOCK;
	ast_out[0].a = 1;
	ast_out[0].b = nglobals + nfuncs;
	ast_out[0].val = 0;

	return nn * 32;
}
