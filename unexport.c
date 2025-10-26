/* Searching for non-exported symbols
https://herm1tvx.blogspot.com/2025/10/searching-for-non-exported-symbols.html
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <link.h>
#include <elf.h>
#include <assert.h>
#include <string.h>

#include "hde64.h"

int __attribute__ ((noinline)) check_password(char *pass)
{
	printf("Enter the password:");
	return 0;
}

int main(int argc, char **argv)
{
	/* get base address from link_map */
	struct link_map *lm = _r_debug.r_map;
	uint8_t *map = NULL;
	for ( ; lm != NULL; lm = lm->l_prev) {
		printf("LM %s %p\n", lm->l_name, lm->l_ld);
		if (*lm->l_name == 0) {
			map = (void*)lm->l_addr;
			break;
		}
	}
	printf("base address %p\n", map);

	/* find rodata segment */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)map;
	Elf64_Phdr *phdr = (Elf64_Phdr*)(map + ehdr->e_phoff);
	Elf64_Addr data_addr = 0;
	unsigned data_size;
	/* we assume four-segment layout */
	for (int i = 0; i < ehdr->e_phnum; i++)
		if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags == PF_R)) {
			data_addr = phdr[i].p_vaddr;
			data_size = phdr[i].p_filesz;
		}
	assert(data_size != 0);
	printf("rodata @ %lx (%d)\n", data_addr, data_size);

	/* find text */
	Elf64_Dyn *dyn = NULL;
	uintptr_t init = 0, fini = 0;
	for (dyn = (Elf64_Dyn*)_DYNAMIC; dyn->d_tag != DT_NULL; ++dyn) {
		if (dyn->d_tag == DT_INIT)
			init = dyn->d_un.d_val;
		if (dyn->d_tag == DT_FINI)
			fini = dyn->d_un.d_val;
	}
	assert(init != 0 && fini != 0);
	printf("init %lx fini %lx\n", init, fini);

	/* disassemble */
	int len;
	struct code {
		unsigned flags, imm, off, is_func, len, opcode, visited, modrm, disp;
		struct code *link, *next, *func;
	} *code = NULL, *tail = NULL, *c, *fast[fini];
	bzero(fast, sizeof(fast));
	for (Elf64_Addr i = init; i < fini; i += len) {
		hde64s hs;
		len = hde64_disasm(map + i, &hs);
		assert(len > 0);
		c = malloc(sizeof(struct code));
		assert(c != NULL);
		c->opcode = hs.opcode;
		c->off = i;
		c->len = len;
		c->flags = hs.flags;
		c->imm = hs.imm.imm32;
		c->disp = hs.disp.disp32;
		c->modrm = hs.modrm;
		if (code == NULL) {
			code = c;
			tail = c;
		} else {
			tail->next = c;
			tail = c;
		}
		fast[i] = c;
	}

	/* link calls/jmps, mark functions */
	for (c = code; c; c = c->next)
		if (c->flags & F_RELATIVE) {
			Elf64_Addr jmp;
			if (c->flags & F_IMM32)
				jmp = c->off + c->len + (int32_t)c->imm;
			else
				jmp = c->off + c->len + (char)c->imm;
			if (jmp > init && jmp < fini) {
				/* let it just fail on PLTs */
				if (fast[jmp] == NULL)
					printf("link error @ %x -> %lx\n", c->off, jmp);
				else
					c->link = fast[jmp];
				if (c->opcode == 0xe8 && c->link)
					c->link->is_func = 1;
			}
		}

	/* make functions list */
	struct code *func = NULL;
	for (c = code; c; c = c->next)
		if (c->is_func) {
			printf("func @ %d\n", c->off);
			if (func == NULL) {
				func = c;
				tail = c;
			} else {
				tail->func = c;
				tail = c;
			}
		}

	/* find string in rodata segment */
	char needle[32];
	strcpy(needle, "enter the password:");
	needle[0] = 'E';
	char *p = memmem(map + data_addr, data_size, needle, strlen(needle));
	assert(p != NULL);
	Elf64_Addr str = p - (char*)map;
	printf("Found needle @ %lx\n", str);

	/* traverse CFG */
	int traverse(struct code *code, int (*cb)(struct code *c, void *arg), void *arg) {
		struct code *c = code;
		int r = 0, s;
		/* until RET */
		while (c && c->opcode != 0xc3) {
			/* prevent loops */
			if (c->visited)
				return r;
			c->visited = 1;
			s = cb(c, arg);
			if (s)
				r = s;
			/* don't recurse into functions */
			if (c->link && c->opcode != 0xe8) {
				if (c->opcode == 0xe9 || c->opcode == 0xeb) {
					c = c->link;
					continue;
				} else {
					/* Jcc */
					int s = traverse(c->link, cb, arg);
					if (s)
						r = s;
				}
			}
			c = c->next;
		}
		return r;
	}
	int cb(struct code *c, void *arg) {
		Elf64_Addr sa = *(Elf64_Addr*)arg;

		/* lea rip-relative, in real code one should check for other insn too */
		if (c->opcode == 0x8d && (c->flags & F_MODRM) && (c->modrm & 0xc7) == 5) {
			Elf64_Addr da = c->off + c->len + (int)c->disp;
			if (sa == da) {
				printf("%x LEA\n", c->off);
				return 1;
			}
		}
		return 0;
	}

	/* for each function... */
	for (c = func; c; c = c->func) {
		printf("checking %x...\n", c->off);
		if (traverse(c, cb, &str) == 1)
			break;
	}
	assert(c != NULL);

	check_password(NULL);
	puts("");

	assert((char*)check_password - (char*)map == c->off);
	printf("OK, found it @ %x\n", c->off);
}
