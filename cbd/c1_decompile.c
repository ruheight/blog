/*
 * c1_decompile.c — Meta-C decompiler core
 *
 * Recursive descent parser for c1-generated x86-64 binaries.
 * See grammar.txt for the formal grammar.
 *
 * Architecture:
 *   pass 0 — scan_functions: find function prologues, param counts, frames
 *   pass 1 — scan_labels:    collect all jump targets, classify loop exits
 *   pass 2 — decompile:      recursive descent per function
 *
 * Label-based disambiguation:
 *   Every JE32/JNE32/JMP32 target is recorded with direction (fwd/back).
 *   A backward JMP32 target  = while L_top
 *   A backward JNE32 target  = do_while L_top
 *   The JE32 target from a while = while L_exit (= break target)
 *   The address after JNE32 in a do_while = do_while L_exit (= break target)
 *   Forward JMP32 to a known L_exit = break (not else-separator)
 *
 * Entry: long decompile(long in, long in_len, long out_buf)
 */

#include "ast.h"

struct node *ast;
long code_base;
#ifdef C1M_SHARED
long bss_base; /* shared with c1_compile.c in c1m */
#endif

unsigned char *bin;
long bin_len, ip;

long nn;

struct func {
	long off;   /* start offset in binary */
	long end;   /* one-past-last offset */
	long npar;  /* parameter count */
	long frame; /* stack frame size in bytes */
};

struct func fns[256];
long d_nfuncs;

struct global {
	long addr; /* BSS address */
	long id;   /* global variable ID */
};

struct global globals[4096];
long nglobals, next_gid;

struct local {
	long disp; /* RBP displacement */
	long vid;  /* variable ID */
};

struct local locals[256];
long nlocals;
long local_seen[512]; /* 1 if vid already registered, for O(1) disp_to_vid */

/* global stmt buffer to avoid huge stack arrays in recursive parse_stmt */
struct stmt {
	long ip; /* instruction pointer at stmt start */
	long nd; /* AST node index */
};

struct stmt g_stmts[8192];
long g_stmts_top;

/*
 * Label tables from pre-scan:
 *   loop_exit[i]      — known loop-exit addresses (break targets)
 *   dws[i].top, dws[i].test, dws[i].exit — do_while boundaries
 *     top = backward target of JNE32
 *     test = address of TEST (3 bytes before JNE32)
 *     exit = address after JNE32
 *   A forward JMP32 to addr in [top..test) = continue in that do_while.
 */
long loop_exit[1024];
long n_loop_exits;

struct dowhile {
	long top;
	long test;
	long exit;
};

struct dowhile dws[256];

/* exact continue targets: forward JMP32 targets that land in (dw_top..dw_test) */
long cont_target_addr[1024];
long n_cont_targets;

/* forward JMP candidates for deferred continue detection */
struct fwd_jmp { long off; long tgt; };
struct fwd_jmp fwd_jmps[512];

/* global ID remapping table for renumber */
long gid_map[4096];

long rb(long off) { return (long)bin[off] & 0xff; }
long pk(void) { return rb(ip); }
long pk1(void) { return rb(ip + 1); }
long pk2(void) { return rb(ip + 2); }
long pk3(void) { return rb(ip + 3); }
long pk4(void) { return rb(ip + 4); }

long rd1(void) { long v; v = rb(ip); ip++; return v; }

/* sign-extend 32-bit value to 64-bit (Meta-C has no signed casts) */
long sext32(long v)
{
	v &= 0xffffffff;
	if (v & 0x80000000)
		v |= 0xffffffff00000000;
	return v;
}

/* read little-endian signed i32 at off (no ip advance) */
long r32(long off)
{
	return sext32(rb(off) | (rb(off+1) << 8) | (rb(off+2) << 16) | (rb(off+3) << 24));
}

long rd4(void) { long v; v = r32(ip); ip += 4; return v; }

long rd8(void)
{
	long lo, hi;
	lo = rd4() & 0xffffffff;
	hi = rd4() & 0xffffffff;
	return lo | (hi << 32);
}

/* emit one AST node, return its index */
long em(long kind, long a, long b, long val)
{
	long i;
	i = nn;
	ast[i].kind = kind;
	ast[i].a = a;
	ast[i].b = b;
	ast[i].val = val;
	nn++;
	return i;
}

/* shallow-copy AST node at src into the next slot */
void emit_copy(long src)
{
	ast[nn].kind = ast[src].kind;
	ast[nn].a = ast[src].a;
	ast[nn].b = ast[src].b;
	ast[nn].val = ast[src].val;
	nn++;
}

/* copy g_stmts[sbase..sbase+ns) nodes into ast, return N_BLOCK */
long emit_stmts_block(long sbase, long ns)
{
	long start, i;
	start = nn;
	for (i = 0; i < ns; i++)
		emit_copy(g_stmts[sbase + i].nd);
	return em(N_BLOCK, start, ns, 0);
}

/* read ModRM byte + displacement (disp8 or disp32) from ip, advance ip */
long read_disp(void)
{
	long modrm, mod, d;
	modrm = rd1();
	mod = (modrm >> 6) & 3;
	if (mod == 1) {
		d = rd1();
		if (d >= 128)
			d -= 256;
	} else {
		d = rd4();
	}
	return d;
}

/* map RBP displacement to local variable ID.
 * Derives vid from stack offset and registers new locals on first sight. */
/* map RBP displacement to local variable ID.
 * O(1) via local_seen[] direct-index (vid derived from displacement). */
long disp_to_vid(long d)
{
	long vid;
	vid = ((-d) / 8) - 1;
#ifdef DEBUG
	if (vid < 0 || vid >= 512) trap(0x0076, vid);
#endif
	if (local_seen[vid])
		return vid;
	locals[nlocals].disp = d;
	locals[nlocals].vid = vid;
	nlocals++;
	local_seen[vid] = 1;
	return vid;
}

/* resolve RIP-relative displacement to a global variable ID.
 * Allocates a new global if this BSS address hasn't been seen yet. */
long rip_to_gid(long d, long insn_end)
{
	long addr, i;
	addr = code_base + insn_end + d;
	for (i = 0; i < nglobals; i++) {
		if (globals[i].addr == addr)
			return globals[i].id;
	}
#ifdef DEBUG
	if (nglobals >= 4096)
		trap(0x0060, nglobals);
#endif
	globals[nglobals].addr = addr;
	globals[nglobals].id = next_gid;
	nglobals++;
	next_gid++;
	return next_gid - 1;
}

/* look up function ID by code offset */
long off_to_fid(long off)
{
	long i;
	for (i = 0; i < d_nfuncs; i++) {
		if (fns[i].off == off)
			return i;
	}
#ifdef DEBUG
	trap(0x0040, off); /* unknown function offset */
#endif
	return 0;
}

