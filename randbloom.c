/*
Random hashes and bloom filters
https://herm1tvx.blogspot.com/2025/03/revisiting-api-hashing-random-hashes.html
(x) 2025, herm1t.vx@gmail.com

To do something useful, a virus or a beacon needs system functions. The obvious
way is to simply compile a list of the necessary functions, thus simplifying the
work for antivirus and reverse engineers. To prevent strings from being visible
in the code, the string itself can be replaced with a hash function. It seems
that the first person to come up with this technique was StarZero from IKX
group. In his virus Voodoo (autumn 1998), he used CRC32 to search for the
necessary APIs. Despite the fact that over twenty-five years have passed, it is
still used in products such as Cobalt Strike and Metasploit.

But why CRC32? In 2004, Z0mbie suggested generating hash functions randomly,
using the template H = ROL(H, x) + C. Since the list of all function names is
known, one can check the hash function for collisions. And such a hash
(ROR 13/ADD) is used in Cobalt Strike. Moreover, if the constant in the ROR
instruction is changed, the number of detections is significantly reduced.

First, I'll note that to get a hash function, we need to mix the current hash
value with the string character and then modify the value itself. These
functions are called bit mixers. For mixing, we can use addition, subtraction,
XOR, bitwise OR, and AND. To modify the resulting hash value, we apply the same
operations with shifts and random constants. Then, we simply count the number
of collisions, as we know the complete list of strings. To make everything work,
we only need two instructions: H op= C and H op= x, but there could be more.

two insns:

h ^= c;       h <<= 7 ;     5f1a78fc 6dfcb77c fc4d9458 b07f8df7
h ^= c;       h ^= h << 7 ; af0469ff 9de76def 0d9f14dc 5d266a3e
h ^= h << 8 ; h -= c;       01ba9e07 04b2920f 278a6796 3f149f08
h ^= c;       h <<= 6 ;     84e68d17 87bb3ad7 e0526355 57b08292
h ^= c;       h <<= 6 ;     84e68d17 87bb3ad7 e0526355 57b08292
h *= 27;      h -= c;       fce78723 fce752f7 d15d5384 cedd7cf4
h *= 22;      h += c;       015fb542 015fce93 518288ab 3de30d57
h ^= c;       h *= h >> 1 ; 47238200 172ac567 4e058200 03743335

three insns:

h <<= 17;     h -= h >> 23; h += c;       02e4083b 02fe080e 034f61d5 b0b601a4
h ^= h >> 17; h <<= 10;     h -= c;       9819d0ba 576950c3 8d211d62 70236089
h ^= h << 5 ; h *= h >> 9 ; h ^= c;       7a944b1c 03dffc91 5d8e1515 4e4ffed3
h -= h << 14; h -= c;       h -= h << 1 ; 2fb3009d 6fba8081 b005c008 900d004c
h ^= c;       h ^= h >> 21; h <<= 13;     2c3aacbd c055473c 862a5c83 9429902d
h += 18;      h -= c;       h += h << 10; a41dc261 e4ddfa69 0dbf77a4 89a12404
h <<= 20;     h *= 30;      h -= c;       52d1a0ec c2113e65 cd91d113 bb9d6191
h += h << 28; h ^= h << 6 ; h += c;       1ef009f9 bef48b71 00630aaa 1a296978

They don't have to be perfect, it's enough for them to be good enough for the
specific data.

However, constants like 0x573fe1e3 or 0xc179afea draw attention, and if we
combine them into an array, it may trigger a heuristic rule identifying sections
with high entropy, which indicates that the data is encrypted, compressed, or,
as in our case, indistinguishable from random. But we can use a Bloom filter to
determine if the current string is part of our known set. This is a pretty simple
construct. We calculate k different hash functions and for each value H_k, we set
H_k % m to 1, where m is the size of the filter in bits. If we have n elements in
the filter (the strings we need), we can calculate the other parameters, and all
necessary formulas are available on Wikipedia. The advantage of the filter is that
it's small and sparse. If a string is definitely not in our set, the filter will
always respond "no," but if the string is present, the filter responds "maybe yes"
with a certain probability. The fewer elements in the filter (n), the larger the
size of the filter (m), and the more independent hash functions (k), the lower the
probability of a false positive.

We already have a large number of random hash functions, but code that frequently
includes bit instructions and random constants will look very similar to an
encryption routine. It would be nice to have fewer hash functions (but then
false positives will increase). One possible scenario is to take the Kirsch-
Mitzenmacher formula: h_i = h_1 + i * h_2, but a much better solution was
suggested by Russ Cox, who referenced Vyssotsky's article on "rotating hash."
Instead of calculating different hash functions, we calculate a single value,
and the i-th hash is computed as Hi = ROL(H_{i-1}, x). This way, there will
only be one hash function in the code. Moreover, we can use any bit mixer,
not just ROL or ROR

64 bytes, 5 hashes, 67  bits, 0 coll., 2724 unique
02400032004102c2a0041083500210000888a001102000000080081284223800
001400000200010404000000004100109010000600a400044110100000010841
h |= 5 ;      h *= c;

64 bytes, 5 hashes, 63  bits, 0 coll., 2726 unique
10078d0502201820044002020200000088040800409018220805200120000040
0000200000000048004404020980c42020200040000504000004080000208800
h += h << 4 ; h += c;

With some parameters we could even achieve some sort of "compression" by
packing 90 bytes of strings from set of 36Kb into 32 bytes of filter:

32 bytes, 7 hashes, 79  bits, 0 coll., 2727 unique
8948004a208920c103023603404adeeb0c4c04024c6a504083c5a06016010c9a
h <<= 26;     h += c;

32 bytes, 7 hashes, 65  bits, 0 coll., 2723 unique
144a01d72210894445af800985001010000160070800100b04131420510019b1
h *= 9 ;      h += c;

And eventually, we get a random function that executes a random number of times
and works with a sparse array consisting of random values.

References

Matthew Brennan, Hackers No Hashing: Randomizing API Hashes to Evade Cobalt
Strike Shellcode Detection, 2024
https://www.huntress.com/blog/hackers-no-hashing-randomizing-api-hashes-to-evade-cobalt-strike-shellcode-detection

Jon Maiga The construct of a bit mixer, 2020
https://jonkagstrom.com/bit-mixer-construction/

Russ Cox, "Rotating hashes", 2008
https://research.swtch.com/hash

M. D. Mc Ilroy "A variant method of file searching", CACM, Vol 6, Issue 3, 1963
https://dl.acm.org/doi/10.1145/366274.366304

*/
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

