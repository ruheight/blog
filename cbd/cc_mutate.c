/*
 * cc_mutate.c — AST mutation engine
 *
 * Included by c1m.c and c2m.c. ast (struct node *) defined by caller.
 * Each mutation enabled by #ifdef MUT_xxx.
 * Entry point: mutate(ast_buf, ast_len, seed) -> new ast_len.
 */

#include "ast.h"

#if defined(MUT_LOCAL) || defined(MUT_GLOBAL) || defined(MUT_COMMUTE) || defined(MUT_REASSOC) || defined(MUT_IF_INVERT) || defined(MUT_STORAGE)
#define MUTATE
#endif

/* rng_state, srng(), rng() defined in cc_lib.c */

#if defined(MUT_LOCAL) || defined(MUT_GLOBAL)

/* swap two AST nodes by index (field-by-field copy) */
void node_swap(long a, long b)
{
	struct node tmp;
	tmp.kind = ast[a].kind;
	tmp.a = ast[a].a;
	tmp.b = ast[a].b;
	tmp.val = ast[a].val;
	ast[a].kind = ast[b].kind;
	ast[a].a = ast[b].a;
	ast[a].b = ast[b].b;
	ast[a].val = ast[b].val;
	ast[b].kind = tmp.kind;
	ast[b].a = tmp.a;
	ast[b].b = tmp.b;
	ast[b].val = tmp.val;
}

/* Fisher-Yates shuffle on ast[base+from .. base+to-1] */
void shuffle(long base, long from, long to)
{
	long i, j;
	for (i = to - 1; i > from; i--) {
		j = from + (rng() & 0x7fffffff) % (i - from + 1);
		if (i != j)
			node_swap(base + i, base + j);
	}
}

#endif

#ifdef MUT_LOCAL
/* shuffle non-parameter local DECLs within each function body.
 * Parameters must stay at the front (ABI slot assignment depends on order). */
void mutate_local(void)
{
	long top_start, top_count, i, child;
	long body, bstart, bcount, npar, ndecls;

	top_start = ast[0].a;
	top_count = ast[0].b;
	for (i = 0; i < top_count; i++) {
		child = top_start + i;
		if (ast[child].kind == N_FUNC) {
			npar = ast[child].b;
			body = ast[child].a;
			bstart = ast[body].a;
			bcount = ast[body].b;
			ndecls = 0;
			while (ndecls < bcount && ast[bstart + ndecls].kind == N_DECL)
				ndecls++;
			if (ndecls - npar >= 2)
				shuffle(bstart, npar, ndecls);
		}
	}
}
#endif

#ifdef MUT_GLOBAL
/* shuffle top-level global DECLs and FUNCs independently.
 * Last DECL (_bss_end) is excluded — it must remain last. */
void mutate_global(void)
{
	long top_start, top_count, ngdecl, first_fn, nfn;
#ifdef DEBUG
	long j;
#endif

	top_start = ast[0].a;
	top_count = ast[0].b;

	ngdecl = 0;
	while (ngdecl < top_count && ast[top_start + ngdecl].kind == N_DECL)
		ngdecl++;
	first_fn = ngdecl;
	nfn = top_count - first_fn;

#ifdef DEBUG
	for (j = 0; j < ngdecl; j++) {
		if (ast[top_start + j].kind != N_DECL)
			trap(0x3000, j);
	}
	for (j = first_fn; j < first_fn + nfn; j++) {
		if (ast[top_start + j].kind != N_FUNC)
			trap(0x3001, j);
	}
#endif

	if (ngdecl >= 3)
		shuffle(top_start, 0, ngdecl - 1);
	if (nfn >= 2)
		shuffle(top_start, first_fn, first_fn + nfn);
}
#endif

#ifdef MUT_COMMUTE
/* operators whose operands can be swapped without changing semantics */
long is_commutative(long k)
{
	return k == N_ADD || k == N_MUL ||
	       k == N_AND || k == N_OR || k == N_XOR ||
	       k == N_EQ || k == N_NE;
}
#endif

#ifdef MUT_REASSOC
/* operators eligible for tree rotation (associative) */
long is_assoc(long k)
{
	return k == N_ADD || k == N_MUL ||
	       k == N_AND || k == N_OR || k == N_XOR;
}

/* random tree rotation on associative operators: (op (op a b) c) <-> (op a (op b c)).
 * Coin-flip per candidate so repeated passes explore different shapes. */