/* map x86 SETcc opcode to AST comparison kind (inverse of compiler's setcc_for) */
long setcc_kind(long opc)
{
	if (opc == 0x94) return N_EQ;
	if (opc == 0x95) return N_NE;
	if (opc == 0x9c) return N_LT_S;
	if (opc == 0x92) return N_LT_U;
	if (opc == 0x9f) return N_GT_S;
	if (opc == 0x97) return N_GT_U;
	if (opc == 0x9e) return N_LE_S;
	if (opc == 0x96) return N_LE_U;
	if (opc == 0x9d) return N_GE_S;
	if (opc == 0x93) return N_GE_U;
	return N_EQ;
}

/* sort a long array (insertion sort, small arrays) */
void sort_long_array(long *arr, long n)
{
	long i, j, tmp;
	for (i = 1; i < n; i++) {
		tmp = arr[i];
		j = i - 1;
		while (j >= 0 && arr[j] > tmp) {
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = tmp;
	}
}

/* binary search in sorted array */
long bsearch_long(long *arr, long n, long val)
{
	long lo, hi, mid;
	lo = 0;
	hi = n;
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (arr[mid] < val)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < n && arr[lo] == val)
		return 1;
	return 0;
}

/* check if addr was marked as a loop exit during scan_labels */
long is_loop_exit(long addr)
{
	return bsearch_long(loop_exit, n_loop_exits, addr);
}

/* add addr to a set (array + count) if not already present */
void add_to_set(long *arr, long *count, long addr, long max_sz, long trap_id)
{
	long i;
	for (i = 0; i < *count; i++) {
		if (arr[i] == addr)
			return;
	}
#ifdef DEBUG
	if (*count >= max_sz)
		trap(trap_id, *count);
#endif
	arr[*count] = addr;
	*count = *count + 1;
}

void add_loop_exit(long addr)
{
	add_to_set(loop_exit, &n_loop_exits, addr, 1024, 0x0061);
}

long n_dowhiles;

/* check if target is a loop exit of a loop that contains from_addr.
 * Checks both do/while (via dws[]) and while (via backward JMP32 at target-5). */
long is_enclosing_loop_exit(long target, long from_addr)
{
	long i, rel, wh_top;
	if (!bsearch_long(loop_exit, n_loop_exits, target))
		return 0;
	/* check do/while loops */
	for (i = 0; i < n_dowhiles; i++) {
		if (dws[i].exit == target &&
		    from_addr >= dws[i].top && from_addr < dws[i].exit)
			return 1;
	}
	/* check while loops: backward JMP32 right before target */
	if (target >= 5 && rb(target - 5) == 0xe9) {
		rel = sext32(r32(target - 4));
		wh_top = target + rel;
		if (wh_top < target && from_addr >= wh_top && from_addr < target)
			return 1;
	}
	return 0;
}

/* check if a forward JMP from from_addr to target is a do_while continue.
 * target must equal a do_while TEST addr, and from_addr must be inside
 * that do_while's body [top..exit). */
/* continue in do/while jumps forward to condition expression (before TEST).
 * target == dws[i].test - K where K = size of condition expression.
 * We check that target lands in (top, test] and from_addr is inside [top, exit),
 * AND target is the INNERMOST matching do/while (closest enclosing). */
long is_dw_continue(long target, long from_addr)
{
	long i, best, rel, wh_top;
	best = -1;
	for (i = 0; i < n_dowhiles; i++) {
		if (target > dws[i].top && target <= dws[i].test &&
		    from_addr >= dws[i].top && from_addr < dws[i].exit) {
			/* pick innermost (smallest range) */
			if (best < 0 || (dws[i].exit - dws[i].top) < (dws[best].exit - dws[best].top))
				best = i;
		}
	}
	if (best < 0)
		return 0;
	/* heuristic: target should be close to test (condition is small) */
	if (dws[best].test - target >= 64)
		return 0;
	/* reject if target is the exit of an inner loop that contains from_addr.
	 * This distinguishes break-from-inner-loop from continue-in-outer-dowhile. */
	for (i = 0; i < n_dowhiles; i++) {
		if (dws[i].exit == target &&
		    from_addr >= dws[i].top && from_addr < dws[i].exit)
			return 0;
	}
	/* also check while-loop exits: backward JMP32 right before target */
	if (target >= 5 && rb(target - 5) == 0xe9) {
		rel = sext32(r32(target - 4));
		wh_top = target + rel;
		if (wh_top < from_addr && from_addr >= wh_top && from_addr < target)
			return 0;
	}
	return 1;
}

void add_cont_target(long addr)
{
	add_to_set(cont_target_addr, &n_cont_targets, addr, 1024, 0x0062);
}

/* return byte length of c1-generated x86 instruction at off.
 * Returns 0 for unrecognized encodings (stops the scan). */
long insn_len(long off)
{
	long b0, b1, b2;
	b0 = rb(off);
	if (b0 == 0x55 || b0 == 0x50 || b0 == 0x59 || b0 == 0x58 ||
	    b0 == 0xc9 || b0 == 0xc3 || b0 == 0xf4)
		return 1;
	if (b0 == 0xe9 || b0 == 0xe8)
		return 5;           /* JMP32 / CALL32: opcode + rel32 */
	if (b0 == 0x0f) {
		b1 = rb(off + 1);
		if (b1 == 0x84 || b1 == 0x85)
			return 6;       /* JE32 / JNE32: 0F 8x + rel32 */
		if (b1 >= 0x90 && b1 <= 0x9f)
			return 3;       /* SETcc: 0F 9x C0 */
#ifdef DEBUG
		trap(0x0070, b1);   /* unknown 0F xx opcode */
#endif
		return 0;           /* stop scan on unknown 0F prefix */
	}
	if (b0 == 0x89)
		return 2;           /* DSTO4 (89 01) */
	if (b0 == 0x88)
		return 2;           /* DSTO1 (88 01) */
	if (b0 == 0x66)
		return 3;           /* DSTO2 (66 89 01) */
	b1 = rb(off + 1);
	if (b0 == 0x49 || b0 == 0x4c) {
		/* REX.W+B: 49 89 Cx (mov r8+,rax) = 3, or 4C 89 xx <disp> */
		if (b0 == 0x49)
			return 3;
		b2 = rb(off + 2);
		if ((b2 & 0xc0) == 0x40) return 4;
		return 7;
	}
	if (b0 == 0x41)
		return 2;           /* old REX.B pop (shouldn't appear now) */
	/* b0 == 0x48: REX.W prefix — most instructions */
	if (b0 != 0x48)
		return 0;
	if (b1 == 0xb8)
		return 10;          /* NUM: 48 B8 <imm64> */
	if (b1 == 0x81)
		return 7;           /* SUB RSP: 48 81 EC <imm32> */
	b2 = rb(off + 2);
	if (b1 == 0x0f) {
		/* 48 0F xx xx */
		if (b2 == 0xb6 || b2 == 0xb7)
			return 4;       /* MOVZX byte/word: 48 0F B6/B7 00/C0 */
		if (b2 == 0xaf)
			return 4;       /* IMUL: 48 0F AF C1 */
		return 4;           /* catch-all 48 0F xx xx */
	}
	if (b1 == 0x89) {
		if (b2 == 0xe5 || b2 == 0xd0 || b2 == 0x01 ||
		    b2 == 0xc7 || b2 == 0xc6 || b2 == 0xc2 || b2 == 0xc1 || b2 == 0xc0)
			return 3;       /* mov rbp,rsp / mov rax,rdx / dsto8 / mov reg,rax */
		if (b2 == 0x05)
			return 7;       /* STORE_G: 48 89 05 <d32> */
		if ((b2 & 0xc0) == 0x40) return 4;  /* STORE_L disp8 */
		if ((b2 & 0xc0) == 0x80) return 7;  /* STORE_L disp32 */
		return 3;
	}
	if (b1 == 0x8b) {
		if (b2 == 0x00) return 3;            /* DEREF8 */
		if (b2 == 0x05) return 7;            /* LOAD_G */
		if ((b2 & 0xc0) == 0x40) return 4;  /* LOAD_L disp8 */
		if ((b2 & 0xc0) == 0x80) return 7;  /* LOAD_L disp32 */
		return 3;
	}
	if (b1 == 0x8d) {
		if (b2 == 0x05) return 7;            /* ADDR_G / LEA RIP */
		if ((b2 & 0xc0) == 0x40) return 4;  /* LEA_L disp8 */
		if ((b2 & 0xc0) == 0x80) return 7;  /* LEA_L disp32 */
		return 3;
	}
	/* 48 xx xx: 3 bytes for most ALU ops, TEST, CQO, etc. */
	if (b1 == 0x99) return 2;            /* CQO: 48 99 */
	return 3;
}

