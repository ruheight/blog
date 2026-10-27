/*
 * c0 — Meta-C parser
 *
 * Reads preprocessed Meta-C from stdin, writes serialised AST to stdout.
 * Diagnostics go to stderr.
 *
 * Usage: cpp source.c | ./c0 > ast.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#include "ast.h"

#define MAX_NODES   (1 << 18)
#define MAX_SYMS    4096
#define MAX_INPUT   (8 << 20)
#define MAX_NAME    256
#define MAX_MEMBERS 128
#define MAX_TAGS    512

static struct node nodes[MAX_NODES];
static int nn; /* node count */

/* Token types */
enum {
	T_EOF = 0, T_NUM, T_IDENT,
	T_IF, T_ELSE, T_WHILE, T_DO, T_FOR, T_RETURN, T_BREAK, T_CONTINUE,
	T_STRUCT, T_UNION, T_ENUM, T_SIZEOF,
	T_VOID, T_CHAR_KW, T_SHORT, T_INT, T_LONG, T_SIGNED, T_UNSIGNED,
	T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK,
	T_SEMI, T_COMMA, T_DOT, T_ARROW,
	T_AMP, T_STAR, T_PLUS, T_MINUS, T_SLASH, T_PERCENT,
	T_TILDE, T_BANG, T_PIPE, T_CARET,
	T_LT, T_GT, T_EQ, T_ASSIGN, T_NE, T_LE, T_GE, T_SHL, T_SHR,
	    T_LOG_AND, T_LOG_OR, T_INC, T_DEC,
	T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
	T_AMP_EQ, T_PIPE_EQ, T_CARET_EQ, T_SHL_EQ, T_SHR_EQ,
	T_STRING,
};

enum { SIGN_NONE = 0, SIGN_SIGNED, SIGN_UNSIGNED };

struct type {
	int width;	/* 0=void, 1=char, 2=short, 4=int, 8=long */
	int sign;
	int ptrs;
	int tag;	/* struct/union tag index, -1 if none */
	int is_struct;	/* 1=struct, 2=union, 3=enum */
	int arr_inner;	/* inner (rightmost) array dimension, 0 if none */
};

struct member {
	char name[MAX_NAME];
	struct type type;
	int offset;
	int arr_count;
};

struct tag {
	char name[MAX_NAME];
	int kind;	/* 1=struct, 2=union, 3=enum */
	int size;
	int align;	/* max field alignment (for natural alignment) */
	struct member members[MAX_MEMBERS];
	int nmembers;
	int defined;
};

static struct tag tags[MAX_TAGS];
static int ntags;

struct sym {
	char name[MAX_NAME];
	int id;
	int is_func;
	int is_global;
	int is_param;
	int used;
	int defined;
	int declared;
	int param_count;
	struct type type;
	int arr_count;
	int arr_inner;	/* inner dimension of multidimensional array, 0 if 1D */
	long enum_val;	/* for enum constants (id == -1) */
	int decl_line;	/* source line of declaration */
	int owner_fid;	/* for globals: fid of sole user (-1=none, -2=multi) */
};

static struct sym syms[MAX_SYMS];
static int nsyms;

static int cur_func_id = -1;
static int in_for;  /* forbid continue inside for */
static int in_loop; /* track nesting depth of while/do-while (and for) */
static int local_id_next;
static int local_start;
static int global_id_next = GLOBAL_ID_BASE;
static int func_id_next = 1; /* 0 is reserved for main */
static int func_def_next = 1; /* definition-order counter */
static int fid_remap_old[256], fid_remap_new[256];
static int nfid_remap;

static char input[MAX_INPUT];
static int input_len;
static int pos;
static int line = 1;
static int pack_align = 0; /* 0=natural alignment, N=max alignment limited to N */
static const char *cur_file = "<stdin>";

static int tok;
static long tok_num;
static int tok_unsigned; /* set by U/u suffix on numeric literal */
static char tok_name[MAX_NAME];
static char tok_str[1024];   /* string literal buffer */
static int tok_str_len;      /* length excluding \0 */
static int errors;

static struct type type_ulong = { 8, SIGN_UNSIGNED, 0, -1, 0, 0 };
static struct type type_sint  = { 4, SIGN_SIGNED, 0, -1, 0, 0 };

static void error(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s:%d: error: ", cur_file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	errors++;
}

