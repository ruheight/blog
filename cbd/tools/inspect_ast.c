/*
 * inspect_ast — dump serialised AST as s-expressions
 *
 * Usage: ./inspect_ast < ast.bin
 *    or: ./inspect_ast ast.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

static struct node *nodes;
static int nnodes;

static const char *vname(long id, char *buf, int sz)
{
	if (id >= GLOBAL_ID_BASE)
		snprintf(buf, sz, "g%ld", id - GLOBAL_ID_BASE);
	else
		snprintf(buf, sz, "v%ld", id);
	return buf;
}

static const char *fname(long id, char *buf, int sz)
{
	if (id == 0)
		snprintf(buf, sz, "main");
	else
		snprintf(buf, sz, "f%ld", id);
	return buf;
}

/* type name for DECL.b width: 1=char 2=short 4=int 8/0=long */
static const char *typename_w(long w)
{
	if (w == 1) return "char";
	if (w == 2) return "short";
	if (w == 4) return "int";
	if (w > 8) return "struct";
	return "long";
}

/* deref width label: 1=char 2=short 4=int 8=long */
static const char *deref_w(long w)
{
	if (w == 1) return "char";
	if (w == 2) return "short";
	if (w == 4) return "int";
	return "";
}

static const char *binop(int k)
{
	switch (k) {
	case N_ADD:    return "+";
	case N_SUB:    return "-";
	case N_MUL:    return "*";
	case N_DIV_S:  return "/s";
	case N_DIV_U:  return "/u";
	case N_MOD_S:  return "%s";
	case N_MOD_U:  return "%u";
	case N_SHL:    return "<<";
	case N_SHR_S:  return ">>s";
	case N_SHR_U:  return ">>u";
	case N_AND:    return "&";
	case N_OR:     return "|";
	case N_XOR:    return "^";
	case N_LOG_AND: return "&&";
	case N_LOG_OR: return "||";
	case N_EQ:     return "==";
	case N_NE:     return "!=";
	case N_LT_S:   return "<s";
	case N_LT_U:   return "<u";
	case N_GT_S:   return ">s";
	case N_GT_U:   return ">u";
	case N_LE_S:   return "<=s";
	case N_LE_U:   return "<=u";
	case N_GE_S:   return ">=s";
	case N_GE_U:   return ">=u";
	}
	return "?";
}

/* is this an expression node (prints inline, no newline) */
static int is_expr(int idx)
{
	int k;
	if (idx < 0 || idx >= nnodes)
		return 0;
	k = (int)nodes[idx].kind;
	if (k == N_NUM || k == N_VAR)
		return 1;
	if (k >= N_ADD && k <= N_GE_U)
		return 1;
	if (k == N_NEG || k == N_NOT || k == N_LOG_NOT)
		return 1;
	if (k == N_DEREF || k == N_ADDR)
		return 1;
	if (k == N_ASSIGN || k == N_CALL || k == N_CALL_IND || k == N_FUNC_ADDR)
		return 1;
	return 0;
}

/* print DECL: (type name) or (type name[count]) or (struct(sz) name[count]) */
static void print_decl(struct node *d, char *buf, int sz)
{
	long w = d->b;
	long count = d->a;
	const char *tn = typename_w(w);

	if (w > 8) {
		/* struct/large type: show byte size */
		if (count > 1)
			printf("(struct(%ld) %s[%ld])", w, vname(d->val, buf, sz), count);
		else
			printf("(struct(%ld) %s)", w, vname(d->val, buf, sz));
	} else if (count > 1) {
		printf("(%s %s[%ld])", tn, vname(d->val, buf, sz), count);
	} else {
		printf("(%s %s)", tn, vname(d->val, buf, sz));
	}
}

/* print expression inline (no leading indent, no trailing newline) */
static void expr(int idx)
{
	struct node *n;
	char buf[32];
	int i, k;

	if (idx < 0 || idx >= nnodes) {
		printf("?");
		return;
	}
	n = &nodes[idx];
	k = (int)n->kind;

	if (k == N_NUM) {
		if (n->val >= -128 && n->val <= 4096)
			printf("%ld", n->val);
		else
			printf("0x%lx", n->val);
		return;
	}
	if (k == N_VAR) {
		printf("%s", vname(n->val, buf, sizeof(buf)));
		return;
	}
	if (k >= N_ADD && k <= N_GE_U) {
		printf("(%s ", binop(k));
		expr((int)n->a);
		printf(" ");
		expr((int)n->b);
		printf(")");
		return;
	}
	if (k == N_NEG) { printf("(- "); expr((int)n->a); printf(")"); return; }
	if (k == N_NOT) { printf("(~ "); expr((int)n->a); printf(")"); return; }
	if (k == N_LOG_NOT) { printf("(! "); expr((int)n->a); printf(")"); return; }
	if (k == N_DEREF) {
		const char *dw = deref_w(n->b);
		if (dw[0])
			printf("(*%s ", dw);
		else
			printf("(* ");
		expr((int)n->a);
		printf(")");
		return;
	}
	if (k == N_ADDR) {
		printf("(& ");
		expr((int)n->a);
		printf(")");
		return;
	}
	if (k == N_ASSIGN) {
		printf("(= ");
		expr((int)n->a);
		printf(" ");
		expr((int)n->b);
		printf(")");
		return;
	}
	if (k == N_CALL) {
		printf("(%s", fname(n->val, buf, sizeof(buf)));
		for (i = 0; i < (int)n->b; i++) {
			printf(" ");
			expr((int)n->a + i);
		}
		printf(")");
		return;
	}
	if (k == N_CALL_IND) {
		printf("(icall ");
		expr((int)n->a);
		for (i = 0; i < (int)n->b; i++) {
			printf(" ");
			expr((int)n->val + i);
		}
		printf(")");
		return;
	}
	if (k == N_FUNC_ADDR) {
		printf("&%s", fname(n->val, buf, sizeof(buf)));
		return;
	}
	printf("?%d", k);
}