/* pass 0: find function prologues (push rbp; mov rbp,rsp).
 * Extracts frame size from optional sub rsp,imm32 and param count
 * from the consecutive mov [rbp+disp],reg stores. */
void scan_functions(void)
{
	long off, rel, i, fid, frame, npar, modrm;

	d_nfuncs = 0;
	ip = 1;
	rel = rd4();
	fns[0].off = ip + rel;
	d_nfuncs = 1;

	ip = 6;
	while (ip + 4 <= bin_len) {
		if (rb(ip) != 0x55 || rb(ip+1) != 0x48 || rb(ip+2) != 0x89 || rb(ip+3) != 0xe5) {
			ip++;
			continue;
		}
		off = ip;
		fid = -1;
		for (i = 0; i < d_nfuncs; i++) {
			if (fns[i].off == off)
				fid = i;
		}
		if (fid < 0) {
			fid = d_nfuncs;
			fns[d_nfuncs].off = off;
			d_nfuncs++;
		}
		ip += 4;
		frame = 0;
		if (ip + 7 <= bin_len && rb(ip) == 0x48 && rb(ip+1) == 0x81 && rb(ip+2) == 0xec) {
			ip += 3;
			frame = rd4();
		}
		fns[fid].frame = frame;
		npar = 0;
		while (ip + 3 <= bin_len &&
		       (rb(ip) == 0x48 || rb(ip) == 0x4c) &&
		       rb(ip+1) == 0x89 &&
		       (rb(ip+2) & 7) == 5 &&
		       ((rb(ip+2) & 0xc0) == 0x40 || (rb(ip+2) & 0xc0) == 0x80)) {
			modrm = rb(ip + 2);
			npar++;
			ip += 3;
			if ((modrm & 0xc0) == 0x40)
				ip++;
			else
				ip += 4;
		}
		fns[fid].npar = npar;
	}
}

/*
 * pass 1: scan_labels
 *
 * Walk binary looking for:
 *   E9 <rel32>       JMP32
 *   0F 84 <rel32>    JE32
 *   0F 85 <rel32>    JNE32
 *
 * Identify loop exits:
 *   - WHILE pattern:    ... E TEST JE32(fwd) <body> JMP32(back) [exit]
 *     The JE32 target = exit = break target.
 *     Detection: JMP32 backward → find the preceding JE32 whose target
 *     is right after this JMP32.
 *
 *   - DO_WHILE pattern: [top] <body> E TEST JNE32(back) [exit]
 *     Exit = address right after JNE32 instruction.
 *     Detection: JNE32 backward → exit = source + 6.
 *
 * We scan byte-by-byte for these patterns. This is a heuristic pre-scan;
 * only exact c1-generated sequences will match.
 */
void scan_labels(long start, long end)
{
	long off, len, rel, target, je_off, je_len, je_rel, i;
	long n_fwd, foff, ftgt;

	n_loop_exits = 0;
	n_dowhiles = 0;
	n_cont_targets = 0;
	n_fwd = 0;

	/* Single pass: collect loop boundaries + forward JMP candidates.
	 * Backward JNE32 → do_while, backward JMP32 → while candidate.
	 * Forward JMP32 → saved for deferred continue check (needs dws[]). */
	off = start;
	while (off < end) {
		len = insn_len(off);
		if (len == 0)
			break;

		/* JNE32 backward: do_while backedge */
		if (rb(off) == 0x0f && rb(off+1) == 0x85 && len == 6) {
			rel = r32(off + 2);
			target = off + 6 + rel;
			if (target < off && target >= start) {
				add_loop_exit(off + 6);
				dws[n_dowhiles].top = target;
				dws[n_dowhiles].test = off - 3;
				dws[n_dowhiles].exit = off + 6;
				n_dowhiles++;
			}
		}

		if (rb(off) == 0xe9 && len == 5) {
			rel = r32(off + 1);
			target = off + 5 + rel;

			/* JMP32 backward: while backedge candidate */
			if (target < off && target >= start) {
				je_off = target;
				while (je_off < off) {
					je_len = insn_len(je_off);
					if (je_len == 0) break;
					if (rb(je_off) == 0x0f && rb(je_off+1) == 0x84 && je_len == 6) {
						je_rel = r32(je_off + 2);
						if (je_off + 6 + je_rel == off + 5) {
							add_loop_exit(off + 5);
							break;
						}
						if (je_off + 6 + je_rel > off)
							break;
					}
					je_off += je_len;
				}
			}

			/* JMP32 forward: save for deferred continue check */
			if (target > off && n_fwd < 512) {
				fwd_jmps[n_fwd].off = off;
				fwd_jmps[n_fwd].tgt = target;
				n_fwd++;
			}
		}

		off += len;
	}

	/* Deferred: check forward JMP candidates against do_while boundaries.
	 * Iterates saved candidates, not the binary — no second instruction walk. */
	for (i = 0; i < n_fwd; i++) {
		foff = fwd_jmps[i].off;
		ftgt = fwd_jmps[i].tgt;
		for (off = 0; off < n_dowhiles; off++) {
			if (foff >= dws[off].top && foff < dws[off].test &&
			    ftgt > dws[off].top && ftgt <= dws[off].test) {
				/* verify straight-line from target to dw_test */
				je_off = ftgt;
				while (je_off < dws[off].test) {
					je_len = insn_len(je_off);
					if (je_len == 0)
						break;
					if (rb(je_off) == 0xe9 || rb(je_off) == 0xe8 ||
					    (rb(je_off) == 0x0f && (rb(je_off+1) == 0x84 || rb(je_off+1) == 0x85)))
						break;
					je_off += je_len;
				}
				if (je_off >= dws[off].test)
					add_cont_target(ftgt);
			}
		}
	}
}

