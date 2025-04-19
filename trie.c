/* https://herm1tvx.blogspot.com/2025/04/xz-backdoor-strings-tries-and-automata.html */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <assert.h>
#include <ctype.h>

/* dump array */
void dumpa(uint32_t *p, int size)
{
	for (int i = 0; i < size; i++) {
		printf("%08x ", p[i]);
		if (i % 8 == 7)
			puts("");
	}
	puts("");
}

char *s[] = {
	"arrow",
	"base",
	"bat",
	"case",
	"castle",
	"car",
	"card",
	"care",
	"cat",
	"cats",
	"can",
	"cape",
	"deer",
	"dear",
	"deep",
	"art",
	"article",
	"bar",
	NULL
};

struct trie {
	/* basic trie just term bit and edges */
	/* one could use single "term" node */
	unsigned term:1;
	/* in this toy example I use lowercase letters,
	real trie will have 256 pointers */
	struct trie *c[32];
	/* we will use these later */
	unsigned bitmap;
	unsigned size;
};

void traverse0(struct trie *node)
{
	for (int i = 0; i < 32; i++)
		if (node->c[i]) {
			putchar(i + 'a');
			if (node->c[i]->term)
				putchar('\'');
			traverse0(node->c[i]);
			node->size++;
		}
}

int match0(struct trie *node, char *s)
{
	if (*s == 0) {
		if (node->term)
			return 1;
		return 0;
	}
	unsigned b = *s - 'a';
	if (node->c[b] == NULL)
		return 0;
	return match0(node->c[b], s + 1);
}

/* serialize as S-expression */
char output[128], os = 0;
void serialize(struct trie *node)
{
//	output[os++] = '(';
	for (int i = 0; i < 32; i++)
		if (node->c[i]) {
			output[os++] = node->c[i]->term ? toupper(i + 'a') : i + 'a';
			serialize(node->c[i]);
		}
	output[os++] = ')';
}

/* now we keep only non-NULL pointers and bitmap to find their index */
void compress(struct trie *node)
{
	for (int i = 0, p = 0; i < 32; i++)
		if (node->c[i]) {
			node->bitmap |= 1 << i;
			node->c[p++] = node->c[i];
			compress(node->c[i]);
		}
}

/* match with bitmap trie */
int match1(struct trie *node, char *s)
{
	if (*s == 0) {
		if (node->term)
			return 1;
		return 0;
	}
	unsigned b = 1 << (*s - 'a');
	if ((node->bitmap & b) == 0)
		return 0;
	unsigned next = __builtin_popcount(node->bitmap & (b - 1));
	return match1(node->c[next], s + 1);
}

void traverse1(struct trie *node)
{
	if (node->term)
		putchar('\'');
	int p = 0;
	for (int i = 0; i < 32; i++)
		if (node->bitmap & (1 << i)) {
			putchar(i + 'a');
			traverse1(node->c[p++]);
		}
}

uint32_t n[1024], nmax = 0;

/* array mapped trie */
void encode(struct trie *node)
{
	uint32_t base = nmax;
	n[base] = node->bitmap;
	/* we use single array for masks and indexes */
	/* one could allocate more "slots" for the mask */
	nmax = nmax + 1 + node->size;
	for (int i = 0; i < node->size; i++) {
		n[base + 1 + i] = nmax | (node->c[i]->term << 31);
		encode(node->c[i]);
	}
}

uint32_t x[1024], xmax = 0;

void traverse2(int i)
{
	int bits = 0;
	unsigned mask = n[i];
	for (int j = 0; j < 31; j++) {
		if ((mask >> j) & 1) {
			int next = i + 1 + bits;
			putchar(j + 'a');
			if (n[next] >> 31)
				putchar('\'');
			/* clean term flag */
			traverse2(n[next] & 0x7fffffff);
			bits++;
		}
	}
}

/* use separate arrays for masks and nodes */
unsigned encodex(struct trie *node)
{
	unsigned mask = node->bitmap;
	/* find index in mask array (n), we keep only unique masks */
	int im = -1;
	for (int i = 0; i < nmax; i++)
		if (n[i] == mask) {
			im = i;
			break;
		}
	if (im == -1) {
		im = nmax;
		n[nmax++] = mask;
	}

	/* fill array of edges (x) */
	uint32_t base = xmax;
	xmax += node->size;
	for (int i = 0; i < node->size; i++) {
		/* use relative values */
		/* one could achieve some LZ-style compression by finding
		overlapping values and adjusting relative indexes */
		uint32_t next = xmax - base;
		/* i-th edge: term:1 | next offset:15 | mask index:16 */
		x[base + i] = encodex(node->c[i]) | (next << 16) |
			((unsigned)node->c[i]->term << 31);
	}
	return im;
}

