/* ffi_elf.h — minimal ELF64 structs + symbol resolver for FFI demos
 *
 * Packed structs match x86-64 ELF layout (no alignment padding needed
 * because ELF64 fields are naturally aligned on x86-64). */

struct elf_ehdr {
	char e_ident[16];
	short e_type;
	short e_machine;
	int e_version;
	long e_entry;
	long e_phoff;
	long e_shoff;
	int e_flags;
	short e_ehsize;
	short e_phentsize;
	short e_phnum;
	short e_shentsize;
	short e_shnum;
	short e_shstrndx;
};

struct elf_phdr {
	int p_type;
	int p_flags;
	long p_offset;
	long p_vaddr;
	long p_paddr;
	long p_filesz;
	long p_memsz;
	long p_align;
};

struct elf_dyn {
	long d_tag;
	long d_val;
};

struct elf_sym {
	int st_name;
	char st_info;
	char st_other;
	short st_shndx;
	long st_value;
	long st_size;
};

struct link_map {
	long l_addr;
	long l_name;
	long l_ld;
	long l_next;
};

struct r_debug {
	int r_version;
	int _pad;
	long r_map;
};

/* --- string helpers --- */

long str_eq(long a, long b)
{
	while (*(unsigned char *)a != 0) {
		if (*(unsigned char *)a != *(unsigned char *)b)
			return 0;
		a = a + 1;
		b = b + 1;
	}
	return *(unsigned char *)b == 0;
}

long str_contains(long hay, long needle)
{
	long j, nlen;
	nlen = 0;
	while (*(unsigned char *)(needle + nlen) != 0)
		nlen = nlen + 1;
	while (*(unsigned char *)hay != 0) {
		j = 0;
		while (j < nlen && *(unsigned char *)(hay + j) == *(unsigned char *)(needle + j))
			j = j + 1;
		if (j == nlen)
			return 1;
		hay = hay + 1;
	}
	return 0;
}

/* --- ELF resolver --- */

/* scan backwards from addr (page-aligned) for ELF magic */
long find_elf_base(long addr)
{
	long p;
	struct elf_ehdr *eh;
	p = addr & ~4095;
	while (p > 0) {
		eh = (struct elf_ehdr *)p;
		if (eh->e_ident[0] == 127 && eh->e_ident[1] == 69 &&
		    eh->e_ident[2] == 76 && eh->e_ident[3] == 70)
			return p;
		p = p - 4096;
	}
	return 0;
}

/* find PT_DYNAMIC program header, return its virtual address */
long find_dynamic(long base)
{
	struct elf_ehdr *eh;
	struct elf_phdr *ph;
	long i;
	eh = (struct elf_ehdr *)base;
	ph = (struct elf_phdr *)(base + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == 2)
			return base + ph[i].p_vaddr;
	}
	return 0;
}

/* DT_DEBUG -> r_debug -> first link_map */
long find_link_map(long dyn_addr)
{
	struct elf_dyn *d;
	struct r_debug *rd;
	d = (struct elf_dyn *)dyn_addr;
	while (d->d_tag != 0) {
		if (d->d_tag == 21) {
			rd = (struct r_debug *)d->d_val;
			return rd->r_map;
		}
		d = (struct elf_dyn *)((long)d + 16);
	}
	return 0;
}

/* walk link_map chain, find entry whose l_name contains needle */
long find_lib(long lm_addr, long needle)
{
	struct link_map *lm;
	lm = (struct link_map *)lm_addr;
	while ((long)lm != 0) {
		if (lm->l_name != 0 && *(unsigned char *)lm->l_name != 0) {
			if (str_contains(lm->l_name, needle))
				return (long)lm;
		}
		lm = (struct link_map *)lm->l_next;
	}
	return 0;
}

/* resolve symbol by name from a link_map via DT_HASH (SYSV) */
long resolve_sym(long lm_addr, long name)
{
	struct link_map *lm;
	struct elf_dyn *d;
	struct elf_sym *sym;
	long symtab, strtab, hashtab, nchain, i;
	lm = (struct link_map *)lm_addr;
	symtab = 0; strtab = 0; hashtab = 0;
	d = (struct elf_dyn *)lm->l_ld;
	while (d->d_tag != 0) {
		if (d->d_tag == 6) symtab = d->d_val;
		if (d->d_tag == 5) strtab = d->d_val;
		if (d->d_tag == 4) hashtab = d->d_val;
		d = (struct elf_dyn *)((long)d + 16);
	}
	if (symtab == 0 || strtab == 0 || hashtab == 0) return 0;
	nchain = (long)(*(int *)(hashtab + 4));
	for (i = 0; i < nchain; i++) {
		sym = (struct elf_sym *)(symtab + i * 24);
		if (sym->st_name != 0) {
			if (str_eq(strtab + (long)sym->st_name, name))
				return lm->l_addr + sym->st_value;
		}
	}
	return 0;
}

/* resolve libc + one symbol by name, return function pointer (0 on fail) */
long ffi_resolve(long host_text, long sym_name)
{
	long base, dyn, lm, libc_lm;
	char s_libc[] = "libc";
	base = find_elf_base(host_text);
	if (base == 0) return 0;
	dyn = find_dynamic(base);
	if (dyn == 0) return 0;
	lm = find_link_map(dyn);
	if (lm == 0) return 0;
	libc_lm = find_lib(lm, (long)&s_libc);
	if (libc_lm == 0) return 0;
	return resolve_sym(libc_lm, sym_name);
}