/* indent */
static void ind(int d)
{
	int i;
	for (i = 0; i < d; i++)
		printf("  ");
}

/* print statement/block with indentation */
static void stmt(int idx, int depth)
{
	struct node *n;
	char buf[32];
	int i, k;

	if (idx < 0 || idx >= nnodes)
		return;
	n = &nodes[idx];
	k = (int)n->kind;

	if (k == N_BLOCK) {
		for (i = 0; i < (int)n->b; i++)
			stmt((int)n->a + i, depth);
		return;
	}

	ind(depth);

	if (k == N_FUNC) {
		/* find param count and list them */
		struct node *body = &nodes[(int)n->a];
		int npar = (int)n->b;
		printf("(func %s (", fname(n->val, buf, sizeof(buf)));
		for (i = 0; i < npar && i < (int)body->b; i++) {
			struct node *d = &nodes[(int)body->a + i];
			if (d->kind == N_DECL) {
				if (i > 0) printf(" ");
				printf("%s", vname(d->val, buf, sizeof(buf)));
			}
		}
		printf(")\n");
		/* print decls (locals only, skip params) */
		for (i = npar; i < (int)body->b; i++) {
			struct node *d = &nodes[(int)body->a + i];
			if (d->kind != N_DECL)
				break;
			ind(depth + 1);
			print_decl(d, buf, sizeof(buf));
			printf("\n");
		}
		/* print body stmts */
		for (; i < (int)body->b; i++)
			stmt((int)body->a + i, depth + 1);
		ind(depth);
		printf(")\n");
		return;
	}

	if (k == N_DECL) {
		print_decl(n, buf, sizeof(buf));
		printf("\n");
		return;
	}

	if (k == N_IF) {
		printf("(if ");
		expr((int)n->a);
		printf("\n");
		stmt((int)n->b, depth + 1);
		if (n->val) {
			ind(depth); printf("  ;else\n");
			stmt((int)n->val, depth + 1);
		}
		ind(depth); printf(")\n");
		return;
	}

	if (k == N_WHILE) {
		printf("(while ");
		expr((int)n->a);
		printf("\n");
		stmt((int)n->b, depth + 1);
		ind(depth); printf(")\n");
		return;
	}

	if (k == N_DO_WHILE) {
		printf("(do\n");
		stmt((int)n->a, depth + 1);
		ind(depth); printf("  ;while ");
		expr((int)n->b);
		printf(")\n");
		return;
	}

	if (k == N_RETURN) {
		if (n->a) {
			printf("(return ");
			expr((int)n->a);
			printf(")\n");
		} else {
			printf("(return)\n");
		}
		return;
	}

	if (k == N_BREAK)    { printf("(break)\n"); return; }
	if (k == N_CONTINUE) { printf("(continue)\n"); return; }

	/* expression statement */
	if (is_expr(idx)) {
		expr(idx);
		printf("\n");
		return;
	}

	printf("(?%d a=%ld b=%ld val=%ld)\n", k, n->a, n->b, n->val);
}

int main(int argc, char **argv)
{
	FILE *f;
	long sz;

	if (argc > 1) {
		f = fopen(argv[1], "rb");
		if (!f) { perror(argv[1]); return 1; }
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fseek(f, 0, SEEK_SET);
	} else {
		f = stdin;
		sz = 0;
	}

	if (sz > 0) {
		nodes = malloc(sz);
		fread(nodes, 1, sz, f);
	} else {
		nodes = malloc(1 << 24);
		sz = fread(nodes, 1, 1 << 24, f);
	}
	if (argc > 1)
		fclose(f);

	if (sz <= 0 || sz % sizeof(*nodes) != 0) {
		fprintf(stderr, "invalid AST: %ld bytes\n", sz);
		return 1;
	}

	nnodes = sz / sizeof(*nodes);
	fprintf(stderr, "%d nodes (%ld bytes)\n", nnodes, sz);
	stmt(0, 0);

	free(nodes);
	return 0;
}