void traversex(int mi, int ni)
{
	unsigned mask = n[mi];

	int bits = 0;
	for (int j = 0; j < 31; j++) {
		if ((mask >> j) & 1) {
			int next = ni + bits;
			putchar(j + 'a');
			if (x[next] >> 31)
				putchar('\'');
			traversex(x[next] & 0xffff, ni + ((x[next] >> 16) & 0x7fff));
			bits++;
		}
	}
}

int _match(int mi, int ni, char *s)
{
	unsigned bp = 1 << (*s - 'a');
	unsigned ma = n[mi];
	if ((ma & bp) == 0)
		return 0;
	unsigned next = ni + __builtin_popcount(ma & (bp - 1));
	if ((x[next] >> 31) && s[1] == 0)
		return 1;
	return _match(x[next] & 0xffff, ni + ((x[next] >> 16) & 0x7fff), s + 1);
}

int match(char *s)
{
	return _match(0, 0, s);
}

int main(int argc, char **argv)
{
	struct trie root;
	bzero(&root, sizeof(root));

	/* build basic trie */
	for (int i = 0; s[i]; i++) {
		struct trie *cur = &root;
		for (int j = 0; s[i][j]; j++) {
			int idx = s[i][j] - 'a';
			if (cur->c[idx] != NULL) {
				cur = cur->c[idx];
				continue;
			}
			struct trie *n = malloc(sizeof(struct trie));
			assert(n != NULL);
			bzero(n, sizeof(struct trie));
			cur->c[idx] = n;
			cur = n;
		}
		cur->term = 1;
	}
	traverse0(&root);
	puts("");

	serialize(&root);
	printf("Serialize trie into S-expression:\n%s\n", output);
	printf("Unserialize:\n");
	char buf[32], pos = 0;
	/* simple automata to reconstruct string array from
	serialized trie */
	for (int i = 0; i < os; i++) {
		char in = output[i];
		switch (in) {
			case '(':
				/* ignore, it's redundant */
				break;
			case ')':
				pos--;
				break;
			case 'A' ... 'Z':
				buf[pos++] = tolower(in);
				buf[pos] = 0;
				printf("%s,", buf);
				break;
			default:
				buf[pos++] = in;
				break;
		}
	}
	puts("");

	assert(match0(&root, "cat") == 1);
	assert(match0(&root, "cas") == 0);
	assert(match0(&root, "case") == 1);
	assert(match0(&root, "xxx") == 0);
	assert(match0(&root, "deer") == 1);


	compress(&root);
	/* now we can get number of children nodes with popcount(bitmap)
	and index of child pointer with popcount(bitmap & ((1 << bitpos) - 1))
	https://en.wikipedia.org/wiki/Bitwise_trie_with_bitmap */

	traverse1(&root);
	puts("");

	assert(match1(&root, "cat") == 1);
	assert(match1(&root, "cas") == 0);
	assert(match1(&root, "case") == 1);
	assert(match1(&root, "xxx") == 0);
	assert(match1(&root, "deer") == 1);

	/* serialize the trie into AMT */
	printf("\nBasic AMT (masks and edges in the same array):\n");
	encode(&root);
	traverse2(0);
	puts("");
	printf("size is %d bytes\n", nmax * 4);
	dumpa(n, nmax);

	printf("\nAMT, XZB-style\n");
	nmax = 0;
	/* split masks and nodes into two arrays (XZ backdoor style) */
	encodex(&root);
	/* XZB uses two pointers (masks and nodes) and LZ-like compression:
	it stores not indexes, but offsets to next mask/edges block which
	could be negative, I use absolute index for masks and relative always
	positive offset for edges, this doesn't saves any bytes, but would
	allow to compress the whole table better */
	traversex(0, 0);
	puts("");
	printf("masks and edges in the separate arrays:\n");
	printf("size is %d bytes (%d masks, %d edges)\n", nmax * 4 + xmax * 4, nmax, xmax);
	printf("masks:\n");
	dumpa(n, nmax);
	printf("edges:\n");
	dumpa(x, xmax);

	assert(match("cat") == 1);
	assert(match("cas") == 0);
	assert(match("case") == 1);
	assert(match("xxx") == 0);
	assert(match("deer") == 1);
	printf("OK\n");

	return 0;
}