const char *needed[] = {
	"fork",
	"open",
	"read",
	"write",
	"close",
	"mmap",
	"execve",
	"dup2",
	"malloc",
	"free",
	"connect",
	"socket",
	"bind",
	"accept",
	"listen",
	NULL
};

const char *ops[] = {
	"h += c;       ",
	"h -= c;       ",
	"h *= c;       ",
	"h ^= c;       ",
	"h |= c;       ",
	"h &= c;       ",
	[0x10] =
	"h += h << %-2d; ",
	"h -= h << %-2d; ",
	"h *= h << %-2d; ",
	"h ^= h << %-2d; ",
	"h += h >> %-2d; ",
	"h -= h >> %-2d; ",
	"h *= h >> %-2d; ",
	"h ^= h >> %-2d; ",
	"h <<= %-2d;     ",
	"h >>= %-2d;     ",
	"h += %-2d;      ",
	"h -= %-2d;      ",
	"h *= %-2d;      ",
	"h ^= %-2d;      ",
	"h |= %-2d;      ",
	"h &= %-2d;      ",
};

#define ROR(x,y) ((unsigned)(x) >> (y) | (unsigned)(x) << (32 - (y)))
#define ROL(x,y) ((unsigned)(x) << (y) | (unsigned)(x) >> (32 - (y)))

