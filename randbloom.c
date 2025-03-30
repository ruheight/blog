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