long parse_full_expr(void);
long parse_stmt(long end);

/* detect POP_P sequence: 58 {48|49} 89 Cx (pop rax; mov reg, rax).
 * This is the call-arg pop, distinct from POP59 (binop operand). */
long is_pop_p(void)
{
	if (pk() == 0x58 && (pk1() == 0x48 || pk1() == 0x49) && pk2() == 0x89)
		return 1;
	return 0;
}

void skip_pop_p(void)
{
	ip += 4; /* 58 {48|49} 89 Cx */
}

/* parse post-expression suffixes: deref loads, unary ops,
 * variable stores, and short-circuit LOG_AND/OR.
 * Uses speculative parsing with backtrack for LOG_AND/OR. */
long apply_suffixes(long nd)
{
	long b0, b1, b2, d, vid, gid;
	long right, rel, target;
	long save_ip, save_nn;

	while (1) {
		b0 = pk(); b1 = pk1(); b2 = pk2();

		/* --- dereference loads (1/2/4/8 byte) --- */
		if (b0 == 0x48 && b1 == 0x0f && b2 == 0xb6 && pk3() == 0x00) {
			ip += 4; nd = em(N_DEREF, nd, 1, 0);
		} else if (b0 == 0x48 && b1 == 0x0f && b2 == 0xb7 && pk3() == 0x00) {
			ip += 4; nd = em(N_DEREF, nd, 2, 0);
		} else if (b0 == 0x48 && b1 == 0x63 && b2 == 0x00) {
			ip += 3; nd = em(N_DEREF, nd, 4, 0);
		} else if (b0 == 0x48 && b1 == 0x8b && b2 == 0x00) {
			ip += 3; nd = em(N_DEREF, nd, 8, 0);
		/* --- unary ops --- */
		} else if (b0 == 0x48 && b1 == 0xf7 && b2 == 0xd8) {
			ip += 3; nd = em(N_NEG, nd, 0, 0);
		} else if (b0 == 0x48 && b1 == 0xf7 && b2 == 0xd0) {
			ip += 3; nd = em(N_NOT, nd, 0, 0);
		} else if (b0 == 0x48 && b1 == 0x85 && b2 == 0xc0 &&
		           pk3() == 0x0f && pk4() == 0x94) {
			ip += 10; nd = em(N_LOG_NOT, nd, 0, 0);
		/* --- variable stores (global / local) --- */
		} else if (b0 == 0x48 && b1 == 0x89 && b2 == 0x05) {
			ip += 3; d = rd4(); gid = rip_to_gid(d, ip);
			nd = em(N_ASSIGN, em(N_VAR, 0, 0, gid), nd, 0);
			break;
		} else if (b0 == 0x48 && b1 == 0x89 && (b2 == 0x45 || b2 == 0x85)) {
			ip += 2; d = read_disp(); vid = disp_to_vid(d);
			nd = em(N_ASSIGN, em(N_VAR, 0, 0, vid), nd, 0);
			break;
		/* --- short-circuit logic (LOG_AND / LOG_OR) --- */
		} else if (b0 == 0x48 && b1 == 0x85 && b2 == 0xc0 &&
		           pk3() == 0x0f && pk4() == 0x84) {
			/* candidate LOG_AND: TEST JE32 → false.
			 * Verify: false_label → XOR_RAX.
			 * Must be forward jump (backward = while exit). */
			rel = r32(ip + 5);
			target = ip + 9 + rel;
			if (target > ip && target+3 <= bin_len && rb(target)==0x48 && rb(target+1)==0x31 && rb(target+2)==0xc0) {
				/* Parse right side, then verify epilogue:
				 * Discriminator: byte at ip+4 after right E == 0x95 (SETNE). */
				save_ip = ip;
				save_nn = nn;
				ip += 9;
				right = parse_full_expr();
				if (pk() == 0x48 && pk1() == 0x85 && pk2() == 0xc0 &&
				    pk3() == 0x0f && pk4() == 0x95) {
					/* confirmed LOG_AND epilogue:
					 * TEST + SETNE + MOVZX + JMP32 + XOR_RAX.
					 * Skip structurally via insn_len. */
					ip += insn_len(ip);  /* TEST */
					ip += insn_len(ip);  /* SETNE */
					ip += insn_len(ip);  /* MOVZX */
					ip += insn_len(ip);  /* JMP32 */
					ip += insn_len(ip);  /* XOR RAX,RAX */
					nd = em(N_LOG_AND, nd, right, 0);
				} else {
					/* not LOG_AND epilogue — backtrack */
					ip = save_ip;
					nn = save_nn;
					break;
				}
			} else {
				break;
			}
		} else if (b0 == 0x48 && b1 == 0x85 && b2 == 0xc0 &&
		           pk3() == 0x0f && pk4() == 0x85) {
			/* candidate LOG_OR: TEST JNE32 → true.
			 * Verify: true_label → NUM(1) (48 B8).
			 * Must be forward jump (backward = do_while backedge). */
			rel = r32(ip + 5);
			target = ip + 9 + rel;
			if (target > ip && target+2 <= bin_len && rb(target)==0x48 && rb(target+1)==0xb8) {
				/* Parse right side, then verify epilogue:
				 * after right E must be TEST JE32 NUM(1) JMP XOR_RAX.
				 * Discriminator: byte at ip+4 after right E == 0x84 (JE32). */
				save_ip = ip;
				save_nn = nn;
				ip += 9;
				right = parse_full_expr();
				if (pk() == 0x48 && pk1() == 0x85 && pk2() == 0xc0 &&
				    pk3() == 0x0f && pk4() == 0x84) {
					/* confirmed LOG_OR epilogue:
					 * TEST + JE32 + NUM(1) + JMP32 + XOR_RAX.
					 * Skip structurally via insn_len. */
					ip += insn_len(ip);  /* TEST */
					ip += insn_len(ip);  /* JE32 */
					ip += insn_len(ip);  /* NUM (movabs rax, 1) */
					ip += insn_len(ip);  /* JMP32 */
					ip += insn_len(ip);  /* XOR RAX,RAX */
					nd = em(N_LOG_OR, nd, right, 0);
				} else {
					/* not LOG_OR epilogue — backtrack */
					ip = save_ip;
					nn = save_nn;
					break;
				}
			} else {
				break;
			}
		} else {
			break;
		}
	}
	return nd;
}

/* parse primary expression: local/global load, LEA, NUM, zero-arg CALL.
 * Dispatches on first 3 bytes, then applies suffixes. */
