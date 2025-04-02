/*
Reverse compression to lower the entropy
(x) 2025, herm1t.vx@gmail.com
herm1tvx.blogspot.com/2025/04/compression-entropy-and-polymorphism.html

For a long time, antivirus programs have used heuristics to look for encrypted
or compressed fragments in files. They can be used to, for example, enable
emulation and wait for the unpacking procedure to complete. This led me to the
question of how to reduce the entropy of encrypted data. One obvious method is
base32 [1]; by adding three zero bits to every five bits, the entropy will not
exceed five.

Another option is inverse compression. We take the encrypted text and decode it
using the Huffman algorithm, and the entropy will decrease. How the "unpacked"
text will look depends on the frequency table, which is calculated during
compression (if the text were compressed). However, that would require storing
it somewhere, which is not very elegant. Fortunately, there is an algorithm
that allows generating a random set of probabilities with a specified
entropy [2].

H_{n-1} = (H_n + p * \log2 p + (1 - p) * (\log2 (1 - p))) / (1 - p)

Since we know H_n (this is the entropy value we are aiming for), we need to
find the roots of this equation. From these, we can get ranges from which we
can randomly select probability p_i. To solve the equation, I first find the
minimum of the function and then find the roots using bisection. Since the sum
of all probabilities cannot exceed one, we need to scale the obtained values
to form a simplex:

sum = 0 p_k = p_k * (1 - sum) sum += p_k

Once we have the vector with probabilities, we use the classic Huffman
algorithm. We sort the probabilities in ascending order, build the character
tree, and instead of compressing the text, we decompress it. Naturally, its
length will increase, but in terms of entropy value and frequencies, it will
correspond to a random model. To decode this text, we will need either the
saved frequency table or the seed of the pseudorandom generator used to
generate the table. Then, it must be compressed using the standard algorithm.

We can also shuffle the resulting array of probabilities to make the symbol
selection random. Or we can randomly swap branches of the tree if they are at
the same level of depth. (If we use a similar trick in regular, non-“inverse”
compression, we would get “polymorphic compression” [3]; in Huffman, only the
length of the code matters, not its specific value.)

	while (list) {
		...
                if (random() > (RAND_MAX / 2)) {
                        void *tmp;
                        tmp = left;
                        left = right;
                        right = tmp;
                }
                // insert new node

References

[1] Modexp “Shellcode: Entropy Reduction With Base32 Encoding”
https://modexp.wordpress.com/2023/04/07/shellcode-entropy-reduction-with-base-n-encoding/

[2] Peter Swaszek, Sidharth Wali “Generating probabilities with a specified entropy”

[3] https://x.com/vx_herm1t/status/1444972910972706816
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

double L(double E, double p)
{
	return (E + p*log2f(p) + (1.0 - p)*(log2f(1 - p))) / (1 - p);
}

double solve(double min, double max, double H, double r, int d)
{
	double med;
	for (int i = 0; i < 20; i++) {
		med = (max - min) / 2.0 + min;
		double v = L(H, med);
		if ((d && (v < r)) || (!d && (v > r)))
			min = med;
		else
			max = med;
	}
	return med;
}

double find_min(double H, double left, double right) {
	const double eps = 1e-6;
	while (right - left > eps) {
		double mid1 = left + (right - left) / 3;
		double mid2 = right - (right - left) / 3;
		if (L(H, mid1) < L(H, mid2))
			right = mid2;
		else
			left = mid1;
	}
	return (left + right) / 2;
}

double rand_range(double min, double max) {
	return min + ((double)random() / (double)RAND_MAX) * (max - min);
}

void simplex(double p[], int n, double E)
{
#ifdef	DEBUG_TEST
	n = 5;
	H = 1.5;
	double next[] = { 0.4715, 0.7871, 0.2085, 0, 0 };
#endif
	double a, b, c;
	for (int i = n - 1; i >= 1; i--) {
		double l = log2f((double)i - 1.0);
		if (1.0 <= E && E <= l) {
			a = solve(0, 1, E, l, 1);
			printf("(1) E = %f [0 %f]", E, a);
			p[i] = rand_range(0, a);
		} else {
			double m = find_min(E, 0.0, 1.0);
			if (E < 1.0) {
				a = solve(0, m, E, 0, 0);
				b = solve(m, 1, E, 0, 1);
				c = solve(0, 1, E, l, 1);
				printf("(2) E = %f [0 %f] [%f %f]", E, a, b, c);
				p[i] = random() & 1 ? rand_range(0, a) : rand_range(b, c);
			} else {
				// E > l
				a = solve(0, m, E, l, 0);
				b = solve(m, 1, E, l, 1);
				printf("(3) E = %f [%f %f]", E, a, b);
				p[i] = rand_range(a, b);
			}
		}
#ifdef	DEBUG_TEST
		p[i] = next[4 - i];
#endif
		printf(" q = %f\n", p[i]);
		E = L(E, p[i]);
	}
	p[1] = a;
	p[0] = 1.0 - a;
	printf("q0,q1, %f %f\n", p[0], p[1]);
	double sum = 0;
	for (int k = n - 1; k >= 0; k--) {
		p[k] = p[k] * (1.0 - sum);
		sum += p[k];
		if (k < 16)
			printf("%f\n", p[k]);
		if (k == 16)
			puts("...");
	}
	printf("Sum %f\n", sum);
}

void shuffle(double *a, size_t n) {
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = random() % (i + 1);
		double t = a[i];
		a[i] = a[j];
		a[j] = t;
	}
}

void print_buf(uint8_t *buf)
{
	int i = 0;
	for (int j = 0; j < 8; j++) {
//		for (int k = 0; k < 32; k++)
//			printf("%02x ", buf[i + k]);
		for (int k = 0; k < 32; k++)
			printf("%c", isprint(buf[i + k]) ? buf[i + k] : '.');
		i += 32;
		puts("");
	}

}

/* boring texbook Huffman without any speedups */
typedef struct node {
	unsigned char v;
	unsigned int freq;
	struct node *left, *right, *next;
} node_t;