static void warn(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s:%d: warning: ", cur_file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static int emit(int kind, long a, long b, long val)
{
	int i;

	if (nn >= MAX_NODES) {
		fprintf(stderr, "c0: too many nodes\n");
		exit(1);
	}
	i = nn++;
	nodes[i].kind = kind;
	nodes[i].a = a;
	nodes[i].b = b;
	nodes[i].val = val;
	return i;
}

static int emit_copy(int src)
{
	int i = emit(0, 0, 0, 0);
	nodes[i] = nodes[src];
	return i;
}

static int type_width(struct type *t)
{
	if (t->ptrs > 0)
		return 8;
	if (t->is_struct && t->tag >= 0 && tags[t->tag].defined)
		return tags[t->tag].size;
	return t->width;
}

/* natural alignment for a type: char→1, short→2, int→4, long/ptr→8.
 * For structs, alignment = max alignment of any member (stored in tags[].align).
 * If pack_align > 0, alignment is capped at pack_align. */
static int type_align(struct type *t)
{
	int a;
	if (t->ptrs > 0)
		a = 8;
	else if (t->is_struct && t->tag >= 0 && tags[t->tag].defined)
		a = tags[t->tag].align;
	else
		a = t->width ? t->width : 8;
	if (a > 8) a = 8;
	if (a < 1) a = 1;
	if (pack_align > 0 && a > pack_align)
		a = pack_align;
	return a;
}

static int align_up_c0(int off, int al)
{
	return (off + al - 1) & ~(al - 1);
}

static int type_deref_size(struct type *t)
{
	struct type inner;

	if (t->ptrs <= 0)
		return 1;
	inner = *t;
	inner.ptrs--;
	return type_width(&inner);
}

static int type_is_signed(struct type *t)
{
	if (t->ptrs > 0)
		return 0;
	return t->sign == SIGN_SIGNED;
}

/* array element count for DECL.a field.
 * For arrays of scalars (width <= 8): count = number of elements.
 * For arrays of structs (width > 8): count = element_count * (struct_size / 8). */
static int arr_slots(int count, struct type *t)
{
	int w;

	if (count <= 0)
		return 1;
	w = type_width(t);
	if (w <= 8)
		return count;
	return count * (w / 8);
}

/* element width for DECL.b field.
 * Returns the width in bytes (1/2/4/8) for a variable's storage.
 * Pointers and longs → 8, int → 4, short → 2, char → 1.
 * Struct/union → struct size. */
static int decl_elem_width(struct type *t)
{
	if (t->ptrs > 0)
		return 8;
	if (t->is_struct && t->tag >= 0 && tags[t->tag].defined)
		return tags[t->tag].size;
	return t->width ? t->width : 8;
}

/* Lexer */
static int peek(void)
{
	return (pos < input_len) ? (unsigned char)input[pos] : -1;
}

static int getch(void)
{
	int c;

	if (pos >= input_len)
		return -1;
	c = (unsigned char)input[pos++];
	if (c == '\n')
		line++;
	return c;
}

static void skip_ws(void)
{
	int c;

	for (;;) {
		c = peek();
		if (c < 0)
			return;
		if (c == '#') {
			/* cpp directives: line markers and #pragma pack */
			pos++;
			while (pos < input_len && input[pos] == ' ')
				pos++;
			/* #pragma pack(N) / #pragma pack() */
			if (pos + 11 < input_len &&
			    input[pos]=='p' && input[pos+1]=='r' &&
			    input[pos+2]=='a' && input[pos+3]=='g' &&
			    input[pos+4]=='m' && input[pos+5]=='a' &&
			    input[pos+6]==' ') {
				pos += 7;
				while (pos < input_len && input[pos] == ' ') pos++;
				if (pos + 4 < input_len &&
				    input[pos]=='p' && input[pos+1]=='a' &&
				    input[pos+2]=='c' && input[pos+3]=='k') {
					pos += 4;
					while (pos < input_len && input[pos] == ' ') pos++;
					if (pos < input_len && input[pos] == '(') {
						pos++;
						while (pos < input_len && input[pos] == ' ') pos++;
						if (pos < input_len && input[pos] == ')') {
							/* #pragma pack() — restore default alignment */
							pack_align = 0;
						} else if (pos < input_len && isdigit((unsigned char)input[pos])) {
							int pv = 0;
							while (pos < input_len && isdigit((unsigned char)input[pos]))
								pv = pv * 10 + (input[pos++] - '0');
							/* #pragma pack(N) — limit alignment to N */
							pack_align = pv;
						}
					}
				}
				while (pos < input_len && input[pos] != '\n') pos++;
				if (pos < input_len) pos++;
				continue;
			}
			if (pos < input_len && isdigit((unsigned char)input[pos])) {
				int n = 0;
				while (pos < input_len &&
				       isdigit((unsigned char)input[pos]))
					n = n * 10 + (input[pos++] - '0');
				line = n;
				while (pos < input_len && input[pos] == ' ')
					pos++;
				if (pos < input_len && input[pos] == '"') {
					static char fn[512];
					int fi = 0;
					pos++;
					while (pos < input_len && input[pos] != '"' &&
					       fi < 511)
						fn[fi++] = input[pos++];
					fn[fi] = 0;
					if (pos < input_len)
						pos++;
					cur_file = fn;
				}
			}
			while (pos < input_len && input[pos] != '\n')
				pos++;
			if (pos < input_len)
				pos++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			getch();
			continue;
		}
		break;
	}
}

static int is_id0(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_id1(int c)
{
	return is_id0(c) || (c >= '0' && c <= '9');
}

static int keyword(const char *s)
{
	if (!strcmp(s, "if")) return T_IF;
	if (!strcmp(s, "else")) return T_ELSE;
	if (!strcmp(s, "while")) return T_WHILE;
	if (!strcmp(s, "do")) return T_DO;
	if (!strcmp(s, "for")) return T_FOR;
	if (!strcmp(s, "return")) return T_RETURN;
	if (!strcmp(s, "break")) return T_BREAK;
	if (!strcmp(s, "continue")) return T_CONTINUE;
	if (!strcmp(s, "struct")) return T_STRUCT;
	if (!strcmp(s, "union")) return T_UNION;
	if (!strcmp(s, "enum")) return T_ENUM;
	if (!strcmp(s, "sizeof")) return T_SIZEOF;
	if (!strcmp(s, "void")) return T_VOID;
	if (!strcmp(s, "char")) return T_CHAR_KW;
	if (!strcmp(s, "short")) return T_SHORT;
	if (!strcmp(s, "int")) return T_INT;
	if (!strcmp(s, "long")) return T_LONG;
	if (!strcmp(s, "signed")) return T_SIGNED;
	if (!strcmp(s, "unsigned")) return T_UNSIGNED;
	if (!strcmp(s, "typedef") || !strcmp(s, "goto") ||
	    !strcmp(s, "const") || !strcmp(s, "register") ||
	    !strcmp(s, "static") || !strcmp(s, "auto") ||
	    !strcmp(s, "extern") || !strcmp(s, "volatile") ||
	    !strcmp(s, "switch") ||
	    !strcmp(s, "case") || !strcmp(s, "default")) {
		error("'%s' not supported in Meta-C", s);
		return T_IDENT;
	}
	return 0;
}

static void next(void)
{
	int c, kw;

	skip_ws();
	c = peek();
	if (c < 0) { tok = T_EOF; return; }

	/* number */
	if (isdigit(c)) {
		long v = 0;
		if (c == '0' && pos + 1 < input_len &&
		    (input[pos+1] == 'x' || input[pos+1] == 'X')) {
			pos += 2;
			while (pos < input_len &&
			       isxdigit((unsigned char)input[pos])) {
				c = input[pos++];
				if (c >= '0' && c <= '9') v = v*16 + c - '0';
				else if (c >= 'a' && c <= 'f') v = v*16 + c-'a'+10;
				else v = v*16 + c-'A'+10;
			}
		} else {
			while (pos < input_len &&
			       isdigit((unsigned char)input[pos]))
				v = v*10 + (input[pos++] - '0');
		}
		/* parse U/L suffixes: U=unsigned, L=long (default), UL/LU */
		tok_unsigned = 0;
		while (pos < input_len &&
		       (input[pos]=='u' || input[pos]=='U' ||
		        input[pos]=='l' || input[pos]=='L')) {
			if (input[pos]=='u' || input[pos]=='U')
				tok_unsigned = 1;
			/* L is default (long) — accepted but no effect */
			pos++;
		}
		tok = T_NUM; tok_num = v;
		return;
	}

	/* char literal */
	if (c == '\'') {
		long v;
		getch();
		c = getch();
		if (c == '\\') {
			c = getch();
			switch (c) {
			case 'n': v = '\n'; break;
			case 't': v = '\t'; break;
			case 'r': v = '\r'; break;
			case '\\': v = '\\'; break;
			case '\'': v = '\''; break;
			case '0': v = 0; break;
			case 'a': v = '\a'; break;
			case 'b': v = '\b'; break;
			case 'f': v = '\f'; break;
			case 'x':
				v = 0;
				while (pos < input_len &&
				       isxdigit((unsigned char)input[pos])) {
					c = input[pos++];
					if (c>='0' && c<='9') v = v*16 + c-'0';
					else if (c>='a' && c<='f') v = v*16+c-'a'+10;
					else v = v*16 + c-'A'+10;
				}
				break;
			default:
				if (c >= '0' && c <= '7') {
					v = c - '0';
					if (pos<input_len && input[pos]>='0' && input[pos]<='7')
						v = v*8 + (input[pos++]-'0');
					if (pos<input_len && input[pos]>='0' && input[pos]<='7')
						v = v*8 + (input[pos++]-'0');
				} else {
					v = c;
				}
			}
		} else {
			v = c;
		}
		if (peek() == '\'') getch();
		else error("unterminated char literal");
		tok = T_NUM; tok_num = v; tok_unsigned = 0;
		return;
	}

	/* string literal: collect bytes into tok_str, set tok = T_STRING */
	if (c == '"') {
		int si = 0;
		pos++; /* skip opening quote */
		while (pos < input_len && input[pos] != '"') {
			int ch = input[pos];
			if (ch == '\\' && pos + 1 < input_len) {
				pos++;
				ch = input[pos];
				if (ch == 'n') ch = 10;
				else if (ch == 't') ch = 9;
				else if (ch == '0') ch = 0;
				else if (ch == '\\') ch = 92;
				else if (ch == '"') ch = 34;
			}
			if (si < 1023) tok_str[si++] = ch;
			pos++;
		}
		if (pos < input_len) pos++; /* skip closing " */
		tok_str[si] = 0;
		tok_str_len = si;
		tok = T_STRING;
		return;
	}

	/* identifier */
	if (is_id0(c)) {
		int i = 0;
		while (pos < input_len && is_id1((unsigned char)input[pos]) &&
		       i < MAX_NAME-1)
			tok_name[i++] = input[pos++];
		tok_name[i] = 0;
		kw = keyword(tok_name);
		tok = kw ? kw : T_IDENT;
		return;
	}

	/* punctuation */
	getch();
	switch (c) {
	case '{': tok=T_LBRACE; return;
	case '}': tok=T_RBRACE; return;
	case '(': tok=T_LPAREN; return;
	case ')': tok=T_RPAREN; return;
	case '[': tok=T_LBRACK; return;
	case ']': tok=T_RBRACK; return;
	case ';': tok=T_SEMI; return;
	case ',': tok=T_COMMA; return;
	case '~': tok=T_TILDE; return;
	case '.': tok=T_DOT; return;
	case '+':
		if (peek()=='+') { getch(); tok=T_INC; return; }
		if (peek()=='=') { getch(); tok=T_PLUS_EQ; return; }
		tok=T_PLUS; return;
	case '-':
		if (peek()=='-') { getch(); tok=T_DEC; return; }
		if (peek()=='>') { getch(); tok=T_ARROW; return; }
		if (peek()=='=') { getch(); tok=T_MINUS_EQ; return; }
		tok=T_MINUS; return;
	case '*':
		if (peek()=='=') { getch(); tok=T_STAR_EQ; return; }
		tok=T_STAR; return;
	case '/':
		if (peek()=='=') { getch(); tok=T_SLASH_EQ; return; }
		tok=T_SLASH; return;
	case '%':
		if (peek()=='=') { getch(); tok=T_PERCENT_EQ; return; }
		tok=T_PERCENT; return;
	case '&':
		if (peek()=='&') { getch(); tok=T_LOG_AND; return; }
		if (peek()=='=') { getch(); tok=T_AMP_EQ; return; }
		tok=T_AMP; return;
	case '|':
		if (peek()=='|') { getch(); tok=T_LOG_OR; return; }
		if (peek()=='=') { getch(); tok=T_PIPE_EQ; return; }
		tok=T_PIPE; return;
	case '^':
		if (peek()=='=') { getch(); tok=T_CARET_EQ; return; }
		tok=T_CARET; return;
	case '<':
		if (peek()=='<') {
			getch();
			if (peek()=='=') { getch(); tok=T_SHL_EQ; return; }
			tok=T_SHL; return;
		}
		if (peek()=='=') { getch(); tok=T_LE; return; }
		tok=T_LT; return;
	case '>':
		if (peek()=='>') {
			getch();
			if (peek()=='=') { getch(); tok=T_SHR_EQ; return; }
			tok=T_SHR; return;
		}
		if (peek()=='=') { getch(); tok=T_GE; return; }
		tok=T_GT; return;
	case '=':
		if (peek()=='=') { getch(); tok=T_EQ; return; }
		tok=T_ASSIGN; return;
	case '!':
		if (peek()=='=') { getch(); tok=T_NE; return; }
		tok=T_BANG; return;
	case '?':
		error("ternary not supported in Meta-C");
		tok=T_BANG; return;
	default:
		error("unexpected char '%c' (0x%02x)", c, c);
		next();
	}
}

static void expect(int t, const char *what)
{
	if (tok != t) {
		error("expected %s", what);
		return;
	}
	next();
}

/* Symbol lookup — locals first, then globals */
static struct sym *sym_find(const char *name)
{
	int i;

	if (cur_func_id >= 0) {
		for (i = local_start; i < nsyms; i++)
			if (!strcmp(syms[i].name, name))
				return &syms[i];
	}
	for (i = 0; i < local_start; i++)
		if (!strcmp(syms[i].name, name))
			return &syms[i];
	return NULL;
}

/* track global variable usage: which functions reference it.
 * A single-owner scalar global is a pseudo-static — error. */
static void track_global_use(struct sym *s)
{
	if (!s->is_global || s->is_func || s->id < 0) return;
	if (cur_func_id < 0) return; /* top-level, not inside a function */
	if (s->owner_fid == -1)
		s->owner_fid = cur_func_id;
	else if (s->owner_fid != cur_func_id && s->owner_fid != -2)
		s->owner_fid = -2; /* multi-owner */
}

static struct sym *sym_add(const char *name)
{
	struct sym *s;

	if (nsyms >= MAX_SYMS) {
		fprintf(stderr, "c0: too many symbols\n");
		exit(1);
	}
	s = &syms[nsyms++];
	memset(s, 0, sizeof(*s));
	strncpy(s->name, name, MAX_NAME-1);
	s->type.tag = -1;
	s->id = -2; /* sentinel: not assigned yet */
	s->decl_line = line;
	s->owner_fid = -1; /* no owner yet */
	return s;
}

static int tag_find(const char *name)
{
	int i;
	for (i = 0; i < ntags; i++)
		if (!strcmp(tags[i].name, name))
			return i;
	return -1;
}

static int tag_add(const char *name, int kind)
{
	int i;

	if (ntags >= MAX_TAGS) {
		fprintf(stderr, "c0: too many tags\n");
		exit(1);
	}
	i = ntags++;
	memset(&tags[i], 0, sizeof(tags[i]));
	strncpy(tags[i].name, name, MAX_NAME-1);
	tags[i].kind = kind;
	return i;
}

/* Forward declarations */
static int parse_expr(struct type *tp);
static int parse_assign(struct type *tp);
static int parse_cast(struct type *tp);
static int parse_stmt(void);
static int is_type_tok(void);

static int parse_postfix_tail(int nd, struct type *tp);

/*
 * emit_truncate: if target type is narrower than source, emit AND mask.
 * For signed narrow types, also emit sign-extension via shift pair.
 * Returns the (possibly wrapped) node.
 */
static int emit_truncate(int nd, struct type *from, struct type *to)
{
	int tw, fw;
	long mask;

	/* only truncate scalar-to-scalar (no pointers, no structs) */
	if (to->ptrs > 0 || from->ptrs > 0) return nd;
	if (to->is_struct || from->is_struct) return nd;

	tw = to->width;
	fw = from->width;
	if (tw <= 0 || tw >= fw) return nd;

	/* unsigned: AND mask */
	if (tw == 1) mask = 0xFF;
	else if (tw == 2) mask = 0xFFFF;
	else if (tw == 4) mask = 0xFFFFFFFF;
	else return nd;

	nd = emit(N_AND, nd, emit(N_NUM, 0, 0, mask), 0);

	/* signed narrow: sign-extend via SHL then SHR_S */
	if (to->sign == SIGN_SIGNED) {
		long shift = (8 - tw) * 8;
		nd = emit(N_SHL, nd, emit(N_NUM, 0, 0, shift), 0);
		nd = emit(N_SHR_S, nd, emit(N_NUM, 0, 0, shift), 0);
	}
	return nd;
}

/* Desugar ++/-- to ASSIGN: nd = nd +/- 1 */
static int emit_incdec(int nd, int is_inc)
{
	return emit(N_ASSIGN, nd,
		    emit(is_inc ? N_ADD : N_SUB,
			 emit_copy(nd), emit(N_NUM, 0, 0, 1), 0), 0);
}

/* Parse multidimensional array brackets: [N][M]... → total element count (0 if none)
 * *inner_out = inner (rightmost) dimension, 0 if 1D or none */
static int parse_array_dims(int *inner_out)
{
	int arr = 0, ndims = 0, last = 0;

	while (tok == T_LBRACK) {
		next();
		if (tok == T_RBRACK) {
			/* empty brackets: [] — size from initializer */
			if (arr == 0) arr = -1; /* sentinel: size unknown */
			ndims++;
		} else {
			if (tok != T_NUM) error("expected array size");
			last = (int)tok_num;
			if (arr == 0) arr = last;
			else arr *= last;
			ndims++;
			next();
		}
		expect(T_RBRACK, "']'");
	}
	*inner_out = (ndims >= 2) ? last : 0;
	return arr;
}

/* Type specifier parser */
static struct type parse_typespec(void)
{
	struct type t;
	int has_sign = 0, has_width = 0, is_void = 0;

	memset(&t, 0, sizeof(t));
	t.tag = -1;

	for (;;) {
		if (tok == T_SIGNED) {
			t.sign = SIGN_SIGNED; has_sign = 1; next();
		} else if (tok == T_UNSIGNED) {
			t.sign = SIGN_UNSIGNED; has_sign = 1; next();
		} else if (tok == T_CHAR_KW) {
			t.width = 1; has_width = 1; next();
		} else if (tok == T_SHORT) {
			t.width = 2; has_width = 1; next();
		} else if (tok == T_INT) {
			if (!has_width) t.width = 4;
			has_width = 1; next();
		} else if (tok == T_LONG) {
			t.width = 8; has_width = 1; next();
		} else if (tok == T_VOID) {
			is_void = 1; has_width = 1; next();
		} else if (tok == T_STRUCT || tok == T_UNION) {
			int kind = (tok == T_STRUCT) ? 1 : 2;
			int ti;
			next();
			if (tok != T_IDENT) {
				error("expected tag name");
				break;
			}
			ti = tag_find(tok_name);
			if (ti < 0) ti = tag_add(tok_name, kind);
			t.tag = ti; t.is_struct = kind;
			t.sign = SIGN_NONE; has_width = 1;
			next();
				if (tok == T_LBRACE) {
				int off = 0, maxsz = 0, max_al = 1;
				next();
				tags[ti].defined = 1;
				tags[ti].nmembers = 0;
				while (tok != T_RBRACE && tok != T_EOF) {
					struct type mt = parse_typespec();
					while (tok == T_STAR) { mt.ptrs++; next(); }
					if (tok != T_IDENT) {
						error("expected member name");
						break;
					}
					struct member *m = &tags[ti].members[tags[ti].nmembers];
					strncpy(m->name, tok_name, MAX_NAME-1);
					m->type = mt;
					m->arr_count = 0;
					next();
					int msz = (mt.ptrs > 0) ? 8 :
						  (mt.is_struct && mt.tag >= 0) ?
						  tags[mt.tag].size :
						  (mt.width ? mt.width : 8);
					/* field alignment: natural or capped by pack_align */
					int fal = type_align(&mt);
					if (fal > max_al) max_al = fal;
					{
						int dummy_inner = 0;
						m->arr_count = parse_array_dims(&dummy_inner);
					}
					if (m->arr_count > 0)
						msz *= m->arr_count;
					if (kind == 1) {
						off = align_up_c0(off, fal);
						m->offset = off;
						off += msz;
					} else {
						m->offset = 0;
						if (msz > maxsz) maxsz = msz;
					}
					tags[ti].nmembers++;
					expect(T_SEMI, "';'");
				}
				/* struct size rounded up to max member alignment */
				if (kind == 1)
					tags[ti].size = align_up_c0(off, max_al);
				else
					tags[ti].size = maxsz;
				tags[ti].align = max_al;
				expect(T_RBRACE, "'}'");
			} else if (tags[ti].defined) {
				/* already defined — use existing size */
			}
			t.width = (tags[ti].defined) ? tags[ti].size : 0;
			break;
		} else if (tok == T_ENUM) {
			int ti;
			next();
			ti = -1;
			if (tok == T_IDENT) {
				ti = tag_find(tok_name);
				if (ti < 0) ti = tag_add(tok_name, 3);
				next();
			}
			if (tok == T_LBRACE) {
				long ev = 0;
				next();
				while (tok != T_RBRACE && tok != T_EOF) {
					if (tok != T_IDENT) {
						error("expected enum constant");
						break;
					}
					struct sym *es = sym_find(tok_name);
					if (!es) es = sym_add(tok_name);
					es->is_global = 1;
					es->declared = 1;
					es->used = 1;
					es->id = -1; /* enum constant marker */
					es->type.width = 4;
					es->type.sign = SIGN_SIGNED;
					es->type.tag = -1;
					next();
					if (tok == T_ASSIGN) {
						int neg = 0;
						next();
						if (tok == T_MINUS) { neg = 1; next(); }
						if (tok != T_NUM) error("expected constant");
						ev = neg ? -tok_num : tok_num;
						next();
					}
					es->enum_val = ev++;
					if (tok == T_COMMA) next();
				}
				if (ti >= 0) tags[ti].defined = 1;
				expect(T_RBRACE, "'}'");
			}
			t.width = 4; t.sign = SIGN_SIGNED; has_width = 1;
			break;
		} else {
			break;
		}
	}

	if (is_void) { t.width = 0; t.sign = SIGN_NONE; return t; }
	if (!has_width && has_sign) t.width = 8; /* bare unsigned → unsigned long */
	if (!has_width && !has_sign && !t.is_struct) {
		error("expected type");
		t.width = 4; t.sign = SIGN_SIGNED;
	}
	if (!has_sign && !t.is_struct) t.sign = SIGN_SIGNED;
	return t;
}

static int is_type_tok(void)
{
	return tok == T_VOID || tok == T_CHAR_KW || tok == T_SHORT ||
	       tok == T_INT || tok == T_LONG || tok == T_SIGNED ||
	       tok == T_UNSIGNED || tok == T_STRUCT || tok == T_UNION ||
	       tok == T_ENUM;
}

/* Signedness checks */
static void check_sign(struct type *l, struct type *r, const char *op)
{
	if (l->ptrs > 0 || r->ptrs > 0) return;
	if (l->sign == SIGN_NONE || r->sign == SIGN_NONE) return;
	if (l->sign != r->sign)
		error("mixed signed/unsigned in '%s' (need explicit cast)", op);
}

static int pick_su(struct type *l, struct type *r, int sk, int uk)
{
	if (l->ptrs > 0 || r->ptrs > 0) return uk;
	if (l->sign == SIGN_UNSIGNED || r->sign == SIGN_UNSIGNED) return uk;
	return sk;
}

/* sizeof(type) or sizeof(expr) */
static long eval_sizeof(void)
{
	long sz;

	expect(T_LPAREN, "'('");
	if (is_type_tok()) {
		struct type t = parse_typespec();
		while (tok == T_STAR) { t.ptrs++; next(); }
		sz = type_width(&t);
		if (sz == 0) sz = 8;
	} else {
		struct type t;
		parse_expr(&t);
		sz = type_width(&t);
		if (sz == 0) sz = 8;
	}
	expect(T_RPAREN, "')'");
	return sz;
}

/* Expression parsers */
static int parse_primary(struct type *tp)
{
	int nd;

	if (tok == T_NUM) {
		nd = emit(N_NUM, 0, 0, tok_num);
		tp->width = 8;
		tp->sign = tok_unsigned ? SIGN_UNSIGNED : SIGN_SIGNED;
		tp->ptrs = 0;
		tp->tag = -1;
		tp->is_struct = 0;
		next();
		return nd;
	}

	if (tok == T_SIZEOF) {
		next();
		nd = emit(N_NUM, 0, 0, eval_sizeof());
		*tp = type_ulong;
		return nd;
	}

	if (tok == T_LPAREN) {
		next();
		if (is_type_tok()) {
			/* cast */
			struct type ct = parse_typespec();
			while (tok == T_STAR) { ct.ptrs++; next(); }
			expect(T_RPAREN, "')'");
			nd = parse_cast(tp);
			nd = emit_truncate(nd, tp, &ct);
			*tp = ct;
			return nd;
		}
		nd = parse_expr(tp);
		expect(T_RPAREN, "')'");
		return nd;
	}

	if (tok == T_IDENT) {
		char name[MAX_NAME];
		struct sym *s;

		strncpy(name, tok_name, MAX_NAME);
		next();

		/* function call */
		if (tok == T_LPAREN) {
			int args[6];
			int argc = 0;
			next();
			while (tok != T_RPAREN && tok != T_EOF) {
				struct type at;
				if (argc >= 6) { error("max 6 args"); break; }
				args[argc] = parse_assign(&at);
				argc++;
				if (tok == T_COMMA) next();
			}
			expect(T_RPAREN, "')'");

			s = sym_find(name);
			if (!s) {
				/* unknown identifier with () — assume function */
				s = sym_add(name);
				s->is_func = 1;
				s->is_global = 1;
				s->declared = 1;
				s->id = !strcmp(name, "main") ? 0 : func_id_next++;
			}
			s->used = 1;

			if (s->is_func) {
				/* direct call */
				int astart = nn;
				for (int i = 0; i < argc; i++)
					nodes[emit(0, 0, 0, 0)] = nodes[args[i]];
				nd = emit(N_CALL, astart, argc, s->id);
				*tp = s->type;
				if (tp->width == 0 && tp->ptrs == 0)
					*tp = type_ulong;
			} else {
				/* indirect call: variable holds function pointer */
				track_global_use(s);
				int callee = emit(N_VAR, 0, 0, s->id);
				int astart = nn;
				for (int i = 0; i < argc; i++)
					nodes[emit(0, 0, 0, 0)] = nodes[args[i]];
				nd = emit(N_CALL_IND, callee, argc, astart);
				*tp = type_ulong;
			}
			return nd;
		}

		/* enum constant */
		s = sym_find(name);
		if (s && s->id == -1 && s->declared) {
			nd = emit(N_NUM, 0, 0, s->enum_val);
			*tp = type_sint;
			return nd;
		}

		/* function used as value: address-of-function */
		if (s && s->is_func) {
			s->used = 1;
			nd = emit(N_FUNC_ADDR, 0, 0, s->id);
			*tp = type_ulong;
			tp->ptrs = 1;
			return nd;
		}

		/* variable */
		if (!s) {
			error("undefined '%s'", name);
			s = sym_add(name);
			s->declared = 1; s->id = 0; s->type = type_ulong;
		}
		s->used = 1;
		track_global_use(s);
		nd = emit(N_VAR, 0, 0, s->id);
		*tp = s->type;
		if (s->arr_count > 0 && s->id != -1) {
			nd = emit(N_ADDR, nd, 0, 0);
			tp->ptrs++;
			if (s->arr_inner > 0)
				tp->arr_inner = s->arr_inner;
		}
		return nd;
	}

	error("expected expression");
	*tp = type_ulong;
	return emit(N_NUM, 0, 0, 0);
}

/* Postfix tail: [i], .x, ->x, (args) applied to an already-parsed nd */
static int parse_postfix_tail(int nd, struct type *tp)
{
	for (;;) {
		/* indirect call: expr(args) */
		if (tok == T_LPAREN) {
			int args[6];
			int argc = 0;
			int astart;
			next();
			while (tok != T_RPAREN && tok != T_EOF) {
				struct type at;
				if (argc >= 6) { error("max 6 args"); break; }
				args[argc] = parse_assign(&at);
				argc++;
				if (tok == T_COMMA) next();
			}
			expect(T_RPAREN, "')'");
			astart = nn;
			for (int i = 0; i < argc; i++)
				nodes[emit(0, 0, 0, 0)] = nodes[args[i]];
			nd = emit(N_CALL_IND, nd, argc, astart);
			*tp = type_ulong;
			continue;
		}
		if (tok == T_LBRACK) {
			struct type it;
			int idx, esz, sc, add;

			next();
			idx = parse_expr(&it);
			expect(T_RBRACK, "']'");

			/* 2D array outer subscript: stride = inner_dim * elem_size, no deref */
			if (tp->arr_inner > 0) {
				esz = tp->arr_inner * (type_deref_size(tp) > 0 ? type_deref_size(tp) : 8);
				if (esz > 1) {
					sc = emit(N_NUM, 0, 0, esz);
					idx = emit(N_MUL, idx, sc, 0);
				}
				nd = emit(N_ADD, nd, idx, 0);
				tp->arr_inner = 0; /* inner dim consumed */
				continue;
			}

			esz = type_deref_size(tp);
			if (esz > 1) {
				sc = emit(N_NUM, 0, 0, esz);
				idx = emit(N_MUL, idx, sc, 0);
			}
			add = emit(N_ADD, nd, idx, 0);

			struct type inner = *tp;
			if (inner.ptrs > 0) inner.ptrs--;
			int w = type_width(&inner);
			if (w == 0) w = 8;

			if (inner.is_struct && inner.ptrs == 0) {
				nd = add;
				*tp = inner;
				tp->ptrs = 1;
			} else {
				nd = emit(N_DEREF, add, w, 0);
				*tp = inner;
			}
			continue;
		}

		if (tok == T_DOT || tok == T_ARROW) {
			int is_dot = (tok == T_DOT);
			int ti, mi;
			struct member *m = NULL;

			next();
			if (tok != T_IDENT) { error("expected member"); break; }

			ti = tp->tag;
			if (ti < 0) { error("member access on non-struct"); next(); continue; }

			for (mi = 0; mi < tags[ti].nmembers; mi++) {
				if (!strcmp(tags[ti].members[mi].name, tok_name)) {
					m = &tags[ti].members[mi];
					break;
				}
			}
			if (!m) { error("no member '%s'", tok_name); next(); continue; }
			next();

			/* '.' on a struct value: need address, not value.
			 * '->' on a pointer: nd already holds the address. */
			if (is_dot && tp->ptrs == 0)
				nd = emit(N_ADDR, nd, 0, 0);

			if (m->offset > 0)
				nd = emit(N_ADD, nd, emit(N_NUM, 0, 0, m->offset), 0);

			int mw = type_width(&m->type);
			if (mw == 0) mw = 8;

			if (m->type.is_struct || m->arr_count > 0) {
				*tp = m->type;
				tp->ptrs = 1;
			} else if (m->type.ptrs > 0) {
				nd = emit(N_DEREF, nd, 8, 0);
				*tp = m->type;
			} else {
				nd = emit(N_DEREF, nd, mw, 0);
				*tp = m->type;
			}
			continue;
		}
		/* T_INC/T_DEC handled by parse_stmt, not here */
		break;
	}
	return nd;
}

/* Postfix: a[i], a.x, a->x */
static int parse_postfix(struct type *tp)
{
	return parse_postfix_tail(parse_primary(tp), tp);
}

static int parse_unary(struct type *tp)
{
	int nd;

	if (tok == T_MINUS) {
		next();
		/* constant fold: -(literal) → negative literal (1 node vs 2) */
		if (tok == T_NUM) {
			nd = emit(N_NUM, 0, 0, -tok_num);
			tp->width = 8;
			tp->sign = tok_unsigned ? SIGN_UNSIGNED : SIGN_SIGNED;
			tp->ptrs = 0;
			tp->tag = -1;
			tp->is_struct = 0;
			next();
			return nd;
		}
		nd = parse_cast(tp);
		return emit(N_NEG, nd, 0, 0);
	}
	if (tok == T_TILDE) {
		next();
		nd = parse_cast(tp);
		return emit(N_NOT, nd, 0, 0);
	}
	if (tok == T_BANG) {
		next();
		nd = parse_cast(tp);
		*tp = type_ulong;
		return emit(N_LOG_NOT, nd, 0, 0);
	}
	/* T_INC/T_DEC as prefix handled by parse_stmt, not here.
	 * If we see them in expression context, it's an error. */
	if (tok == T_INC || tok == T_DEC)
		error("++/-- only allowed as statement (not in expression)");
	if (tok == T_AMP) {
		next();
		nd = parse_cast(tp);
		/* array/struct decay already produced pointer — &arr == arr */
		if (nodes[nd].kind == N_ADDR)
			return nd;
		/* &(*p) == p — cancel deref */
		if (nodes[nd].kind == N_DEREF) {
			tp->ptrs++;
			return nodes[nd].a;
		}
		/* struct subscript yields pointer arithmetic, & is redundant */
		if (tp->ptrs > 0 && nodes[nd].kind == N_ADD)
			return nd;
		nd = emit(N_ADDR, nd, 0, 0);
		tp->ptrs++;
		return nd;
	}
	if (tok == T_STAR) {
		next();
		nd = parse_cast(tp);
		if (tp->ptrs > 0) {
			tp->ptrs--;
			int w = type_width(tp);
			if (w == 0) w = 8;
			if (tp->is_struct && tp->ptrs == 0) {
				tp->ptrs = 1;
				return nd;
			}
			return emit(N_DEREF, nd, w, 0);
		}
		return emit(N_DEREF, nd, 8, 0);
	}
	return parse_postfix(tp);
}

static int parse_cast(struct type *tp)
{
	if (tok == T_LPAREN) {
		/* Peek: is this (type)expr or (expr)? */
		next();
		if (is_type_tok()) {
			struct type ct = parse_typespec();
			while (tok == T_STAR) { ct.ptrs++; next(); }
			expect(T_RPAREN, "')'");
			int nd = parse_cast(tp);
			nd = emit_truncate(nd, tp, &ct);
			*tp = ct;
			return nd;
		}
		/* not a cast — it was '(' expr ')'; apply postfix []/./-> */
		int nd = parse_expr(tp);
		expect(T_RPAREN, "')'");
		return parse_postfix_tail(nd, tp);
	}
	return parse_unary(tp);
}

static int parse_mul(struct type *tp)
{
	int nd = parse_cast(tp);
	while (tok == T_STAR || tok == T_SLASH || tok == T_PERCENT) {
		int op = tok;
		struct type rt;
		next();
		int r = parse_cast(&rt);
		if (op == T_STAR) {
			check_sign(tp, &rt, "*");
			nd = emit(N_MUL, nd, r, 0);
		} else if (op == T_SLASH) {
			check_sign(tp, &rt, "/");
			nd = emit(pick_su(tp, &rt, N_DIV_S, N_DIV_U), nd, r, 0);
		} else {
			check_sign(tp, &rt, "%");
			nd = emit(pick_su(tp, &rt, N_MOD_S, N_MOD_U), nd, r, 0);
		}
	}
	return nd;
}

static int parse_add(struct type *tp)
{
	int nd = parse_mul(tp);
	while (tok == T_PLUS || tok == T_MINUS) {
		int op = tok;
		struct type rt;
		next();
		int r = parse_mul(&rt);
		if (op == T_PLUS) {
			if (tp->ptrs > 0 && rt.ptrs == 0) {
				int esz = type_deref_size(tp);
				if (esz > 1) r = emit(N_MUL, r, emit(N_NUM,0,0,esz), 0);
			} else if (rt.ptrs > 0 && tp->ptrs == 0) {
				int esz = type_deref_size(&rt);
				if (esz > 1) nd = emit(N_MUL, nd, emit(N_NUM,0,0,esz), 0);
				*tp = rt;
			}
			nd = emit(N_ADD, nd, r, 0);
		} else {
			if (tp->ptrs > 0 && rt.ptrs > 0) {
				/* ptr - ptr: byte diff / element size */
				int esz = type_deref_size(tp);
				nd = emit(N_SUB, nd, r, 0);
				if (esz > 1)
					nd = emit(N_DIV_S, nd, emit(N_NUM,0,0,esz), 0);
				tp->ptrs = 0;
				tp->width = 8;
				tp->sign = SIGN_SIGNED;
			} else {
				if (tp->ptrs > 0 && rt.ptrs == 0) {
					int esz = type_deref_size(tp);
					if (esz > 1) r = emit(N_MUL, r, emit(N_NUM,0,0,esz), 0);
				}
				nd = emit(N_SUB, nd, r, 0);
			}
		}
	}
	return nd;
}

static int parse_shift(struct type *tp)
{
	int nd = parse_add(tp);
	while (tok == T_SHL || tok == T_SHR) {
		int op = tok;
		struct type rt;
		next();
		int r = parse_add(&rt);
		if (op == T_SHL) nd = emit(N_SHL, nd, r, 0);
		else nd = emit(type_is_signed(tp) ? N_SHR_S : N_SHR_U, nd, r, 0);
	}
	return nd;
}

static int parse_rel(struct type *tp)
{
	int nd = parse_shift(tp);
	while (tok == T_LT || tok == T_GT || tok == T_LE || tok == T_GE) {
		int op = tok;
		struct type rt;
		next();
		int r = parse_shift(&rt);
		check_sign(tp, &rt, op==T_LT?"<":op==T_GT?">":op==T_LE?"<=":">=");
		int k;
		switch (op) {
		case T_LT: k = pick_su(tp,&rt,N_LT_S,N_LT_U); break;
		case T_GT: k = pick_su(tp,&rt,N_GT_S,N_GT_U); break;
		case T_LE: k = pick_su(tp,&rt,N_LE_S,N_LE_U); break;
		default:   k = pick_su(tp,&rt,N_GE_S,N_GE_U); break;
		}
		nd = emit(k, nd, r, 0);
		*tp = type_ulong;
	}
	return nd;
}

static int parse_eq(struct type *tp)
{
	int nd = parse_rel(tp);
	while (tok == T_EQ || tok == T_NE) {
		int op = tok;
		struct type rt;
		next();
		int r = parse_rel(&rt);
		nd = emit(op == T_EQ ? N_EQ : N_NE, nd, r, 0);
		*tp = type_ulong;
	}
	return nd;
}

static int parse_band(struct type *tp)
{
	int nd = parse_eq(tp);
	while (tok == T_AMP) { struct type rt; next(); int r = parse_eq(&rt); nd = emit(N_AND, nd, r, 0); }
	return nd;
}

static int parse_bxor(struct type *tp)
{
	int nd = parse_band(tp);
	while (tok == T_CARET) { struct type rt; next(); int r = parse_band(&rt); nd = emit(N_XOR, nd, r, 0); }
	return nd;
}

static int parse_bor(struct type *tp)
{
	int nd = parse_bxor(tp);
	while (tok == T_PIPE) { struct type rt; next(); int r = parse_bxor(&rt); nd = emit(N_OR, nd, r, 0); }
	return nd;
}

static int parse_land(struct type *tp)
{
	int nd = parse_bor(tp);
	while (tok == T_LOG_AND) { struct type rt; next(); int r = parse_bor(&rt); nd = emit(N_LOG_AND, nd, r, 0); *tp = type_ulong; }
	return nd;
}

static int parse_lor(struct type *tp)
{
	int nd = parse_land(tp);
	while (tok == T_LOG_OR) { struct type rt; next(); int r = parse_land(&rt); nd = emit(N_LOG_OR, nd, r, 0); *tp = type_ulong; }
	return nd;
}

static int parse_assign(struct type *tp)
{
	int nd = parse_lor(tp);

	if (tok == T_ASSIGN) {
		struct type rt;
		next();
		int r = parse_assign(&rt);
		if (tp->is_struct && tp->ptrs == 0 &&
		    rt.is_struct && rt.ptrs == 0)
			error("struct assignment not supported"
			      " (use field-by-field copy)");
		return emit(N_ASSIGN, nd, r, 0);
	}

	/* compound assignment: desugar x op= y → x = x op y */
	if (tok >= T_PLUS_EQ && tok <= T_SHR_EQ) {
		int op = tok;
		struct type rt;
		next();
		int r = parse_assign(&rt);
		int dup = emit(nodes[nd].kind, nodes[nd].a, nodes[nd].b, nodes[nd].val);
		int bin;
		switch (op) {
		case T_PLUS_EQ:    bin = emit(N_ADD, dup, r, 0); break;
		case T_MINUS_EQ:   bin = emit(N_SUB, dup, r, 0); break;
		case T_STAR_EQ:    bin = emit(N_MUL, dup, r, 0); break;
		case T_SLASH_EQ:   bin = emit(pick_su(tp,&rt,N_DIV_S,N_DIV_U), dup, r, 0); break;
		case T_PERCENT_EQ: bin = emit(pick_su(tp,&rt,N_MOD_S,N_MOD_U), dup, r, 0); break;
		case T_AMP_EQ:     bin = emit(N_AND, dup, r, 0); break;
		case T_PIPE_EQ:    bin = emit(N_OR, dup, r, 0); break;
		case T_CARET_EQ:   bin = emit(N_XOR, dup, r, 0); break;
		case T_SHL_EQ:     bin = emit(N_SHL, dup, r, 0); break;
		case T_SHR_EQ:     bin = emit(type_is_signed(tp)?N_SHR_S:N_SHR_U, dup, r, 0); break;
		default:           bin = r; break;
		}
		return emit(N_ASSIGN, nd, bin, 0);
	}
	return nd;
}

static int parse_expr(struct type *tp)
{
	return parse_assign(tp);
}

/* Parse compound statement (block in braces) or single statement.
 * Returns a BLOCK node. */
static int parse_body(void)
{
	if (tok == T_LBRACE) {
		int stmts[8192], ns = 0;
		next();
		while (tok != T_RBRACE && tok != T_EOF) {
			int nd = parse_stmt();
			if (nd >= 0 && ns < 8192)
				stmts[ns++] = nd;
		}
		expect(T_RBRACE, "'}'");
		int start = nn;
		for (int i = 0; i < ns; i++)
			nodes[emit(0,0,0,0)] = nodes[stmts[i]];
		return emit(N_BLOCK, start, ns, 0);
	}
	/* single statement */
	int nd = parse_stmt();
	int start = nn;
	if (nd >= 0) {
		nodes[emit(0,0,0,0)] = nodes[nd];
		return emit(N_BLOCK, start, 1, 0);
	}
	return emit(N_BLOCK, start, 0, 0);
}

/* Parse a function body: { decls... stmts... } */
static void parse_func_body(int *decl_nodes, int *ndecls,
			    int *stmt_nodes, int *nstmts)
{
	*ndecls = 0;
	*nstmts = 0;

	expect(T_LBRACE, "'{'");

	/* declarations first */
	while (tok != T_RBRACE && tok != T_EOF && is_type_tok()) {
		struct type bt = parse_typespec();
		/* parse one or more declarators */
		for (;;) {
			struct type vt = bt;
			while (tok == T_STAR) { vt.ptrs++; next(); }
			if (tok != T_IDENT) {
				error("expected variable name");
				break;
			}
			char vname[MAX_NAME];
			strncpy(vname, tok_name, MAX_NAME);
			next();

			int arr_inner_dim = 0;
			int arr = parse_array_dims(&arr_inner_dim);

			struct sym *s = sym_find(vname);
			if (s && s->declared && !s->is_func && !s->is_global)
				error("redefinition of '%s'", vname);
			if (!s) s = sym_add(vname);
			s->type = vt;
			s->arr_count = arr;
			s->arr_inner = arr_inner_dim;
			s->declared = 1;
			s->is_global = 0;
			s->id = local_id_next++;

				/* emit DECL: .a = slot count, .b = element width */
				{
					int slots = arr_slots(arr, &vt);
					int ew = decl_elem_width(&vt);
					if (slots == 1 && vt.is_struct && vt.ptrs == 0) {
						int sw = type_width(&vt);
						if (sw > 8) slots = (sw + 7) / 8;
					}
					if (*ndecls < 4096)
						decl_nodes[(*ndecls)++] = emit(N_DECL, slots, ew, s->id);
				}

					/* initializer: x = expr, or x[] = "string" */
			if (tok == T_ASSIGN) {
				next();
				if (tok == T_STRING && vt.width == 1 && vt.ptrs == 0) {
					/* char s[] = "..."; — fixup array size from string length,
					 * then emit packed stores: 8/4/2/1 byte chunks.
					 * The DECL was already emitted above with arr_slots,
					 * so patch its .a field to the correct count. */
					int slen = tok_str_len + 1; /* include \0 */
					int di = decl_nodes[*ndecls - 1]; /* last DECL */
					nodes[di].a = slen; /* fix slot count */
					s->arr_count = slen;
					{
						int off = 0;
						while (off + 8 <= slen) {
							long v8 = 0;
							int k;
							for (k = 0; k < 8; k++)
								v8 |= ((long)(unsigned char)tok_str[off + k]) << (k * 8);
							int addr = emit(N_ADD, emit(N_ADDR, emit(N_VAR, 0, 0, s->id), 0, 0),
									emit(N_NUM, 0, 0, off), 0);
							int deref = emit(N_DEREF, addr, 8, 0);
							int st = emit(N_ASSIGN, deref, emit(N_NUM, 0, 0, v8), 0);
							if (*nstmts < 8192) stmt_nodes[(*nstmts)++] = st;
							off += 8;
						}
						if (off + 4 <= slen) {
							long v4 = 0;
							int k;
							for (k = 0; k < 4; k++)
								v4 |= ((long)(unsigned char)tok_str[off + k]) << (k * 8);
							int addr = emit(N_ADD, emit(N_ADDR, emit(N_VAR, 0, 0, s->id), 0, 0),
									emit(N_NUM, 0, 0, off), 0);
							int deref = emit(N_DEREF, addr, 4, 0);
							int st = emit(N_ASSIGN, deref, emit(N_NUM, 0, 0, v4), 0);
							if (*nstmts < 8192) stmt_nodes[(*nstmts)++] = st;
							off += 4;
						}
						if (off + 2 <= slen) {
							long v2 = 0;
							int k;
							for (k = 0; k < 2; k++)
								v2 |= ((long)(unsigned char)tok_str[off + k]) << (k * 8);
							int addr = emit(N_ADD, emit(N_ADDR, emit(N_VAR, 0, 0, s->id), 0, 0),
									emit(N_NUM, 0, 0, off), 0);
							int deref = emit(N_DEREF, addr, 2, 0);
							int st = emit(N_ASSIGN, deref, emit(N_NUM, 0, 0, v2), 0);
							if (*nstmts < 8192) stmt_nodes[(*nstmts)++] = st;
							off += 2;
						}
						while (off < slen) {
							int addr = emit(N_ADD, emit(N_ADDR, emit(N_VAR, 0, 0, s->id), 0, 0),
									emit(N_NUM, 0, 0, off), 0);
							int deref = emit(N_DEREF, addr, 1, 0);
							int st = emit(N_ASSIGN, deref, emit(N_NUM, 0, 0, (unsigned char)tok_str[off]), 0);
							if (*nstmts < 8192) stmt_nodes[(*nstmts)++] = st;
							off++;
						}
					}
					next(); /* consume T_STRING */
				} else {
					if (arr > 0)
						error("array init not supported");
					{
						struct type rt;
						int r = parse_assign(&rt);
						int v = emit(N_VAR, 0, 0, s->id);
						int a = emit(N_ASSIGN, v, r, 0);
						if (*nstmts < 8192)
							stmt_nodes[(*nstmts)++] = a;
					}
				}
			}

			if (tok == T_COMMA) { next(); continue; }
			break;
		}
		expect(T_SEMI, "';'");
	}

	/* statements */
	int seen_decl_after = 0;
	while (tok != T_RBRACE && tok != T_EOF) {
		if (is_type_tok() && !seen_decl_after) {
			error("declaration after statement (C89)");
			seen_decl_after = 1;
		}
		int nd = parse_stmt();
		if (nd >= 0 && *nstmts < 8192)
			stmt_nodes[(*nstmts)++] = nd;
	}
	expect(T_RBRACE, "'}'");
}

static int parse_stmt(void)
{
	if (tok == T_IF) {
		struct type ct;
		int cond, then_blk, else_blk = 0;

		next();
		expect(T_LPAREN, "'('");
		cond = parse_expr(&ct);
		expect(T_RPAREN, "')'");
		then_blk = parse_body();

		if (tok == T_ELSE) {
			next();
			else_blk = parse_body();
		}
		return emit(N_IF, cond, then_blk, else_blk);
	}

	if (tok == T_WHILE) {
		struct type ct;
		int cond, body;

		next();
		expect(T_LPAREN, "'('");
		cond = parse_expr(&ct);
		expect(T_RPAREN, "')'");
		in_loop++;
		body = parse_body();
		in_loop--;
		return emit(N_WHILE, cond, body, 0);
	}

	if (tok == T_FOR) {
		struct type ct;
		int init_nd, cond, step_nd, body, wbody;
		int wstart, wcount;

		next();
		expect(T_LPAREN, "'('");

		/* init */
		if (tok == T_SEMI) {
			next();
			init_nd = -1;
		} else {
			init_nd = parse_expr(&ct);
			expect(T_SEMI, "';'");
		}

		/* cond */
		if (tok == T_SEMI) {
			cond = emit(N_NUM, 0, 0, 1); /* for(;;) = while(1) */
			next();
		} else {
			cond = parse_expr(&ct);
			expect(T_SEMI, "';'");
		}

		/* step (handle i++ / i-- / ++i / --i as sugar) */
		if (tok == T_RPAREN) {
			step_nd = -1;
		} else if (tok == T_INC || tok == T_DEC) {
			int op = tok;
			next();
			step_nd = parse_expr(&ct);
			step_nd = emit_incdec(step_nd, op == T_INC);
		} else {
			step_nd = parse_expr(&ct);
			if (tok == T_INC || tok == T_DEC) {
				int op = tok;
				next();
				step_nd = emit_incdec(step_nd, op == T_INC);
			}
		}
		expect(T_RPAREN, "')'");

		/* body (continue forbidden — would skip step) */
		{
			int save_in_for = in_for;
			in_for = 1;
			in_loop++;
			body = parse_body();
			in_loop--;
			in_for = save_in_for;
		}

		/* build while body: original body stmts + step */
		wstart = nn;
		wcount = 0;
		/* copy body children */
		if (nodes[body].kind == N_BLOCK) {
			int bi;
			for (bi = 0; bi < (int)nodes[body].b; bi++) {
				nodes[emit(0,0,0,0)] = nodes[(int)nodes[body].a + bi];
				wcount++;
			}
		} else {
			nodes[emit(0,0,0,0)] = nodes[body];
			wcount++;
		}
		if (step_nd >= 0) {
			nodes[emit(0,0,0,0)] = nodes[step_nd];
			wcount++;
		}
		wbody = emit(N_BLOCK, wstart, wcount, 0);

		/* build: init; while(cond) { body; step; } */
		if (init_nd >= 0) {
			int bstart = nn;
			nodes[emit(0,0,0,0)] = nodes[init_nd];
			emit(N_WHILE, cond, wbody, 0);
			return emit(N_BLOCK, bstart, 2, 0);
		}
		return emit(N_WHILE, cond, wbody, 0);
	}

	if (tok == T_DO) {
		struct type ct;
		int body, cond;

		next();
		in_loop++;
		body = parse_body();
		in_loop--;
		expect(T_WHILE, "'while'");
		expect(T_LPAREN, "'('");
		cond = parse_expr(&ct);
		expect(T_RPAREN, "')'");
		expect(T_SEMI, "';'");
		return emit(N_DO_WHILE, body, cond, 0);
	}

	if (tok == T_RETURN) {
		next();
		if (tok == T_SEMI) { next(); return emit(N_RETURN, 0, 0, 0); }
		struct type rt;
		int nd = parse_expr(&rt);
		expect(T_SEMI, "';'");
		return emit(N_RETURN, nd, 0, 0);
	}

	if (tok == T_BREAK) {
		if (in_loop <= 0)
			error("break/continue only inside while/do-while");
		next();
		expect(T_SEMI, "';'");
		return emit(N_BREAK, 0, 0, 0);
	}

	if (tok == T_CONTINUE) {
		if (in_loop <= 0)
			error("break/continue only inside while/do-while");
		else if (in_for)
			error("continue inside for not supported"
			      " (for desugars to while, continue would skip step)");
		next();
		expect(T_SEMI, "';'");
		return emit(N_CONTINUE, 0, 0, 0);
	}

	if (tok == T_LBRACE)
		return parse_body();

	/* prefix ++i; / --i; as statement */
	if (tok == T_INC || tok == T_DEC) {
		struct type et;
		int op = tok;
		int nd;
		next();
		nd = parse_expr(&et);
		expect(T_SEMI, "';'");
		return emit_incdec(nd, op == T_INC);
	}

	/* expression statement (including postfix i++; / i--;) */
	{
		struct type et;
		int nd = parse_expr(&et);
		/* postfix ++/-- as statement: only on simple lvalue */
		if (tok == T_INC || tok == T_DEC) {
			int op = tok;
			if (nodes[nd].kind != N_VAR && nodes[nd].kind != N_DEREF)
				error("++/-- only allowed on simple lvalue as statement");
			next();
			expect(T_SEMI, "';'");
			return emit_incdec(nd, op == T_INC);
		}
		expect(T_SEMI, "';'");
		if (nodes[nd].kind == N_LOG_AND || nodes[nd].kind == N_LOG_OR)
			error("logical expression as statement has no effect"
			      " (use if() instead)");
		else if (nodes[nd].kind != N_ASSIGN && nodes[nd].kind != N_CALL)
			warn("expression statement with unused value");
		return nd;
	}
}

/* Parse function definition */
static int parse_func(struct type ret, const char *name)
{
	struct sym *fs;
	int fid, pcnt = 0;
	int decl_buf[4096], stmt_buf[8192];
	int ndecls, nstmts;

	fs = sym_find(name);
	if (fs && fs->is_func && fs->defined)
		error("redefinition of function '%s'", name);
	if (!fs) {
		fs = sym_add(name);
		fs->is_func = 1;
		fs->is_global = 1;
		fs->declared = 1;
	}
	if (!strcmp(name, "main"))
		fs->id = 0;
	else if (fs->id < 0)
		fs->id = func_id_next++;
	fs->type = ret;
	fid = fs->id;

	/* enter function scope */
	cur_func_id = fid;
	local_start = nsyms;
	local_id_next = 0;

	/* parse params */
	expect(T_LPAREN, "'('");
	if (tok != T_RPAREN) {
		if (tok == T_VOID) {
			next();
		} else {
			while (tok != T_RPAREN && tok != T_EOF) {
				struct type pt = parse_typespec();
				while (tok == T_STAR) { pt.ptrs++; next(); }
				if (tok == T_IDENT) {
					struct sym *ps = sym_add(tok_name);
					ps->type = pt;
					ps->is_param = 1;
					ps->id = local_id_next++;
					ps->declared = 1;
					next();
				} else {
					struct sym *ps = sym_add("_");
					ps->type = pt;
					ps->is_param = 1;
					ps->id = local_id_next++;
					ps->declared = 1;
				}
				if (tok == T_LBRACK) {
					next();
					if (tok != T_RBRACK) next();
					expect(T_RBRACK, "']'");
				}
				pcnt++;
				if (tok == T_COMMA) next();
			}
		}
	}
	expect(T_RPAREN, "')'");
	fs->param_count = pcnt;

	/* forward declaration (prototype): type name(...); */
	if (tok == T_SEMI) {
		next();
		nsyms = local_start;
		cur_func_id = -1;
		return -1;
	}

	/* has body — mark as defined */
	fs->defined = 1;

	/* record fid remap: definition-order fid */
	if (fid != 0 && nfid_remap < 256) {
		fid_remap_old[nfid_remap] = fid;
		fid_remap_new[nfid_remap] = func_def_next++;
		nfid_remap++;
	}

	/* parse body */
	parse_func_body(decl_buf, &ndecls, stmt_buf, &nstmts);

	/* build function body BLOCK: param DECLs, local DECLs, stmts */
	int body_start = nn;
	int body_count = 0;

	/* param DECLs: params always width=8 (SysV ABI passes all as 64-bit) */
	for (int i = local_start; i < nsyms; i++) {
		if (syms[i].is_param) {
			emit(N_DECL, syms[i].arr_count > 0 ? syms[i].arr_count : 1, 8, syms[i].id);
			body_count++;
		}
	}
	/* local DECLs (in declaration order) */
	for (int i = 0; i < ndecls; i++) {
		nodes[emit(0,0,0,0)] = nodes[decl_buf[i]];
		body_count++;
	}
	/* init assignments + statements (in order) */
	for (int i = 0; i < nstmts; i++) {
		nodes[emit(0,0,0,0)] = nodes[stmt_buf[i]];
		body_count++;
	}

	int body_blk = emit(N_BLOCK, body_start, body_count, 0);
	int func_nd = emit(N_FUNC, body_blk, pcnt, fid);

	/* check unused locals (report at declaration line) */
	for (int i = local_start; i < nsyms; i++) {
		if (!syms[i].used && !syms[i].is_func && !syms[i].is_param &&
		    syms[i].id >= 0 && strcmp(syms[i].name, "_")) {
			int save_line = line;
			line = syms[i].decl_line;
			error("unused variable '%s'", syms[i].name);
			line = save_line;
		}
	}

	/* discard local symbols so they don't pollute global scope */
	nsyms = local_start;
	cur_func_id = -1;
	return func_nd;
}

/* Parse one or more global variable declarations of type t.
 * name[] holds the first declarator name (already consumed). */
static void parse_global_vars(struct type bt, char *name,
			     int *gdecls, int *ngdecls, int *bss_end_pos)
{
	struct type t = bt;

	for (;;) {
		struct type vt = t;
		int arr = 0;
		int arr_inner_dim = 0;

		arr = parse_array_dims(&arr_inner_dim);
		if (tok == T_ASSIGN)
			error("global init not supported in Meta-C");

		struct sym *s = sym_find(name);
		if (s && s->declared && s->is_global && !s->is_func)
			error("redefinition of '%s'", name);
		if (!s) s = sym_add(name);
		s->type = vt;
		s->arr_count = arr;
		s->arr_inner = arr_inner_dim;
		s->is_global = 1;
		s->declared = 1;
		s->id = global_id_next++;

			/* emit DECL: .a = slot count, .b = element width */
			{
				int slots = arr_slots(arr, &vt);
				int ew = decl_elem_width(&vt);
				if (slots == 1 && vt.is_struct && vt.ptrs == 0) {
					int sw = type_width(&vt);
					if (sw > 8) slots = (sw + 7) / 8;
				}
				int dnd = emit(N_DECL, slots, ew, s->id);
				if (!strcmp(name, "_bss_end"))
					*bss_end_pos = *ngdecls;
				if (*ngdecls < 1024) gdecls[(*ngdecls)++] = dnd;
			}

		if (tok != T_COMMA) break;
		next();
		t = bt;
		while (tok == T_STAR) { t.ptrs++; next(); }
		if (tok != T_IDENT) { error("expected name"); break; }
		strncpy(name, tok_name, MAX_NAME);
		next();
	}
	expect(T_SEMI, "';'");
}

/* Top-level parser */
static void parse_top(void)
{
	int funcs[1024], nfuncs = 0;
	int gdecls[1024], ngdecls = 0;
	int bss_end_pos = -1;

	while (tok != T_EOF) {
		if (!is_type_tok()) {
			error("expected declaration");
			next();
			continue;
		}

		struct type bt = parse_typespec();

		/* bare struct/union/enum definition: type ; */
		if (tok == T_SEMI) {
			next();
			continue;
		}

		while (tok == T_STAR) { bt.ptrs++; next(); }
		if (tok != T_IDENT) {
			error("expected identifier");
			next();
			continue;
		}
		char name[MAX_NAME];
		strncpy(name, tok_name, MAX_NAME);
		next();

		if (tok == T_LPAREN) {
			int fnd = parse_func(bt, name);
			if (fnd >= 0 && nfuncs < 1024)
				funcs[nfuncs++] = fnd;
		} else {
			parse_global_vars(bt, name, gdecls, &ngdecls, &bss_end_pos);
		}
	}

	/* move _bss_end to the end */
	if (bss_end_pos >= 0 && bss_end_pos < ngdecls - 1) {
		int tmp = gdecls[bss_end_pos];
		for (int i = bss_end_pos; i < ngdecls - 1; i++)
			gdecls[i] = gdecls[i + 1];
		gdecls[ngdecls - 1] = tmp;
	}

	/* renumber global IDs: contiguous in final DECL order.
	 * Two-phase to avoid collisions (old_id may equal another new_id).
	 * Phase 1: old_id → temp (GLOBAL_ID_BASE + ngdecls + i)
	 * Phase 2: temp → final (GLOBAL_ID_BASE + i) */
	for (int phase = 0; phase < 2; phase++) {
		for (int i = 0; i < ngdecls; i++) {
			int old_id, new_id;
			if (phase == 0) {
				old_id = (int)nodes[gdecls[i]].val;
				new_id = GLOBAL_ID_BASE + ngdecls + i; /* temp */
			} else {
				old_id = GLOBAL_ID_BASE + ngdecls + i; /* temp */
				new_id = GLOBAL_ID_BASE + i;           /* final */
			}
			if (old_id != new_id) {
				for (int k = 0; k < nn; k++) {
					if (nodes[k].val == old_id &&
					    (nodes[k].kind == N_VAR || nodes[k].kind == N_DECL))
						nodes[k].val = new_id;
				}
				for (int k = 0; k < nsyms; k++) {
					if (syms[k].is_global && syms[k].id == old_id)
						syms[k].id = new_id;
				}
			}
		}
	}

	/* renumber function IDs: definition order instead of first-mention order.
	 * Two-phase like globals to avoid collisions.
	 * Phase 1: old_fid → temp (256 + i)
	 * Phase 2: temp → new_fid */
	for (int phase = 0; phase < 2; phase++) {
		for (int i = 0; i < nfid_remap; i++) {
			int old_id, new_id;
			if (phase == 0) {
				old_id = fid_remap_old[i];
				new_id = 256 + i; /* temp */
			} else {
				old_id = 256 + i; /* temp */
				new_id = fid_remap_new[i]; /* final */
			}
			if (old_id != new_id) {
				for (int k = 0; k < nn; k++) {
					if (nodes[k].kind == N_FUNC && nodes[k].val == old_id)
						nodes[k].val = new_id;
					if (nodes[k].kind == N_CALL && nodes[k].val == old_id)
						nodes[k].val = new_id;
				}
				for (int k = 0; k < nsyms; k++) {
					if (syms[k].is_func && syms[k].id == old_id)
						syms[k].id = new_id;
				}
			}
		}
	}

	/* build top-level BLOCK (node 0) */
	int tstart = nn;
	int tcount = 0;
	for (int i = 0; i < ngdecls; i++) {
		nodes[emit(0,0,0,0)] = nodes[gdecls[i]];
		tcount++;
	}
	for (int i = 0; i < nfuncs; i++) {
		nodes[emit(0,0,0,0)] = nodes[funcs[i]];
		tcount++;
	}
	nodes[0].kind = N_BLOCK;
	nodes[0].a = tstart;
	nodes[0].b = tcount;
	nodes[0].val = 0;

	/* diagnostics */
	for (int i = 0; i < nsyms; i++) {
		if (syms[i].is_global && !syms[i].is_func && !syms[i].used &&
		    syms[i].declared && syms[i].id >= 0 &&
		    strcmp(syms[i].name, "_bss_end"))
			error("unused global '%s'", syms[i].name);
		if (syms[i].is_func && !syms[i].used && syms[i].defined &&
		    strcmp(syms[i].name, "main"))
			warn("unused function '%s'", syms[i].name);
		if (syms[i].is_func && syms[i].used && !syms[i].defined)
			error("undefined function '%s'", syms[i].name);
		/* single-function scalar global = pseudo-static.
		 * Breaks MUT_STORAGE demote: global persists between calls
		 * but local doesn't. Use a local variable instead.
		 * Exclude struct/union globals: they test BSS struct access
		 * and MUT_STORAGE doesn't demote multi-slot values. */
		if (syms[i].is_global && !syms[i].is_func &&
		    syms[i].used && syms[i].id >= 0 &&
		    syms[i].arr_count == 0 &&
		    !syms[i].type.is_struct &&
		    syms[i].owner_fid >= 0 &&
		    strcmp(syms[i].name, "_bss_end"))
			error("global '%s' used only in one function"
			      " (should be local)", syms[i].name);
	}
	{
		int found = 0;
		for (int i = 0; i < nsyms; i++)
			if (!strcmp(syms[i].name, "_bss_end")) found = 1;
		if (!found) warn("_bss_end not declared");
	}
}

static int opt_symtab;

static void dump_symtab(void)
{
	int i;
	fprintf(stderr, "\n=== Symbol table ===\n");
	fprintf(stderr, "%-24s %6s %5s %s\n", "name", "id", "type", "defined");
	for (i = 0; i < nsyms; i++) {
		const char *kind;
		if (syms[i].is_func)
			kind = "func";
		else if (syms[i].is_global)
			kind = "global";
		else if (syms[i].is_param)
			kind = "param";
		else
			kind = "local";
		if (syms[i].id < 0) continue; /* enum constants */
		fprintf(stderr, "%-24s %6d %5s %s\n",
			syms[i].name,
			syms[i].id,
			kind,
			syms[i].defined ? "def" : "decl");
	}
}

int main(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-S")) opt_symtab = 1;
	}

	input_len = fread(input, 1, MAX_INPUT, stdin);
	if (input_len <= 0) {
		fprintf(stderr, "c0: empty input\n");
		return 1;
	}

	/* reserve node 0 for top-level BLOCK */
	emit(0, 0, 0, 0);

	next();
	parse_top();

	if (errors > 0) {
		fprintf(stderr, "c0: %d error(s)\n", errors);
		return 1;
	}

	if (opt_symtab)
		dump_symtab();

	fwrite(nodes, sizeof(nodes[0]), nn, stdout);
	fprintf(stderr, "c0: %d nodes, %ld bytes\n", nn,
		(long)(nn * sizeof(nodes[0])));
	return 0;
}