int main(int argc, char **argv)
{
	/* bloom filter parameters */
	int shift = 7;
	int nh = 11;
	int bloom_size = 256;

	/* number of hash insns */
	int size = 3;

	int i, j, k, needcount = 0;
	unsigned int prog[32];
	unsigned int args[32];
	uint32_t b[1024];

	srandom(time(NULL));

	/* read strings */
	int n = 0;
	char s[1024];
	char **list = NULL;
	FILE *f = fopen("list.txt", "r");
	if (f == NULL) {
		printf("Failed to open list.txt\n");
		return 2;
	}
	while (fgets(s, sizeof(s), f)) {
		char *p = s + strlen(s) - 1;
		while (*p == '\n' || *p == '\r') {
			*p = 0;
			--p;
		}
		list = realloc(list, (n + 1) * sizeof(char*));
		assert(list != NULL);
		list[n] = strdup(s);
		n++;
	}
	int *len = malloc(n * sizeof(int));
	assert(len != NULL);
	for (i = 0; i < n; i++)
		len[i] = strlen(list[i]);
	assert(len != NULL);
	unsigned *H = malloc(n * sizeof(int));
	assert(H != NULL);

	for (i = 0; needed[i]; i++)
		needcount++;

	for (;;) {
		/* at least one instruction should mix char into hash */
		int cp1 = random() % size;
		for (i = 0; i < size; i++) {
			if (i == cp1) {
				prog[i] = random() % 6;
			} else {
				prog[i] = 0x10 + (random() % 16);
				args[i] = (1 + random()) & 0x1f;
			}
		} // prog

		bzero(b, sizeof(b));

		for (i = 0; i < n; i++) {
			char *s = list[i];
			uint32_t h = 0;
			for (j = 0; j < len[i]; j++) {
				unsigned c = s[j];
				for (k = 0; k < size; k++)
					switch (prog[k]) {
						case 0:		h += c;			break;
						case 1: 	h -= c;			break;
						case 2: 	h *= c;			break;
						case 3: 	h ^= c;			break;
						case 4: 	h |= c;			break;
						case 5: 	h &= c;			break;

						case 0x10:	h += h << args[k];	break;
						case 0x11:	h -= h << args[k];	break;
						case 0x12:	h *= h << args[k];	break;
						case 0x13:	h ^= h << args[k];	break;
						case 0x14:	h += h >> args[k];	break;
						case 0x15:	h -= h >> args[k];	break;
						case 0x16:	h *= h >> args[k];	break;
						case 0x17:	h ^= h >> args[k];	break;
						case 0x18:	h = ROL(h, args[k]);	break;
						case 0x19:	h = ROR(h, args[k]);	break;
						case 0x1a:	h += args[k];		break;
						case 0x1b:	h -= args[k];		break;
						case 0x1c:	h *= args[k];		break;
						case 0x1d:	h ^= args[k];		break;
						case 0x1e:	h |= args[k];		break;
						case 0x1f:	h &= args[k];		break;
					}
			} // chars
			H[i] = h;

			for (j = 0; needed[j]; j++) {
				if (! strcmp(list[i], needed[j])) {
					/* add string into filter */
					for (k = 0; k < nh; k++) {
						int p = h % bloom_size;
						b[p / 32] |= 1UL << (p % 32);
						/* we can replace ROR with any other bitmixer */
						h = ROR(h, shift);
					}
				}
			}
		} // strings

		int unique = 0;
#if	0
		for (i = 0; i < n; i++) {
			int exists = 0;
			for (j = i - 1; j >= 0; j--) {
				if (H[i] == H[j]) {
					exists = 1;
					break;
				}
			}
			if (!exists) 
				unique++;
    		}
#endif
		/* check for collisions */
		int collisions = 0, count = 0;
		for (i = 0; i < n; i++) {
			uint32_t h = H[i];
			/* check filter */
			int found = 1;
			for (j = 0; j < nh; j++) {
				int p = h % bloom_size;
				found &= b[p / 32] >> (p & 31);
				h = ROR(h, shift);
			}
			if (found) {
				int match = 0;
				for (j = 0; needed[j]; j++)
					if (! strcmp(needed[j], list[i])) {
						match = 1;
						break;
					}
				if (match == 0)
					collisions++;
				else {
					count++;
				}
			}
		}

		if (collisions == 0) { // && count == needcount
#if	0
			for (i = 0; i < n; i++) {
				uint32_t h = H[i];
				/* check filter */
				int found = 1;
				for (j = 0; j < nh; j++) {
					int p = h % bloom_size;
					found &= b[p / 32] >> (p & 31);
					h = ROR(h, shift);
				}
				if (found)
					printf("%s ", list[i]);
			}
#endif
			int c = 0;
			for (i = 0; i < bloom_size / 32; i++)
				c += __builtin_popcount(b[i]);
			printf("%d bytes, %d hashes, %-3d bits, %d coll., %d unique ", bloom_size / 8, nh, c, collisions, unique);
			for (i = 0; i < bloom_size / 32; i++)
				printf("%08x", b[i]);
			printf(" ");
			for (i = 0; i < size; i++)
				printf(ops[prog[i]], args[i]);
			printf("\n");
		}
	}
}