void reassoc(long idx)
{
	long k, child, t;
	k = ast[idx].kind;
	if (!is_assoc(k))
		return;
	/* left-heavy: (op (op la lb) R) -> (op la (op lb R)) */
	if (ast[ast[idx].a].kind == k && ast[ast[idx].b].kind != k) {
		if (rng() & 1) {
			child = ast[idx].a;
			ast[idx].a = ast[child].a;
			t = ast[child].b;
			ast[child].a = t;
			ast[child].b = ast[idx].b;
			ast[idx].b = child;
		}
		return;
	}
	/* right-heavy: (op L (op la lb)) -> (op (op L la) lb) */
	if (ast[ast[idx].b].kind == k && ast[ast[idx].a].kind != k) {
		if (rng() & 1) {
			child = ast[idx].b;
			ast[idx].b = ast[child].b;
			t = ast[child].a;
			ast[child].a = ast[idx].a;
			ast[child].b = t;
			ast[idx].a = child;
		}
	}
}
#endif

#ifdef MUT_IF_INVERT
/* next free AST node index — new LOG_NOT nodes are appended here */
long nn_mut;
#endif

#if defined(MUT_COMMUTE) || defined(MUT_REASSOC) || defined(MUT_IF_INVERT)

/* bottom-up AST walk: recurse children, then apply mutations */
void walk_expr(long idx)
{
	long k, i;
#ifdef MUT_COMMUTE
	long t;
#endif
#ifdef MUT_IF_INVERT
	long tmp_then;
#endif
	k = ast[idx].kind;
	if (k == N_NUM || k == N_VAR || k == N_BREAK || k == N_CONTINUE || k == N_DECL)
		return;

	if (k == N_BLOCK || k == N_CALL) {
		for (i = 0; i < ast[idx].b; i++)
			walk_expr(ast[idx].a + i);
		return;
	}
	if (k == N_FUNC) {
		walk_expr(ast[idx].a);
		return;
	}
	if (k == N_IF) {
		walk_expr(ast[idx].a);
		walk_expr(ast[idx].b);
		if (ast[idx].val)
			walk_expr(ast[idx].val);
#ifdef MUT_IF_INVERT
		/* flip if/else branches: deterministically remove existing LOG_NOT,
		 * or randomly insert one (coin flip). Either way swap then/else. */
		if (ast[idx].val) {
			if (ast[ast[idx].a].kind == N_LOG_NOT) {
				/* if (!c) A else B -> if (c) B else A */
				ast[idx].a = ast[ast[idx].a].a;
				tmp_then = ast[idx].b;
				ast[idx].b = ast[idx].val;
				ast[idx].val = tmp_then;
			} else if ((rng() & 1) == 0) {
				/* if (c) A else B -> if (!c) B else A */
				ast[nn_mut].kind = N_LOG_NOT;
				ast[nn_mut].a = ast[idx].a;
				ast[nn_mut].b = 0;
				ast[nn_mut].val = 0;
				ast[idx].a = nn_mut;
				nn_mut++;
				tmp_then = ast[idx].b;
				ast[idx].b = ast[idx].val;
				ast[idx].val = tmp_then;
			}
		}
#endif
		return;
	}
	if (k == N_RETURN) {
		if (ast[idx].a) walk_expr(ast[idx].a);
		return;
	}
	if (k == N_DEREF || k == N_ADDR || k == N_NEG || k == N_NOT || k == N_LOG_NOT) {
		if (ast[idx].a) walk_expr(ast[idx].a);
		return;
	}

	/* binary: WHILE, DO_WHILE, ASSIGN, all binops, LOG_AND/OR */
	if (ast[idx].a) walk_expr(ast[idx].a);
	if (ast[idx].b) walk_expr(ast[idx].b);

#ifdef MUT_COMMUTE
	if (is_commutative(k) && (rng() & 1)) {
		t = ast[idx].a;
		ast[idx].a = ast[idx].b;
		ast[idx].b = t;
	}
#endif
#ifdef MUT_REASSOC
	reassoc(idx);
#endif
}

#endif


#ifdef MUT_STORAGE

