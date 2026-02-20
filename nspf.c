/* https://herm1tvx.blogspot.com/2026/02/not-so-perfect-hash.html */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#define	MAX_KEYS	8192
#define	LOAD_FACTOR	4
#define	MAX_BUCKETS	(MAX_KEYS / LOAD_FACTOR)
#define	MAX_RETRIES	4096

uint32_t bt[MAX_BUCKETS][32];
uint32_t bl[MAX_BUCKETS];
uint32_t na, nb, nc, h1, h2;

static uint32_t hash(const char *s, uint32_t m)
{
	uint32_t h = 1;
	while (*s)
		h = (h ^ (uint8_t)*s++) * m;
	return h;
}

void addkey(char *key, int val, int c, uint32_t m)
{
	uint32_t b = hash(key, m) % nb;
	if (bl[b] == 0 && c == 0)
		return;
	for (int i = 0; i < bl[b]; i++)
		if (bt[b][i] == val)
			return;
	bt[b][bl[b]++] = val; 
	assert(bl[b] < 32);
}

void free_array(char **a, int n)
{
	for (int i = 0; i < n; i++)
		free(a[i]);
	free(a);
}

int main(int argc, char **argv)
{
	char *need[] = {
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
		"wait",
		NULL
	};
	int need_idx[32];
	memset(need_idx, -1, sizeof(need_idx));

	/* read keys */
	char **keys = NULL, s[1024];
	FILE *f = fopen("list.txt", "r");
	assert(f != NULL);
	while (fgets(s, sizeof(s), f)) {
		int l = strlen(s);
		if (l < 2)
			continue;
		if (s[l - 1] == '\n')
			s[l - 1] = 0;
		keys = (char**)realloc(keys, (na + 1) * sizeof(char*));
		keys[na] = strdup(s);
		/* save indices of needed */
		for (nc = 0; need[nc]; nc++)
			if (! strcmp(s, need[nc]))
				need_idx[nc] = na;
		na++;
	}
	fclose(f);
	for (int i = 0; i < nc; i++)
		assert(need_idx[i] != -1);
	nb = na / LOAD_FACTOR;
	printf("Read %d keys, %d buckets, %d needed\n", na, nb, nc);

	srandom(time(NULL));

restart1:
	bzero(bt, sizeof(bt));
	bzero(bl, sizeof(bl));
	h1 = random() | 1;
	/* create buckets */
	for (int i = 0; i < nc; i++)
		addkey(need[i], need_idx[i], 1, h1);
	/* add only to existing buckets */
	for (int i = 0; i < na; i++)
		addkey(keys[i], i, 0, h1);

	/* delta-encode filter, keep it in byte range and save filtered keys */
	char **fk = NULL;
	uint32_t nf = 0, prev = 0, md = 255, cf[nc], fl = 0;
	for (int i = 0; i < nb; i++) {
		if (bl[i] == 0)
			continue;
		int d = i - prev;
		if (d > md) {
			free_array(fk, nf);
			printf("Max distance exceeded, restarting\n");
			goto restart1;
		}
		cf[fl++] = d;
		prev = i;
		printf("%d: ", i);
		for (int j = 0; j < bl[i]; j++) {
			printf("%s, ", keys[bt[i][j]]);
			fk = realloc(fk, (nf + 1) * sizeof(char*));
			fk[nf++] = strdup(keys[bt[i][j]]);
		}
		puts("");
	}
	printf("%d keys in %d buckets\n", nf, fl);

	/* bruteforce byte-sized perfect hash */
	int retries = 0;
	uint32_t mo = 0, mm = 256;
	char used[mm], col;
	assert(nf < mm);
restart2:
	h2 = random() | 1;
	for (int i = nf; i < mm; i++) {
		memset(used, 0, sizeof(used));
		col = 0;
		for (int j = 0; j < nf; j++) {
			uint32_t h = hash(fk[j], h2) % i;
			if (used[h] != 0) {
				col = 1;
				break;
			}
			used[h] = 1;
		}
		if (col == 0) {
			mo = i;
			break;
		}
	}
	if (mo == 0) {
		retries++;
		if (retries > MAX_RETRIES) {
			free_array(fk, nf);
			printf("Restarting from scratch\n");
			goto restart1;
		}
		goto restart2;
	}
	printf("Found mod = %d after %d attempts\n", mo, retries);

	printf("Filter:\n");
	for (int i = 0; i < fl; i++)
		printf("%d, ", cf[i]);
	puts("");

	printf("Unpacking...\n");
	uint8_t filter[nb];
	bzero(filter, sizeof(filter));
	prev = 0;
	for (int i = 0; i < fl; i++) {
		int pos = prev + cf[i];
		prev = pos;
		printf("%d, ", pos);
		filter[pos] = 1;
	}
	puts("");

	printf("Checking...\n");
	uint32_t need_hash[nc];
	for (int j = 0; j < nc; j++)
		need_hash[j] = hash(need[j], h2) % mo;
	for (int i = 0; i < na; i++) {
		unsigned b = hash(keys[i], h1) % nb;
		if (filter[b] == 0)
			continue;
		unsigned h = hash(keys[i], h2) % mo;
		for (int j = 0; j < nc; j++)
			if (h == need_hash[j]) {
				printf("%d %s\n", h, keys[i]);
				assert(strcmp(need[j], keys[i]) == 0);
			}
	}
	printf("h1 = %08x, h2 = %08x\n", h1, h2);
	free_array(keys, na);
	free_array(fk, nf);
	return 0;
}