long parse_expr(void)
{
	long nd, b0, b1, b2, d;
	long vid, gid, inner, rel, fid;

	b0 = pk(); b1 = pk1(); b2 = pk2();

	/* primaries — unambiguous by first 3 bytes */
	if (b0 == 0x48 && b1 == 0x8b && b2 == 0x05) {
		ip += 3; d = rd4(); gid = rip_to_gid(d, ip);
		nd = em(N_VAR, 0, 0, gid);
	} else if (b0 == 0x48 && b1 == 0x8d && b2 == 0x05) {
		ip += 3; d = rd4(); gid = rip_to_gid(d, ip);
		inner = em(N_VAR, 0, 0, gid);
		nd = em(N_ADDR, inner, 0, 0);
	} else if (b0 == 0x48 && b1 == 0x8b && (b2 == 0x45 || b2 == 0x85)) {
		ip += 2; d = read_disp(); vid = disp_to_vid(d);
		nd = em(N_VAR, 0, 0, vid);
	} else if (b0 == 0x48 && b1 == 0x8d && (b2 == 0x45 || b2 == 0x85)) {
		ip += 2; d = read_disp(); vid = disp_to_vid(d);
		inner = em(N_VAR, 0, 0, vid);
		nd = em(N_ADDR, inner, 0, 0);
	} else if (b0 == 0x48 && b1 == 0xb8) {
		ip += 2; nd = em(N_NUM, 0, 0, rd8());
	} else if (b0 == 0xe8) {
		/* zero-argument CALL32 */
		ip++;
		rel = rd4();
		fid = off_to_fid(ip + rel);
		nd = em(N_CALL, nn, 0, fid);
	} else {
#ifdef DEBUG
		trap(0x0001, ip);
		trap(0x0002, b0);
#endif
		return -1;
	}

	return apply_suffixes(nd);
}

/* match binary operator after POP59 (left in rcx, right in rax).
 * Recognizes ALU ops, comparisons, div/mod, and deref-store patterns. */
long match_binop(long left, long right)
{
	long b0, b1, b2, opc;

	b0 = pk(); b1 = pk1(); b2 = pk2();

	if (b0==0x48 && b1==0x01 && b2==0xc8) { ip+=3; return em(N_ADD, left, right, 0); }
	if (b0==0x48 && b1==0x29 && b2==0xc8) { ip+=3; return em(N_SUB, right, left, 0); }
	if (b0==0x48 && b1==0x0f && b2==0xaf) { ip+=4; return em(N_MUL, left, right, 0); }
	if (b0==0x48 && b1==0x21 && b2==0xc8) { ip+=3; return em(N_AND, left, right, 0); }
	if (b0==0x48 && b1==0x09 && b2==0xc8) { ip+=3; return em(N_OR, left, right, 0); }
	if (b0==0x48 && b1==0x31 && b2==0xc8) { ip+=3; return em(N_XOR, left, right, 0); }
	if (b0==0x48 && b1==0xd3 && b2==0xe0) { ip+=3; return em(N_SHL, right, left, 0); }
	if (b0==0x48 && b1==0xd3 && b2==0xf8) { ip+=3; return em(N_SHR_S, right, left, 0); }
	if (b0==0x48 && b1==0xd3 && b2==0xe8) { ip+=3; return em(N_SHR_U, right, left, 0); }

	/* CMP: 48 39 C8 0F <cc> C0 48 0F B6 C0 */
	if (b0==0x48 && b1==0x39 && b2==0xc8) {
		ip += 4; opc = rd1(); ip += 5;
		return em(setcc_kind(opc), right, left, 0);
	}

	/* DIV/MOD (POP59 already inside these combined tokens) */
	if (b0==0x48 && b1==0x99) {
		ip += 6; /* cqo + pop rcx + idiv rcx */
		if (pk()==0x48 && pk1()==0x89 && pk2()==0xd0) {
			ip += 3; return em(N_MOD_S, right, left, 0);
		}
		return em(N_DIV_S, right, left, 0);
	}
	if (b0==0x48 && b1==0x31 && b2==0xd2) {
		ip += 7; /* xor rdx + pop rcx + div rcx */
		if (pk()==0x48 && pk1()==0x89 && pk2()==0xd0) {
			ip += 3; return em(N_MOD_U, right, left, 0);
		}
		return em(N_DIV_U, right, left, 0);
	}

	/* deref store */
	if (b0==0x88 && b1==0x01) { ip+=2; return em(N_ASSIGN, em(N_DEREF, left, 1, 0), right, 0); }
	if (b0==0x66 && b1==0x89 && b2==0x01) { ip+=3; return em(N_ASSIGN, em(N_DEREF, left, 2, 0), right, 0); }
	if (b0==0x89 && b1==0x01) { ip+=2; return em(N_ASSIGN, em(N_DEREF, left, 4, 0), right, 0); }
	if (b0==0x48 && b1==0x89 && b2==0x01) { ip+=3; return em(N_ASSIGN, em(N_DEREF, left, 8, 0), right, 0); }

#ifdef DEBUG
	trap(0x0010, ip);
#endif
	return -1;
}

/* parse a call argument: a primary expr followed by optional
 * inner binops. Backtracking distinguishes arg-separator PUSHes
 * from binop PUSHes. */
long parse_call_arg(void)
{
	long nd, right, save, nn_save, result;

	nd = parse_expr();

	/* try inner binop: E PUSH E POP59 OP (or division with embedded POP) */
	while (pk() == 0x50) {
		save = ip;
		nn_save = nn;
		ip++; /* skip PUSH */
		right = parse_call_arg(); /* recurse */
		if (pk() == 0x59) {
			ip++; /* POP59 */
			result = match_binop(nd, right);
			if (result >= 0) {
				nd = apply_suffixes(result);
			} else {
				ip = save;
				nn = nn_save;
				return nd;
			}
		} else if ((pk() == 0x48 && pk1() == 0x99) ||
		           (pk() == 0x48 && pk1() == 0x31 && pk2() == 0xd2)) {
			result = match_binop(nd, right);
			if (result >= 0) {
				nd = apply_suffixes(result);
			} else {
				ip = save;
				nn = nn_save;
				return nd;
			}
		} else {
			/* no POP59, not division — PUSH is a CALL arg separator */
			ip = save;
			nn = nn_save;
			return nd;
		}
	}
	return nd;
}

/*
 * fold_through: after a binop match, fold remaining stacked args.
 * args[0..fold_argc-1] are the pending left operands.
 * Returns final result node; fold_argc = remaining arg count.
 */
long fold_argc; /* output: remaining arg count after fold_through */

long fold_through(long *args, long argc, long result)
{
	result = apply_suffixes(result);
	argc--;
	while (argc >= 1 && pk() == 0x59) {
		ip++;
		result = match_binop(args[argc - 1], result);
		if (result < 0)
			break;
		result = apply_suffixes(result);
		argc--;
	}
	fold_argc = argc;
	return result;
}