/*
 * MUT_STORAGE — demote globals to locals, promote locals to globals.
 *
 * Demote: scalar global used in exactly one non-cyclic function → local.
 * Promote: random non-param scalar local in non-cyclic function → global.
 * Uses Floyd-Warshall on call graph to detect cycles (mutual recursion).
 *
 * CAVEAT: pseudo-static globals.
 * A "pseudo-static" is a scalar global used in only one function, where
 * that function reads the variable before writing it (relying on the
 * zero-initialized .bss value across calls, like a C static local).
 * If demoted to a local, the zero-init semantics are lost — the local
 * gets stack garbage instead, corrupting program state silently.
 * c0 now detects and rejects pseudo-statics at compile time (error on
 * scalar globals with a single owning function that are not arrays or
 * structs and not _bss_end).  If c0 detection is ever bypassed or
 * weakened, demote MUST add a write-before-read check to avoid this.
 *
 * Arrays (calls/reach): 256 funcs × 4 longs = bitset per function.
 * Bit k in calls[fid] set iff function fid calls function k.
 * reach[][] = transitive closure. Cyclic iff reach[fid] has bit fid.
 */

/* bitset helpers: 256 bits = 4 longs */
#define BS_LONGS 4
#define BS_SET(bs, bit) ((bs)[(bit) >> 6] |= (1L << ((bit) & 63)))
#define BS_TEST(bs, bit) (((bs)[(bit) >> 6] >> ((bit) & 63)) & 1)

long st_calls[1024];   /* call graph: 256 funcs * 4 longs bitset */
long st_reach[1024];   /* transitive closure: same layout */
long st_gvar_owner[256];         /* gid -> owning fid (-1=none, -2=multi) */

/* demote/promote action lists */
/* describes one demote (global->local) or promote (local->global) action.
 * int fields keep this struct 24 bytes; _pad aligns to 4-int boundary. */
struct st_action {
	int gid;       /* global id (demote: old, promote: new) */
	int lid;       /* local id (demote: new, promote: old) */
	int fid;       /* owning function */
	int slots;     /* 1 for scalar */
	int width;     /* element width (1/2/4/8, from DECL.b) */
	int _pad;
};
struct st_action st_demote[64];
struct st_action st_promote[64];
/* st_ndemote, st_npromote moved to mutate_storage() locals */

/* copy AST node fields — c0 has no struct assignment, so copy field-by-field */
void node_copy(long dst, long src)
{
	ast[dst].kind = ast[src].kind;
	ast[dst].a = ast[src].a;
	ast[dst].b = ast[src].b;
	ast[dst].val = ast[src].val;
}

/* max local id tracker per function (for multiple demotes) */
long st_func_max_local[256];

/* recursively scan AST subtree: build call-graph edges (BS_SET)
 * and track which function each global variable appears in. */
void st_scan_node(long ni, long fid)
{
	long k, i, gidx;
	k = ast[ni].kind;
	if (k == N_VAR) {
		if (ast[ni].val >= GLOBAL_ID_BASE) {
			gidx = ast[ni].val - GLOBAL_ID_BASE;
			if (gidx >= 0 && gidx < 256) {
				if (st_gvar_owner[gidx] == -1)
					st_gvar_owner[gidx] = fid;
				else if (st_gvar_owner[gidx] != fid)
					st_gvar_owner[gidx] = -2;
			}
		}
		return;
	}
	if (k == N_CALL) {
		/* record call edge */
		if (ast[ni].val >= 0 && ast[ni].val < 256)
			BS_SET(&st_calls[fid * BS_LONGS], ast[ni].val);
		/* scan arguments */
		for (i = 0; i < ast[ni].b; i++)
			st_scan_node(ast[ni].a + i, fid);
		return;
	}
	if (k == N_NUM || k == N_BREAK || k == N_CONTINUE || k == N_DECL)
		return;
	if (k == N_BLOCK) {
		for (i = 0; i < ast[ni].b; i++)
			st_scan_node(ast[ni].a + i, fid);
		return;
	}
	if (k == N_FUNC) {
		st_scan_node(ast[ni].a, fid);
		return;
	}
	/* unary */
	if (k == N_RETURN || k == N_DEREF || k == N_ADDR ||
	    k == N_NEG || k == N_NOT || k == N_LOG_NOT) {
		if (ast[ni].a) st_scan_node(ast[ni].a, fid);
		return;
	}
	/* binary / if */
	if (ast[ni].a) st_scan_node(ast[ni].a, fid);
	if (ast[ni].b) st_scan_node(ast[ni].b, fid);
	if (k == N_IF && ast[ni].val)
		st_scan_node(ast[ni].val, fid);
}

