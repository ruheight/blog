#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

typedef struct node {
	unsigned char v;
	unsigned int freq;
	struct node *left, *right, *next;
} node_t;

int encode(uint8_t *out, uint8_t *in, int l)
{
	uint8_t *start = out;

	/* simple allocator, full tree has 2N - 1 nodes */
	int np = 0;
	node_t node_pool[511];
	bzero(node_pool, sizeof(node_pool));

	node_t *alloc_node(unsigned char v, unsigned int freq, node_t *left, node_t *right) {
		node_t *r = &node_pool[np++];
		r->v = v;
		r->freq = freq;
		r->left = left;
		r->right = right;
		return r;
	}

	/* bitstream */
	int bits = 0;
	void putbit(unsigned bit) {
		if (bits == 8) {
			bits = 0;
			*++out = 0;
		}
		*out = (*out << 1) | bit;
		bits++;
	}

	/* count frequencies */
	int i, j;
	unsigned int freq[256];
	bzero(freq, sizeof(freq));
	for (i = 0; i < l; i++)
		freq[in[i]]++;

	/* make the list of nodes sorted by freq, lowest first */
	node_t *insert(node_t *list, node_t *node) {
		if (list == NULL)
			return node;
		node_t *p = list, *prev = NULL;
		while (p != NULL) {
			if (node->freq < p->freq) {
				if (prev == NULL) {
					node->next = list;
					list = node;
				} else {
					node->next = prev->next;
					prev->next = node;
				}
				return list;
			}
			prev = p;
			p = p->next;
		}
		node->next = NULL;
		prev->next = node;
		return list;
	}
	node_t *list = NULL;
	for (i = 0; i < 256; i++)
		if (freq[i])
			list = insert(list, alloc_node(i, freq[i], NULL, NULL));

	/* make the tree out of sorted list */
	/* get L, get R, put <L, R, L.freq + R.freq> */
	node_t *tree = NULL, *left, *right;
	while (list) {
		left = list;
		list = list->next;

		if (list == NULL) {
			tree = left;
			break;
		}

		right = list;
		list = list->next;

		if (random() > (RAND_MAX / 2)) {
			void *tmp;
			tmp = left;
			left = right;
			right = tmp;
		}

		/* insert new node */
		list = insert(list, alloc_node(0, left->freq + right->freq, left, right));
	}

	/* serialize the tree */
	int marker = random() > (RAND_MAX / 2) ? 1 : 0;
	putbit(marker);
	void save_tree(node_t *n) {
		if (n->left == NULL) {
			putbit(marker);
			unsigned char s = n->v;
			for (i = 0; i < 8; i++) {
				putbit(s >> 7);
				s <<= 1;
			}
		} else {
			putbit(1 - marker);
			save_tree(n->left);
			save_tree(n->right);
		}
	}
	save_tree(tree);

	/* encode character by searching the tree (slow!) */
	int encode_char(node_t *n, unsigned char v, unsigned long c, int l) {
		if (n->left == NULL) {
			if (n->v == v) {
				for (j = l - 1; j >= 0; j--)
					putbit((c >> j) & 1);
				return 1;
			}
		} else {
			c <<= 1;
			l++;
			if (encode_char(n->left, v, c | 0, l) || encode_char(n->right, v, c | 1, l))
				return 1;
		}
		return 0;
	}
	for (i = 0; i < l; i++)
		encode_char(tree, in[i], 0, 0);

	while (bits < 8)
		putbit(0);

	return out - start;
}

void decode(uint8_t *out, uint8_t *in, int l)
{
	/* the same poormans alloc */
	int np = 0;
	node_t node_pool[511];
	node_t *alloc(void) {
		assert(np < 511);
		return &node_pool[np++];
	}

	/* bitstream */
	int bits = 0;
	unsigned char c, r;
	unsigned char getbit(void) {
		if (bits == 0) {
			c = *in++;
			bits = 8;
		}
		r = c;
		c <<= 1;
		bits--;
		return r >> 7;
	}

	/* reconstruct tree */
	int marker = getbit();
	node_t *read_tree() {
		node_t *n = alloc();
		if (getbit() == marker) {
			int i;
			for (i = 0; i < 8; i++) {
				n->v <<= 1;
				n->v |= getbit();
			}
			n->left = n->right = NULL;
		} else {
			n->left = read_tree();
			n->right = read_tree();
		}
		return n;
	}
	node_t *tree = read_tree();

	/* decode loop */
	uint8_t *end = out + l;
	while (out < end) {
		node_t *n = tree;
		while (n->left)
			n = getbit() == 0 ? n->left : n->right;
		*out++ = n->v;
	}
}

int main(int argc, char **argv)
{
	srandom(time(NULL));
	int s, k, j, x;
	uint8_t *i = strdup(argv[1]);
	s = strlen(i);
	uint8_t *o = malloc(s * 2);
	uint8_t *t = malloc(s);

	printf("Input:\n%s\nLen: %d\n", i, s);
	x = 0;
	for (k = 0; k < 16; k++) {
		int c = encode(o, i, s);
		if (x == 0) {
			printf("Compressed len: %d\n", c);
			x = 1;
		}
		for (j = 0; j < c; j++)
//			printf("%02x", o[j]);
			if (isprint(o[j]) && o[j] != '<')
				printf("%c", o[j]);
			else
				printf(".");

		puts("");
		decode(t, o, s);
		assert(memcmp(t, i, s) == 0);
	}
}
