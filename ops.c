/* https://herm1tvx.blogspot.com/2025/04/xz-backdoor-hiding-key-in-opcodes.html */
/* x86 only */
#include <stdio.h>
#include <stdint.h>
#include <elf.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "hde64.h"

int main(int argc, char **argv)
{
	int h = open(argv[0], 0);
	int l = lseek(h, 0, 2);
	if (h < 0 || l < 0)
		return 2;
	uint8_t m[l];
	lseek(h, 0, 0);
	if (read(h, m , l) != l)
		return 2;
	close(h);

	/* boring ELF stuff to locate .text */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)m;
	Elf64_Phdr *phdr = (Elf64_Phdr*)(m + ehdr->e_phoff);
	Elf64_Dyn *dyn = NULL;
	Elf64_Addr d = 0;
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type == PT_DYNAMIC)
			dyn = (Elf64_Dyn*)(m + phdr[i].p_offset);
		if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X))
			d = phdr[i].p_vaddr - phdr[i].p_offset;
	}
	assert(dyn != NULL);
	Elf64_Addr init = 0, fini = 0;
	for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
		if (dyn[i].d_tag == DT_INIT)
			init = dyn[i].d_un.d_val - d;
		if (dyn[i].d_tag == DT_FINI)
			fini = dyn[i].d_un.d_val - d;
	}
	assert(init > 0 && fini > init);

	/* disassemble and patch/read */
	int len = 0, count = 0;
	unsigned char buf[1024];
	bzero(buf, sizeof buf);
	for (Elf64_Addr i = init; i < fini; i += len) {
		hde64s hs;
		len = hde64_disasm(m + i, &hs);

		/* reg/reg 2-byte opcodes */
		if (hs.modrm_mod != 3 || len != 2)
			continue;

		/* ALU and MOV */
		if ((m[i] & 0xC4) == 0x00 || (m[i] & 0xFC) == 0x88) {
			unsigned b = (m[i] >> 1) & 1;
			if (argc > 1) {
				/* set bit in insn */
				unsigned w = (argv[1][count / 8] >> (count % 8)) & 1;
				if (w != b) {
					/* toggle D-bit, swap reg and r/m */
					m[i] ^= 2;
					m[i + 1] = 0xc0 | (hs.modrm_rm << 3) | hs.modrm_reg;
				}
			} else {
				/* read bit from insn */
				buf[count / 8] |= b << (count % 8);
			}
			count++;
		}
	}
	printf("%d bits (%d bytes)\n", count, count / 8);

	/* save copy and run or print the result */
	if (argc > 1) {
		unlink("read");
		h = open("read", O_CREAT|O_WRONLY, 0777);
		write(h, m, l);
		close(h);
		printf("Running modified executable\n");
		system("./read");
	}
	if (strstr(argv[0], "read") != NULL)
		printf("'%s'\n", buf);
}