/* parse a full expression: primary + stacked PUSH chain.
 * Iteratively collects args[] and disambiguates between:
 *
 *   E PUSH E POP59(59) OP   → binary operation (left op right)
 *   E PUSH E PUSH ... POP_P(58+mov) E8  → function call
 *   E PUSH E CQO/XRDX POP59 IDIV/DIV    → division (embedded POP)
 *
 * State machine:
 *   1. parse primary+suffixes → nd
 *   2. while (PUSH): collect args[]
 *      a. at POP_P: build sub-call from last N args
 *      b. at POP59: binary op between args[top-1] and right
 *      c. at CQO/XRDX: division (embedded POP, same as POP59)
 *      d. at PUSH: another arg, keep collecting
 *      e. else: expression done */
long parse_full_expr(void)
{
	long nd, right, save, nn_save, result;
	long args[16];
	long argc, fid, rel, i;
	long npops, sub_start, sub_call;

	nd = parse_expr();

	/*
	 * After primary+suffixes, check for PUSH (binary op or call arg).
	 *
	 * Iterative approach:
	 *   E PUSH → parse right side with parse_call_arg (handles inner binops)
	 *   Then check what follows:
	 *     POP59 (59)  → binary op
	 *     CQO/XRDX    → division (embedded POP59)
	 *     PUSH (50)   → another call arg, keep collecting
	 *     POP_P (58 48/49 89) → end of args, build call
	 *     other       → expression done
	 */
	while (pk() == 0x50) {
		/* collect into args for potential CALL */
		args[0] = nd;
		argc = 1;
		save = ip;  /* progress check: must advance past this PUSH */
		ip++; /* skip PUSH */

		while (1) {
			/* if at POP_P → all args collected, build CALL */
			if (is_pop_p()) {
				npops = 0;
				while (is_pop_p()) {
					skip_pop_p();
					npops++;
				}
				ip++; /* E8 */
				rel = rd4();
				fid = off_to_fid(ip + rel);
				/* build sub-call from last npops args (reversed) */
				sub_start = nn;
				for (i = argc - 1; i >= argc - npops; i--)
					emit_copy(args[i]);
				sub_call = em(N_CALL, sub_start, npops, fid);
#ifdef DEBUG
				if (npops > argc)
					trap(0x0050, npops); /* args underflow */
#endif
				argc -= npops;
				if (pk() == 0x50) {
					/* result pushed → replace args, continue */
					args[argc] = sub_call;
					argc++;
					ip++;
				} else if (argc > 0 && pk() == 0x59) {
					/* sub_call is right operand of binop with args[argc-1] */
					ip++; /* skip POP59 */
					result = match_binop(args[argc - 1], sub_call);
					if (result >= 0) {
						result = fold_through(args, argc, result);
						argc = fold_argc;
						if (argc == 0) {
							nd = result;
							break;
						}
						if (pk() == 0x50) {
							args[argc] = result;
							argc++;
							ip++;
						} else {
							nd = result;
							break;
						}
					} else {
						nd = sub_call;
						break;
					}
				} else {
					/* final call, no remaining args */
					nd = sub_call;
					nd = apply_suffixes(nd);
					break;
				}
			} else {
				/* parse next expression (arg or binop right side) */
				save = ip;
				nn_save = nn;
				right = parse_call_arg();
				right = apply_suffixes(right);

				/* check what follows: POP59 or division (CQO/XRDX)
				 * both lead to match_binop; POP59 consumes 0x59 first,
				 * division's POP is embedded in match_binop. */
				if (pk() == 0x59 ||
				    (pk() == 0x48 && pk1() == 0x99) ||
				    (pk() == 0x48 && pk1() == 0x31 && pk2() == 0xd2)) {
					if (pk() == 0x59)
						ip++;  /* consume POP59; division skips this */
					result = match_binop(args[argc - 1], right);
					if (result >= 0) {
						result = fold_through(args, argc, result);
						argc = fold_argc;
						if (argc == 0) {
							nd = result;
							break;
						}
						if (pk() == 0x50) {
							args[argc] = result;
							argc++;
							ip++;
						} else {
							nd = result;
							break;
						}
					} else {
						ip = save; nn = nn_save;
						break;
					}
				} else if (pk() == 0x50) {
					/* PUSH → right is another call arg */
					args[argc] = right;
					argc++;
					ip++;
				} else if (is_pop_p()) {
					/* POP_P → right is last arg before CALL */
					args[argc] = right;
					argc++;
					/* loop back to top of while(1) to handle POP_P+CALL */
				} else {
					/* nothing recognizable after right —
					 * right was a binop right side without POP59?
					 * Shouldn't happen. Backtrack. */
					ip = save; nn = nn_save;
					break;
				}
			}
		}
#ifdef DEBUG
		if (ip == save) {
			trap(0x0020, ip); /* parse_full_expr: no progress after PUSH */
		}
#endif
	}

	return nd;
}

/*
 * try_wrap_dowhile: check if ip holds TEST JNE32(backward) and, if so,
 * retroactively wrap stmts[sbase..sbase+*ns-1] into a DO_WHILE node.
 * Returns 1 if a DO_WHILE was produced, 0 otherwise.
 */
long try_wrap_dowhile(long sbase, long *ns, long end_addr)
{
	long rel, do_target, do_cond, do_body;
	long i, j, start, cnt;

	if (ip + 9 > end_addr)
		return 0;
	if (pk() != 0x48 || pk1() != 0x85 || pk2() != 0xc0 ||
	    pk3() != 0x0f || pk4() != 0x85)
		return 0;
	rel = r32(ip + 5);
	do_target = ip + 9 + rel;
	if (do_target >= ip)
		return 0;

	/* condition is the last parsed stmt */
	do_cond = g_stmts[g_stmts_top - 1].nd;
	ip += 9; /* skip TEST + JNE32 */

	/* find first stmt whose ip >= do_target */
	j = *ns - 1;
	while (j > 0 && g_stmts[sbase + j - 1].ip >= do_target)
		j--;

	/* wrap stmts [j..ns-1) into do_while body (exclude condition) */
	start = nn;
	cnt = 0;
	for (i = j; i < *ns - 1; i++) {
		emit_copy(g_stmts[sbase + i].nd);
		cnt++;
	}
	do_body = em(N_BLOCK, start, cnt, 0);

	/* replace stmts [j..*ns) with single DO_WHILE */
	g_stmts_top = sbase + j + 1;
	g_stmts[sbase + j].nd = em(N_DO_WHILE, do_body, do_cond, 0);
	*ns = j + 1;
	return 1;
}

/*
 * parse_block: parse statements from ip to end_addr.
 * Handles DO_WHILE retroactively (TEST JNE32 backward after a stmt).
 * Returns a N_BLOCK node index.
 */