/* simple allocator, full tree has 2N - 1 nodes */
node_t node_pool[511];
int np = 0;
node_t *alloc_node(unsigned char v, unsigned int freq, node_t *left, node_t *right) {
	node_t *r = &node_pool[np++];
	r->v = v;
	r->freq = freq;
	r->left = left;
	r->right = right;
	return r;
}

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

/* never use such bitstreams in prod */
unsigned char getbit(unsigned char *in, int *bits) {
	unsigned int p = *bits / 8, r = *bits % 8;
	*bits += 1;
	return (in[p] >> r) & 1;
}

void putbit(unsigned char *out, int *bits, unsigned bit)
{
	unsigned int p = *bits / 8, r = *bits % 8;
	*bits += 1;
	out[p] |= (bit << r);
}

/* encode character by searching the tree (slow!) */
int encode_char(unsigned char *out, int *bits, node_t *n, unsigned char v, unsigned long c, int l) {
	if (n->left == NULL) {
		if (n->v == v) {
			for (int j = l - 1; j >= 0; j--)
				putbit(out, bits, (c >> j) & 1);
			return 1;
		}
	} else {
		c <<= 1;
		l++;
		if (encode_char(out, bits, n->left, v, c | 0, l) ||
			encode_char(out, bits, n->right, v, c | 1, l))
			return 1;
	}
	return 0;
}

double getent(unsigned char *b, int len)
{
	int i;
	unsigned freq[256];
	bzero(freq, sizeof(freq));
	for (int i = 0; i < len; i++)
		freq[b[i]]++;
	double e = 0;
	for (int i = 0; i < 256; i++) {
		if (freq[i]) {
			double p = (double)freq[i] / (double)len;
			e -= p * log2f(p);
		}
	}
	return e;
}

void huff(double p[], int n, unsigned char *in, int len, unsigned char *out)
{
	int i;

	/* in real huffman we count it, here we just scale it */
	unsigned int freq[256];
	bzero(freq, sizeof(freq));
	for (int i = 0; i < n; i++)
		freq[i] = p[i] * 32768;

	node_t *list = NULL;
	for (i = 0; i < 256; i++)
		if (freq[i])
			list = insert(list, alloc_node(i, freq[i], NULL, NULL));

	/* make the tree out of sorted list, still boring as hell */
	/* get L, get R, put <L, R, L.freq + R.freq> */
	node_t *tree = NULL, *left, *right;
	while (list) {
		/* dequeue left */
		left = list;
		list = list->next;

		/* last node is the root */
		if (list == NULL) {
			tree = left;
			break;
		}

		/* dequeue right */
		right = list;
		list = list->next;

		/* insert new node */
		list = insert(list, alloc_node(0, left->freq + right->freq, left, right));
	}

	/* in real huffman we would flatten the tree to adjust too long codes
	and build the table, to speedup compression/decompression */

	printf("Input:\n");
	print_buf(in);
	printf("Input size: %d\n", len);
	printf("Input entropy %f\n", getent(in, len));

	/* expand / decode */
	int bits = 0;
	uint8_t *outs = out;
	while (bits <= (len * 8) - 1) {
		node_t *n;
		for (n = tree; n->left; n = getbit(in, &bits) == 0 ? n->left : n->right)
			;
		*out++ = n->v;
	}
	int olen = out - outs;
	printf("Output:\n");
	print_buf(outs);
	printf("Output size: %d\n", olen);
	printf("Output entropy %f\n", getent(outs, olen));

	/* shrink / encode */
	uint8_t dec[4097];
	bits = 0;
	for (i = 0; i < olen; i++)
		encode_char(dec, &bits, tree, outs[i], 0, 0);

	printf("Compressed size: %d\n", bits / 8);
	if (!memcmp(in, dec, len))
		printf("Decoded successfully\n");
	else
		printf("Decoding failed\n");
}

int main(int argc, char **argv)
{
	int n = 256;
	double p[n], E = 4.1;

	srandom(time(NULL));

	simplex(p, n, E);

	unsigned int freq[256];
	FILE *f;
#ifdef	TEXT
	uint8_t buf[65536];
	bzero(freq, sizeof(freq));
	f = fopen("od.txt", "r");
	if (f) {
		fread(buf, sizeof(buf), 1, f);
		fclose(f);
		for (int i = 0; i < sizeof(buf); i++)
			freq[buf[i]]++;
		for (int i = 0; i < 256; i++)
			p[i] = (double)freq[i] / (double)sizeof(buf);
	}
#endif
	double e = 0;
	for (int i = 0; i < n; i++)
		if (p[i])
			e -= p[i] * log2f(p[i]);
	printf("Simplex entropy %f\n", e);
#if	0
	shuffle(p, n);
#endif
	size_t ilen = 4096, olen = 32768;
	uint8_t in[ilen];
	uint8_t out[olen];
	f = fopen("/dev/urandom", "rb");
	if (f) {
		fread(in, ilen, 1, f);
		fclose(f);
	} else {
		printf("failed to read input!\n");
	}

	huff(p, n, in, ilen, out);
}