/* patch all N_VAR and N_DECL refs: old_id -> new_id in subtree */
void st_patch_refs(long ni, long old_id, long new_id)
{
	long k, i;
	k = ast[ni].kind;
	if (k == N_VAR || k == N_DECL) {
		if (ast[ni].val == old_id)
			ast[ni].val = new_id;
		return;
	}
	if (k == N_NUM || k == N_BREAK || k == N_CONTINUE)
		return;
	if (k == N_BLOCK || k == N_CALL) {
		for (i = 0; i < ast[ni].b; i++)
			st_patch_refs(ast[ni].a + i, old_id, new_id);
		return;
	}
	if (k == N_FUNC) {
		st_patch_refs(ast[ni].a, old_id, new_id);
		return;
	}
	if (k == N_RETURN || k == N_DEREF || k == N_ADDR ||
	    k == N_NEG || k == N_NOT || k == N_LOG_NOT) {
		if (ast[ni].a) st_patch_refs(ast[ni].a, old_id, new_id);
		return;
	}
	if (ast[ni].a) st_patch_refs(ast[ni].a, old_id, new_id);
	if (ast[ni].b) st_patch_refs(ast[ni].b, old_id, new_id);
	if (k == N_IF && ast[ni].val)
		st_patch_refs(ast[ni].val, old_id, new_id);
}