long parse_block(long end_addr)
{
	long sbase, ns, start, i;
	long saved_ip;

	sbase = g_stmts_top;
	/* g_stmts[k] = { .ip = stmt start, .nd = AST node index } */
	ns = 0;
	while (ip < end_addr && !(pk() == 0xc9 && pk1() == 0xc3)) {
		start = ip; /* progress check */
		saved_ip = ip;
#ifdef DEBUG
		if (g_stmts_top >= 8192) trap(0x0071, g_stmts_top);
#endif
		g_stmts[g_stmts_top].nd = parse_stmt(end_addr);
		g_stmts[g_stmts_top].ip = saved_ip;
		g_stmts_top++;
		ns++;
		if (ip == start) {
#ifdef DEBUG
			trap(0x0030, ip); /* parse_block: no progress */
#endif
			break;
		}

		/* DO_WHILE: TEST JNE32(backward) after last stmt */
		if (ip + 9 <= end_addr && pk() == 0x48)
			try_wrap_dowhile(sbase, &ns, end_addr);
	}

	/* build N_BLOCK from collected stmts */
	start = nn;
	for (i = 0; i < ns; i++)
		emit_copy(g_stmts[sbase + i].nd);
	g_stmts_top = sbase;
	return em(N_BLOCK, start, ns, 0);
}

/* append a trailing break/continue node to g_stmts, then emit IF(cond, block). */
long emit_if_trailing(long cond, long sbase, long ns, long trail_kind)
{
	long then_nd;
#ifdef DEBUG
	if (g_stmts_top >= 8192) trap(0x0073, g_stmts_top);
#endif
	g_stmts[g_stmts_top].ip -= 5;
	g_stmts[g_stmts_top].nd = em(trail_kind, 0, 0, 0);
	g_stmts_top++;
	ns++;
	then_nd = emit_stmts_block(sbase, ns);
	g_stmts_top = sbase;
	return em(N_IF, cond, then_nd, 0);
}

/* parse one statement: return, break/continue, expression, if/while */
long parse_stmt(long end)
{
	long nd, cond, then_nd, else_nd;
	long rel, je_target, jmp_target;
	long sbase, ns, saved;
	long saved_ip2;

	/* LEAVE RET = void return */
	if (pk() == 0xc9 && pk1() == 0xc3) {
		ip += 2;
		return em(N_RETURN, 0, 0, 0);
	}
	/* JMP32 = break / continue.
	 * backward → while continue (backedge handled by parse_block).
	 * forward to do_while TEST → do_while continue.
	 * forward to loop exit → break.
	 * c1 compiles continue as JMP to TEST (== dws[i].test). */
	if (pk() == 0xe9) {
		saved = ip;
		ip++;
		rel = rd4();
		if (ip + rel < saved)
			return em(N_CONTINUE, 0, 0, 0);
		if (is_dw_continue(ip + rel, saved))
			return em(N_CONTINUE, 0, 0, 0);
		return em(N_BREAK, 0, 0, 0);
	}

	nd = parse_full_expr();

	/* E followed by LEAVE RET: return or expr-stmt + void return?
	 * ASSIGN/CALL before LEAVE RET → expr-stmt + void return
	 * (canonical form, avoids ambiguity)
	 * anything else → return E */
	if (pk() == 0xc9 && pk1() == 0xc3) {
		if (ast[nd].kind != N_ASSIGN && ast[nd].kind != N_CALL) {
			ip += 2;
			return em(N_RETURN, nd, 0, 0);
		}
		/* fall through: return nd as expression-statement */
	}

	/* IF / WHILE: TEST JE32 (at stmt level, never LOG_AND) */
	if (pk() == 0x48 && pk1() == 0x85 && pk2() == 0xc0 &&
	    pk3() == 0x0f && pk4() == 0x84) {
		cond = nd;
		ip += 5; /* skip TEST + 0F 84 */
		rel = rd4();
		je_target = ip + rel;

		/* Parse body stmts with inline DO_WHILE detection.
		 * Stop before trailing JMP32 (structural: backedge/else-sep). */
		sbase = g_stmts_top;
		ns = 0;
		while (ip < je_target) {
			if (ip + 5 == je_target && pk() == 0xe9)
				break;
			saved = ip; /* progress check */
			saved_ip2 = ip;
#ifdef DEBUG
			if (g_stmts_top >= 8192) trap(0x0072, g_stmts_top);
#endif
			g_stmts[g_stmts_top].nd = parse_stmt(je_target);
			g_stmts[g_stmts_top].ip = saved_ip2;
			g_stmts_top++;
			ns++;
			if (ip == saved) {
#ifdef DEBUG
				trap(0x0031, ip); /* parse_stmt body: no progress */
#endif
				break;
			}

			/* DO_WHILE inside body: TEST JNE32(backward) */
			try_wrap_dowhile(sbase, &ns, je_target);
		}

		/* Classify trailing JMP32 */
		if (ip + 5 == je_target && pk() == 0xe9) {
			ip++;
			rel = rd4();
			jmp_target = ip + rel;

			if (jmp_target < ip - 5) {
				if (is_loop_exit(je_target)) {
					/* WHILE backedge */
					then_nd = emit_stmts_block(sbase, ns);
					g_stmts_top = sbase;
					return em(N_WHILE, cond, then_nd, 0);
				}
				return emit_if_trailing(cond, sbase, ns, N_CONTINUE);
			}

#ifdef HOST_BUILD
				fprintf(stderr, "  trailing JMP at 0x%lx -> 0x%lx (fwd), is_loop_exit=%ld, is_dw_continue=%ld\n",
					ip - 5, jmp_target, is_loop_exit(jmp_target), is_dw_continue(jmp_target, ip - 5));
#endif
				if (is_enclosing_loop_exit(jmp_target, ip - 5) && !is_dw_continue(jmp_target, ip - 5)) {
					return emit_if_trailing(cond, sbase, ns, N_BREAK);
				}

				if (is_dw_continue(jmp_target, ip - 5)) {
					return emit_if_trailing(cond, sbase, ns, N_CONTINUE);
				}

			/* else separator */
			then_nd = emit_stmts_block(sbase, ns);
			else_nd = parse_block(jmp_target);
			g_stmts_top = sbase;
			return em(N_IF, cond, then_nd, else_nd);
		}

		/* no trailing JMP */
		then_nd = emit_stmts_block(sbase, ns);
		g_stmts_top = sbase;
		return em(N_IF, cond, then_nd, 0);
	}

	return nd; /* expression statement */
}

long vid_map[512];

/* decompile one function: skip prologue, parse param saves,
 * decompile body via parse_block, then renumber local variable IDs
 * to sequential order (params 0..npar-1, locals npar..). */
