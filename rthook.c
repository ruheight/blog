/* https://herm1tvx.blogspot.com/2025/10/runtime-hooks.html */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>
#include <assert.h>
#include <dlfcn.h>
#include <link.h>

static Elf64_Sym *lookup(void *base, const char *name)
{
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
	Elf64_Phdr *phdr = (Elf64_Phdr *)(base + ehdr->e_phoff);
	static Elf64_Dyn *dyn = NULL;
	static Elf64_Sym *symtab = NULL;
	static char *strtab = NULL;
	for (int i = 0; i < ehdr->e_phnum; i++)
		if (phdr[i].p_type == PT_DYNAMIC) {
			dyn = (Elf64_Dyn *)(base + phdr[i].p_vaddr);
			break;
		}
	assert(dyn != NULL);
	for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
		if (dyn[i].d_tag == DT_SYMTAB)
			symtab = (Elf64_Sym *)dyn[i].d_un.d_ptr;
		if (dyn[i].d_tag == DT_STRTAB)
			strtab = (char *)dyn[i].d_un.d_ptr;
	}
	assert(symtab != NULL && strtab != NULL);
	for (int i = 0; (char*)&symtab[i] < strtab; i++)
		if (strcmp(strtab + symtab[i].st_name, name) == 0) {
			printf("%s @ %lx\n", name, symtab[i].st_value);
			return &symtab[i];
        	}
	printf("%s not found\n", name);
	return NULL;
}

static char *(*orig)() = NULL;
void crypt(void)
{
	printf("Trust me, I am crypt()\n");
	printf("calling original %s\n", orig("", "aa"));
}

typedef struct link_map *lookup_t;
static lookup_t (*orig_dl_lookup_symbol_x) (const char *undef_name, struct link_map *undef_map,
	const Elf64_Sym **ref, void *symbol_scope[], const void *version,
	int type_class, int flags, struct link_map *skip_map);

static struct link_map *lm = NULL;

static lookup_t ours_dl_lookup_symbol_x (const char *undef_name, struct link_map *undef_map,
	const Elf64_Sym **ref, void *symbol_scope[], const void *version,
	int type_class, int flags, struct link_map *skip_map)
{
	lookup_t res = orig_dl_lookup_symbol_x(undef_name, undef_map, ref,
		symbol_scope, version, type_class, flags, skip_map);
	printf("hook '%s'\n", undef_name);
	if (! strcmp(undef_name, "crypt")) {
		orig = (void*)res->l_addr + (*ref)->st_value;
		*ref = lookup((void*)lm->l_addr, "crypt");
		res = lm;
	}
	return res;
}

static void ii(void) __attribute__((__ifunc__("is")));
static void *ij = &ii;
typedef void (*it)();

static it is()
{
	printf("* ifunc\n");
	/* get our link_map */
        for (lm = _r_debug.r_map; lm != NULL; lm = lm->l_prev)
                if (*lm->l_name == 0)
                        break;
	/* get symbols */
	void *rtld = (void*)_r_debug.r_ldbase;
	void *glro = rtld + lookup(rtld, "_rtld_global_ro")->st_value;
	void *dlmc = rtld + lookup(rtld, "_dl_mcount")->st_value;
	assert(glro != NULL && dlmc != NULL);
	/* get GLRO(dl_lookup_symbol_x) */
	void **dllx = NULL;
	for (int i = 0; i < 0x400; i += 8)
		if (*(void**)(glro + i) == dlmc)  {
			printf("GLRO(dl_mcount) @ %x\n", i);
			dllx = glro + i + 8;
			break;
		}
	assert(dllx != NULL);
	/* it's still rw, hook it */
	orig_dl_lookup_symbol_x = *dllx;
	*dllx = ours_dl_lookup_symbol_x;
	return NULL;
}

static __attribute__((constructor)) void init(void)
{
	/* at this point GLRO is read only */
	printf("* constructor, it's too late\n");
}

int main(int argc, char **argv)
{
	printf("* main\n");
	void *h = dlopen("libcrypt.so", RTLD_LAZY);
	if (h != NULL) {
		printf("dlsym(crypt)\n");
		void (*s)(void) = dlsym(h, "crypt");
		s();
	}
	return 0;
}