void mutate_storage(long *p_ast_len)
{
	long top_start, top_count, ngdecl, nfunc;
	long i, j, k, fid, gidx;
	long bss_end_gidx;
	long max_gid;
	long body, bstart, bcount, npar;
	long nn, new_nn;
	long func_ni;
	long has_demote, has_promote, old_body;
	long new_bstart, new_bcount;
	long old_bstart, old_bcount, old_npar, d;
	long new_top_start, new_top_count;
	long demoted;
	long max_local, new_lid;
	long st_ndemote, st_npromote;

	top_start = ast[0].a;
	top_count = ast[0].b;
	nn = *p_ast_len / 32;

	/* count globals, find bss_end */
	ngdecl = 0;
	while (ngdecl < top_count && ast[top_start + ngdecl].kind == N_DECL)
		ngdecl++;
	nfunc = top_count - ngdecl;
	if (ngdecl < 1 || nfunc < 1)
		return;

	/* bss_end = last global DECL, never touch it */
	bss_end_gidx = ast[top_start + ngdecl - 1].val - GLOBAL_ID_BASE;

	/* init arrays */
	for (i = 0; i < 256 * BS_LONGS; i++) {
		st_calls[i] = 0;
		st_reach[i] = 0;
	}
	for (i = 0; i < 256; i++)
		st_gvar_owner[i] = -1;

	/* scan all functions: build call graph + gvar ownership */
	i = 0;
	while (i < nfunc) {
		func_ni = top_start + ngdecl + i;
		fid = ast[func_ni].val;
		if (fid >= 0 && fid < 256)
			st_scan_node(ast[func_ni].a, fid);
		i++;
	}

	/* Floyd-Warshall transitive closure on call-graph bitset.
	 * After this, BS_TEST(reach[fid], fid) detects cycles (direct
	 * or mutual recursion), which disqualifies a function from
	 * storage mutation (demote/promote could break re-entrant state). */
	for (i = 0; i < 1024; i++)
		st_reach[i] = st_calls[i];
	for (k = 0; k < 256; k++) {
		for (i = 0; i < 256; i++) {
			if (BS_TEST(&st_reach[i * BS_LONGS], k)) {
				for (j = 0; j < BS_LONGS; j++)
					st_reach[i * BS_LONGS + j] |= st_reach[k * BS_LONGS + j];
			}
		}
	}

	/* find max global id */
	max_gid = 0;
	for (i = 0; i < ngdecl; i++) {
		gidx = ast[top_start + i].val - GLOBAL_ID_BASE;
		if (gidx > max_gid) max_gid = gidx;
	}

	/* === DEMOTE: global -> local ===
	 * Demote all scalar globals used in exactly one non-cyclic function.
	 * No write-before-read check needed: c0 rejects pseudo-statics
	 * (see caveat above).  If c0 detection is removed, this MUST be
	 * revised to check write-before-read for each candidate.
	 * Track max_local per function to assign unique ids for multiple demotes. */
	for (i = 0; i < 256; i++)
		st_func_max_local[i] = -1;
	i = 0;
	while (i < nfunc) {
		func_ni = top_start + ngdecl + i;
		fid = ast[func_ni].val;
		if (fid >= 0 && fid < 256) {
			body = ast[func_ni].a;
			bstart = ast[body].a;
			bcount = ast[body].b;
			max_local = 0;
			k = 0;
			while (k < bcount && ast[bstart + k].kind == N_DECL) {
				if (ast[bstart + k].val > max_local)
					max_local = ast[bstart + k].val;
				k++;
			}
			st_func_max_local[fid] = max_local;
		}
		i++;
	}

	st_ndemote = 0;
	i = 0;
	while (i < ngdecl - 1) { /* skip last = bss_end */
		gidx = ast[top_start + i].val - GLOBAL_ID_BASE;
		if (gidx < 0 || gidx >= 256 ||
		    ast[top_start + i].a != 1 ||
		    st_gvar_owner[gidx] < 0) {
			i++;
			continue;
		}
		fid = st_gvar_owner[gidx];
		if (BS_TEST(&st_reach[fid * BS_LONGS], fid)) {
			i++;
			continue;
		}
		new_lid = st_func_max_local[fid] + 1;
		if (new_lid >= 60 || st_ndemote >= 64) {
			i++;
			continue;
		}
		st_demote[st_ndemote].gid = ast[top_start + i].val;
		st_demote[st_ndemote].lid = new_lid;
		st_demote[st_ndemote].fid = fid;
		st_demote[st_ndemote].slots = 1;
		st_demote[st_ndemote].width = ast[top_start + i].b; /* preserve element width */
		st_ndemote++;
		st_func_max_local[fid] = new_lid;
		i++;
	}

	/* === PROMOTE: local -> global === */
	st_npromote = 0;
	i = 0;
	while (i < nfunc) {
		func_ni = top_start + ngdecl + i;
		fid = ast[func_ni].val;
		if (fid < 0 || fid >= 256 ||
		    BS_TEST(&st_reach[fid * BS_LONGS], fid)) {
			i++;
			continue;
		}

		body = ast[func_ni].a;
		bstart = ast[body].a;
		bcount = ast[body].b;
		npar = ast[func_ni].b;

		/* scan locals (skip params) for promote candidate */
		j = npar;
		while (j < bcount && ast[bstart + j].kind == N_DECL) {
			if (ast[bstart + j].a == 1 && (rng() & 31) == 0 &&
			    st_npromote < 64) {
				max_gid++;
				st_promote[st_npromote].gid = max_gid + GLOBAL_ID_BASE;
				st_promote[st_npromote].lid = ast[bstart + j].val;
				st_promote[st_npromote].fid = fid;
				st_promote[st_npromote].slots = 1;
				st_promote[st_npromote].width = ast[bstart + j].b; /* preserve element width */
				st_npromote++;
				j = bcount; /* break */
			}
			j++;
		}
		i++;
	}

#ifdef HOST_BUILD
	fprintf(stderr, "  storage: %ld demote, %ld promote\n",
		st_ndemote, st_npromote);
	for (i = 0; i < st_ndemote; i++)
		fprintf(stderr, "    demote g%d -> local %d in func %d\n",
			st_demote[i].gid - GLOBAL_ID_BASE,
			st_demote[i].lid, st_demote[i].fid);
	for (i = 0; i < st_npromote; i++)
		fprintf(stderr, "    promote local %d -> g%d in func %d\n",
			st_promote[i].lid,
			st_promote[i].gid - GLOBAL_ID_BASE,
			st_promote[i].fid);
#endif

	if (st_ndemote == 0 && st_npromote == 0)
		return;

	/* === PATCH REFS: rename variables in affected function bodies === */
	for (i = 0; i < st_ndemote; i++) {
		for (j = 0; j < nfunc; j++) {
			func_ni = top_start + ngdecl + j;
			if (ast[func_ni].val == st_demote[i].fid) {
				st_patch_refs(ast[func_ni].a,
					st_demote[i].gid, st_demote[i].lid);
				break;
			}
		}
	}
	for (i = 0; i < st_npromote; i++) {
		for (j = 0; j < nfunc; j++) {
			func_ni = top_start + ngdecl + j;
			if (ast[func_ni].val == st_promote[i].fid) {
				st_patch_refs(ast[func_ni].a,
					st_promote[i].lid, st_promote[i].gid);
				break;
			}
		}
	}

	/* === REBUILD function bodies: add/remove DECLs === */
	new_nn = nn;
	i = 0;
	while (i < nfunc) {
		func_ni = top_start + ngdecl + i;
		fid = ast[func_ni].val;

		/* check if this function is affected */
		has_demote = 0;
		for (j = 0; j < st_ndemote; j++)
			if (st_demote[j].fid == fid) has_demote = 1;
		has_promote = 0;
		for (j = 0; j < st_npromote; j++)
			if (st_promote[j].fid == fid) has_promote = 1;
		if (!has_demote && !has_promote) {
			i++;
			continue;
		}

		old_body = ast[func_ni].a;
		old_bstart = ast[old_body].a;
		old_bcount = ast[old_body].b;
		old_npar = ast[func_ni].b;

		/* build new body block at new_nn */
		/* new BLOCK node */
		ast[new_nn].kind = N_BLOCK;
		ast[new_nn].a = new_nn + 1;
		ast[new_nn].b = 0; /* filled below */
		ast[new_nn].val = 0;
		ast[func_ni].a = new_nn;
		new_bstart = new_nn + 1;
		new_bcount = 0;
		new_nn++;

		/* copy param DECLs first */
		for (j = 0; j < old_npar && j < old_bcount; j++) {
			node_copy(new_nn, old_bstart + j);
			new_nn++;
			new_bcount++;
		}

		/* add demoted globals as new local DECLs (after params) */
		for (j = 0; j < st_ndemote; j++) {
			if (st_demote[j].fid == fid) {
				ast[new_nn].kind = N_DECL;
				ast[new_nn].a = st_demote[j].slots;
				ast[new_nn].b = st_demote[j].width;
				ast[new_nn].val = st_demote[j].lid;
				new_nn++;
				new_bcount++;
			}
		}

		/* copy existing non-param DECLs, skipping promoted ones */
		for (j = old_npar; j < old_bcount; j++) {
			d = 0;
			if (ast[old_bstart + j].kind == N_DECL) {
					for (k = 0; k < st_npromote; k++) {
					/* after patch_refs, DECL.val is already the new gid */
					if (st_promote[k].fid == fid &&
					    st_promote[k].gid == ast[old_bstart + j].val) {
						d = 1;
						break;
					}
				}
			}
			if (!d) {
				node_copy(new_nn, old_bstart + j);
				new_nn++;
				new_bcount++;
			}
		}

		/* update body block */
		ast[ast[func_ni].a].b = new_bcount;
		i++;
	}

	/* === REBUILD top block: remove demoted DECLs, add promoted ones === */
	/* Append new children directly; update ast[0].a/b at the end */
	new_top_start = new_nn;
	new_top_count = 0;

	/* copy non-demoted global DECLs (except last = _bss_end) */
	for (i = 0; i < ngdecl - 1; i++) {
		demoted = 0;
		for (j = 0; j < st_ndemote; j++) {
			if (st_demote[j].gid == ast[top_start + i].val) {
				demoted = 1;
				break;
			}
		}
		if (!demoted) {
			node_copy(new_nn, top_start + i);
			new_nn++;
			new_top_count++;
		}
	}

	/* add promoted locals as new global DECLs */
	for (j = 0; j < st_npromote; j++) {
		ast[new_nn].kind = N_DECL;
		ast[new_nn].a = st_promote[j].slots;
		ast[new_nn].b = st_promote[j].width;
		ast[new_nn].val = st_promote[j].gid;
		new_nn++;
		new_top_count++;
	}

	/* copy _bss_end (always last DECL) */
	node_copy(new_nn, top_start + ngdecl - 1);
	new_nn++;
	new_top_count++;

	/* copy FUNCs */
	for (i = 0; i < nfunc; i++) {
		node_copy(new_nn, top_start + ngdecl + i);
		new_nn++;
		new_top_count++;
	}

	/* update root block to point to new children */
	ast[0].a = new_top_start;
	ast[0].b = new_top_count;

	*p_ast_len = new_nn * 32;
}

#endif /* MUT_STORAGE */


/* entry point: apply enabled mutations to AST buffer, return new byte length.
 * caller must seed PRNG (srng) before calling. */
long mutate(long ast_buf, long ast_len)
{
	ast = (struct node *)ast_buf;
#ifdef MUT_GLOBAL
	mutate_global();
#endif
#ifdef MUT_LOCAL
	mutate_local();
#endif
#ifdef MUT_STORAGE
	mutate_storage(&ast_len);
#endif

#if defined(MUT_COMMUTE) || defined(MUT_REASSOC) || defined(MUT_IF_INVERT)
#ifdef MUT_IF_INVERT
	/* walk_expr may append LOG_NOT nodes; track free slot */
	nn_mut = ast_len / 32;
#endif
	walk_expr(0);
#ifdef MUT_IF_INVERT
	/* update length to include any appended nodes */
	ast_len = nn_mut * 32;
#endif
#endif
	return ast_len;
}