long dec_function(long fid)
{
	long npar, start, i, j, r;
	long body, stmts_nd, disp, ndecl;
	long prev_disp, slots;
	long old_vid, new_vid, k, body_start;

	ip = fns[fid].off;
	npar = fns[fid].npar;
	nlocals = 0;
	for (i = 0; i < 512; i++)
		local_seen[i] = 0;

	/* pre-scan labels, then sort for binary search */
	scan_labels(fns[fid].off, fns[fid].end);
	sort_long_array(loop_exit, n_loop_exits);
	sort_long_array(cont_target_addr, n_cont_targets);
#ifdef HOST_BUILD
	{
		long di;
		for (di = 0; di < n_dowhiles; di++)
			fprintf(stderr, "  dw[%ld]: top=0x%lx test=0x%lx exit=0x%lx\n",
				di, dws[di].top, dws[di].test, dws[di].exit);
		for (di = 0; di < n_loop_exits; di++)
			fprintf(stderr, "  loop_exit[%ld]=0x%lx\n", di, loop_exit[di]);
	}
#endif

	/* skip prologue */
	ip += 4;
	if (fns[fid].frame > 0)
		ip += 7;

	/* parse param saves */
	for (i = 0; i < npar; i++) {
		r = pk();
		if (r == 0x48 || r == 0x4c) {
			ip += 2;
			disp = read_disp();
			disp_to_vid(disp);
		}
	}

	/* decompile body using parse_block (handles DO_WHILE at all levels) */
	body_start = nn;
	stmts_nd = parse_block(fns[fid].end);
	if (ip < fns[fid].end && pk() == 0xc9 && pk1() == 0xc3)
		ip += 2;

	/* sort locals by vid */
	for (i = npar + 1; i < nlocals; i++) {
		for (j = i; j > npar && locals[j - 1].vid > locals[j].vid; j--) {
			r = locals[j].vid; locals[j].vid = locals[j-1].vid; locals[j-1].vid = r;
			r = locals[j].disp; locals[j].disp = locals[j-1].disp; locals[j-1].disp = r;
		}
	}

	/* renumber locals to compact sequential IDs:
	 * params keep 0..npar-1, locals become npar, npar+1, ... */
	for (i = 0; i < 512; i++)
		vid_map[i] = i;
	for (i = npar; i < nlocals; i++) {
		old_vid = locals[i].vid;
		new_vid = i;
		if (old_vid != new_vid && old_vid < 512)
			vid_map[old_vid] = new_vid;
		locals[i].vid = new_vid;
	}
	/* patch all AST nodes in this function's body */
	for (k = body_start; k < nn; k++) {
		if ((ast[k].kind == N_VAR || ast[k].kind == N_DECL) &&
		    ast[k].val >= 0 && ast[k].val < GLOBAL_ID_BASE &&
		    ast[k].val < 512)
			ast[k].val = vid_map[ast[k].val];
	}

	/* build BLOCK: DECLs + stmts from parse_block */
	start = nn;
	ndecl = 0;
	for (i = 0; i < npar; i++) {
		em(N_DECL, 1, 0, i);
		ndecl++;
	}
	for (i = npar; i < nlocals; i++) {
		if (i > 0)
			prev_disp = locals[i - 1].disp;
		else
			prev_disp = 0;
		slots = ((-locals[i].disp) - (-prev_disp)) / 8;
#ifdef DEBUG
		if (slots < 1) trap(0x1500, slots);
#endif
		em(N_DECL, slots, 0, locals[i].vid);
		ndecl++;
	}
	/* copy stmts from the block node */
	for (i = 0; i < ast[stmts_nd].b; i++)
		emit_copy(ast[stmts_nd].a + i);
	body = em(N_BLOCK, start, ndecl + ast[stmts_nd].b, 0);
	return em(N_FUNC, body, npar, fid);
}

/* decompile x86-64 binary to serialised AST.
 * Three passes: scan_functions, scan_labels (per function),
 * then recursive descent. Returns AST byte count. */
long decompile(long in, long in_len, long out_buf)
{
	long i, j;
	long funcs[256];
	long top_start, top_count, min_off, k;

	bin = (unsigned char *)in;
	bin_len = in_len;
	ast = (struct node *)out_buf;
	nn = 0;
	code_base = CODE_BASE;
#ifdef C1M_SHARED
	bss_base = BSS_BASE;
#endif

	next_gid = GLOBAL_ID_BASE;
	nglobals = 0;
	g_stmts_top = 0;

	/* init gid_map to identity */
	for (i = 0; i < 4096; i++)
		gid_map[i] = GLOBAL_ID_BASE + i;

	em(0, 0, 0, 0); /* reserve node 0 */

	scan_functions();

	/* compute end offsets */
	for (i = 0; i < d_nfuncs; i++) {
		fns[i].end = bin_len;
		for (j = 0; j < d_nfuncs; j++) {
			if (fns[j].off > fns[i].off && fns[j].off < fns[i].end)
				fns[i].end = fns[j].off;
		}
	}

	/* decompile functions */
	for (i = 0; i < d_nfuncs; i++)
		funcs[i] = dec_function(i);

	/* sort by offset for output */
	for (i = 1; i < d_nfuncs; i++) {
		for (j = i; j > 0 && fns[j - 1].off > fns[j].off; j--) {
			min_off = fns[j].off; fns[j].off = fns[j-1].off; fns[j-1].off = min_off;
			min_off = funcs[j]; funcs[j] = funcs[j-1]; funcs[j-1] = min_off;
		}
	}

	/* top-level BLOCK */
	top_start = nn;
	top_count = 0;

	/* sort globals by BSS address (insertion sort) */
	for (i = 1; i < nglobals; i++) {
		for (j = i; j > 0 && globals[j - 1].addr > globals[j].addr; j--) {
			min_off = globals[j].addr; globals[j].addr = globals[j-1].addr; globals[j-1].addr = min_off;
			min_off = globals[j].id; globals[j].id = globals[j-1].id; globals[j-1].id = min_off;
		}
	}

	/* renumber global IDs by BSS address order.
	 * Build permutation table, then single-pass patch. */
	for (i = 0; i < nglobals; i++) {
		gid_map[globals[i].id - GLOBAL_ID_BASE] = GLOBAL_ID_BASE + i;
		globals[i].id = GLOBAL_ID_BASE + i;
	}
	for (k = 0; k < nn; k++) {
		if ((ast[k].kind == N_VAR || ast[k].kind == N_DECL) &&
		    ast[k].val >= GLOBAL_ID_BASE &&
		    ast[k].val < GLOBAL_ID_BASE + 4096)
			ast[k].val = gid_map[ast[k].val - GLOBAL_ID_BASE];
	}

	/* emit global variable declarations with array sizes from BSS gaps */
	for (i = 0; i < nglobals; i++) {
		if (i + 1 < nglobals)
			min_off = (globals[i + 1].addr - globals[i].addr) / 8;
		else
			min_off = 1;
		if (min_off < 1) min_off = 1;
		em(N_DECL, min_off, 0, globals[i].id);
		top_count++;
	}
	/* _bss_end is already in globals[] (discovered via _bss_end = 0 store).
	 * No sentinel needed: it's the last global by BSS address. */

	for (i = 0; i < d_nfuncs; i++) {
		emit_copy(funcs[i]);
		top_count++;
	}

	ast[0].kind = N_BLOCK;
	ast[0].a = top_start;
	ast[0].b = top_count;
	ast[0].val = 0;

	return nn * 32;
}
