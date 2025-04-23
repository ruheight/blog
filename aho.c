/* https://herm1tvx.blogspot.com/2025/04/xz-backdoor-strings-tries-and-automata.html */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <link.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <time.h>

#define	MAX_CHARS	128

typedef struct ac_node
{
	char level, index, term;
	struct ac_node *next, *fail, *child[MAX_CHARS];
} ac_node;

void ac_insert(ac_node *root, const unsigned char *word, const int index)
{
	ac_node *cur = root;
	int i = 0;
	char c;
	do {
		c = word[i++];
		if (cur->child[c] == NULL)
			cur->child[c] = calloc(1, sizeof(ac_node));
	        cur = cur->child[c];
	/* insert string with trailing zero */
	} while (c != 0);
	cur->term = 1;
	cur->index = index;
	cur->level = i - 1;
}

void ac_build(ac_node *root)
{
	struct ac_node *cur = root, *rear = root;
	do {
		for (int i = 0; i < MAX_CHARS; i++) {
			ac_node *child = cur->child[i];
			if (child == NULL)
				continue;
			if (cur == root) {
				child->fail = root;
			} else {
				ac_node *fail = cur->fail;
				while (fail && !fail->child[i])
					fail = fail->fail;
				child->fail = fail ? fail->child[i] : root;
			}
			rear->next = child;
			rear = child;
		}
		cur = cur->next;
	} while (cur != NULL);
}

/* null-terminated dicr */
ac_node *ac_create(char **dict)
{
	ac_node *root = calloc(1, sizeof(ac_node));
	for (int i = 0; dict[i]; i++)
        	ac_insert(root, dict[i], i + 1);
	ac_build(root);
	return root;
}

void ac_find(ac_node *root, char *text, int len, void (*match)(int, int))
{
	ac_node *cur = root;
	for (int i = 0; i < len; i++) {
		int c = text[i];
		if (cur->child[c] && !cur->child[c]->term) {
			cur = cur->child[c];
			continue;
		}
		while (cur && !cur->child[c])
			cur = cur->fail;
		if (cur) {
			cur = cur->child[c];
			ac_node *temp = cur;
			while (temp->term) {
				match(i - temp->level, temp->index);
				temp = temp->fail;
			}
		} else
			cur = root;
	}
}

char *needed[] = {
	"open",
	"read",
	"write",
	"close",
	"mmap",
	"munmap",
	"fork",
	"exit",
	"connect",
	"socket",
	"accept",
	"bind",
	"listen",
	"execve",
	"malloc",
	"free",
	"realloc",
	"getenv",
	"setenv",
	NULL
};

int main(int argc, char **argv)
{
	/* get libc link_map */
	struct link_map *lm = _r_debug.r_map, *libc_map = NULL;
	for ( ; lm; lm = lm->l_next) {
		if (lm->l_name && strstr(lm->l_name, "libc.")) {
			libc_map = lm;
                        break;
		}
	}
	assert(libc_map != NULL);

	/* get symtab from dynamic */
	char *dynstr = NULL;
	Elf64_Sym *dynsym = NULL;
	Elf64_Dyn *dyn = libc_map->l_ld;
	int strsz = 0;
	for (int  i = 0; dyn[i].d_tag != DT_NULL; i++) {
		if (dyn[i].d_tag == DT_STRTAB)
			dynstr = (void*)dyn[i].d_un.d_ptr;
		if (dyn[i].d_tag == DT_SYMTAB)
			dynsym = (void*)dyn[i].d_un.d_ptr;
		if (dyn[i].d_tag == DT_STRSZ)
			strsz = dyn[i].d_un.d_val;
	}
	assert(dynsym != NULL && dynstr != NULL && strsz != 0);
	int nsyms = ((char*)dynstr - (char*)dynsym) / sizeof(Elf64_Sym);

	int count = 0;
	while (needed[count])
		count++;
	Elf64_Addr res[count];

	printf("strcmp\t");
	clock_t begin = clock();
	for (int x = 0; x < 1000; x++) {
		bzero(res, sizeof(res));
		for (int i = 0; i < nsyms; i++) {
			char *name = dynsym[i].st_name + dynstr;
			for (int j = 0; j < count; j++) {
				if (res[j] != 0)
					continue;
				if (! strcmp(name, needed[j]))
					res[j] = dynsym[i].st_value;
			}
		}
	}
	clock_t end = clock();
	printf("%f (sec)\n", (double)(end - begin) / CLOCKS_PER_SEC / 1000.0);
	for (int i = 0; i < count; i++)
		if (res[i] == 0)
			printf("failed on %s\n", needed[i]);

	char sparse[strsz];
	bzero(sparse, sizeof sparse);
	/* we could unserialize/uncompress pre-built trie here */
	ac_node *root = ac_create(needed);
	bzero(res, sizeof(res));

	/* this slower than hash-based search, but we need no plain strings */
	printf("ACA\t");
	void match(int offset, int index) {
		sparse[offset] = index;
	}
	begin = clock();
	for (int x = 0; x < 1000; x++) {
		/* match all at once */
		bzero(res, sizeof(res));
		ac_find(root, dynstr, strsz, match);
		for (int i = 0; i < nsyms; i++) {
			int name = dynsym[i].st_name;
			if (sparse[name])
				res[sparse[name] - 1] = dynsym[i].st_value;
		}
	}
	end = clock();
	printf("%f (sec)\n", (double)(end - begin) / CLOCKS_PER_SEC / 1000.0);
	for (int i = 0; i < count; i++)
		if (res[i] == 0)
			printf("failed on %s\n", needed[i]);
}
