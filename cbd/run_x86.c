/*
 * run_x86.c — native x86-64 executor for Meta-C binaries
 *
 * Same memory layout as run.c emulator:
 *   0x0000000 .. 0x1FFFFFF  32MB  guard (PROT_NONE)
 *   0x2000000 .. 0x3FFFFFF  32MB  code (RWX)
 *   0x4000000 .. 0x5FFFFFF  32MB  BSS  (RW)
 *   0x6000000 .. 0x7FFFFFF  32MB  input (RW)
 *   0x8000000 .. 0x9FFFFFF  32MB  output (RW)
 *   0xA000000 .. 0xBFFFFFF  32MB  stack (RW, grows down from 0xC000000)
 *
 * Debug port: write to 0xFFFF0000 trapped via SIGSEGV.
 *   rdi = code (< 0x1000 = fatal, >= 0x1000 = warning)
 *   rsi = value
 *
 * Usage: ./run_x86 code.bin [< input] [> output]
 *        ./run_x86 code.bin -T   (not supported — use run_emu)
 *
 * Build: gcc -o run_x86 run_x86.c  (Linux x86-64 only)
 * Requires: mmap at fixed addresses, executable stack/heap
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <setjmp.h>
#include <ucontext.h>

#define REGION_SIZE  (32 * 1024 * 1024)  /* 32MB */
#define CODE_BASE    0x2000000UL
#define BSS_BASE     0x4000000UL
#define INPUT_BASE   0x6000000UL
#define OUTPUT_BASE  0x8000000UL
#define STACK_BASE   0xA000000UL
#define STACK_TOP    0xC000000UL
#define TRAP_ADDR    0xFFFF0000UL

static sigjmp_buf trap_jmp;
static volatile int trap_caught;
static unsigned long saved_rsp;
static volatile long hlt_rax;

/* SIGSEGV handler: intercepts HLT (#GP -> SIGSEGV), trap-port writes,
 * and genuine segfaults. HLT is the normal exit path on Linux. */
static void sigsegv_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	unsigned long addr = (unsigned long)si->si_addr;
	unsigned long rdi, rsi, rip;
	unsigned char *ip;

	(void)sig;

	rdi = uc->uc_mcontext.gregs[REG_RDI];
	rsi = uc->uc_mcontext.gregs[REG_RSI];
	rip = uc->uc_mcontext.gregs[REG_RIP];

	/* HLT (F4) in user mode triggers #GP -> SIGSEGV with si_addr=0.
	 * Distinguish from real null-deref by checking the instruction byte. */
	if (addr == 0 && rip >= CODE_BASE && rip < CODE_BASE + REGION_SIZE) {
		ip = (unsigned char *)rip;
		if (ip[0] == 0xf4) {
			/* HLT — normal termination */
			hlt_rax = (long)uc->uc_mcontext.gregs[REG_RAX];
			siglongjmp(trap_jmp, 2);
		}
	}
	if (addr >= TRAP_ADDR && addr < TRAP_ADDR + 4096) {
		fprintf(stderr, "TRAP %04lx val=%lu (0x%lx)\n", rdi, rsi, rsi);
		if (rdi < 0x1000) {
			trap_caught = 1;
			siglongjmp(trap_jmp, 1);
		}
		/* warning trap — skip the faulting MOV byte store (2 bytes: 88 xx) */
		uc->uc_mcontext.gregs[REG_RIP] = rip + 2;
		return;
	}

	fprintf(stderr, "SEGFAULT at addr=%lx rip=%lx\n", addr, rip);
	fprintf(stderr, "  rax=%lx rbx=%lx rcx=%lx rdx=%lx\n",
		(unsigned long)uc->uc_mcontext.gregs[REG_RAX],
		(unsigned long)uc->uc_mcontext.gregs[REG_RBX],
		(unsigned long)uc->uc_mcontext.gregs[REG_RCX],
		(unsigned long)uc->uc_mcontext.gregs[REG_RDX]);
	fprintf(stderr, "  rsi=%lx rdi=%lx rbp=%lx rsp=%lx\n",
		(unsigned long)uc->uc_mcontext.gregs[REG_RSI],
		(unsigned long)uc->uc_mcontext.gregs[REG_RDI],
		(unsigned long)uc->uc_mcontext.gregs[REG_RBP],
		(unsigned long)uc->uc_mcontext.gregs[REG_RSP]);
	_exit(1);
}

/* mmap a 32MB region at a fixed virtual address.
 * Falls back to MAP_FIXED (clobber) if MAP_FIXED_NOREPLACE unavailable. */
static void *map_region(unsigned long base, int prot)
{
	void *p = mmap((void *)base, REGION_SIZE,
		prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		-1, 0);
	if (p == MAP_FAILED) {
		/* try without NOREPLACE for older kernels */
		p = mmap((void *)base, REGION_SIZE,
			prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			-1, 0);
	}
	if (p == MAP_FAILED) {
		perror("mmap");
		fprintf(stderr, "  failed to map region at 0x%lx\n", base);
		exit(1);
	}
	return p;
}

/* SIGILL handler — kept as safety net in case a kernel delivers SIGILL
 * instead of SIGSEGV for HLT.  In practice, Linux sends SIGSEGV. */
static void sigill_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	(void)sig;
	hlt_rax = (long)uc->uc_mcontext.gregs[REG_RAX];
	/* restore host stack and longjmp */
	uc->uc_mcontext.gregs[REG_RSP] = saved_rsp;
	siglongjmp(trap_jmp, 2);
}

/* jump to the entry stub (E8 rel32 F4) at code_base.
 * Switches to the binary's stack, zeros all GPRs except args, then
 * uses RET to transfer control.  After main returns, HLT fires a
 * signal; the handler captures rax and longjmps back to main(). */
static void __attribute__((noinline)) run_binary(
	long code_base_addr, long input, long in_len, long output)
{
	__asm__ volatile (
		/* stash code_base + args in scratch regs */
		"mov %[fn], %%rax\n\t"
		"mov %[inp], %%r8\n\t"
		"mov %[len], %%r9\n\t"
		"mov %[out], %%r10\n\t"
		/* save callee-saved on HOST stack, then switch */
		"push %%rbx\n\t"
		"push %%r12\n\t"
		"push %%r13\n\t"
		"push %%r14\n\t"
		"push %%r15\n\t"
		"push %%rbp\n\t"
		"mov %%rsp, %[save]\n\t"    /* save host stack */
		"mov %[stk], %%rsp\n\t"     /* switch to binary stack */
		/* load args into ABI regs */
		"mov %%r8, %%rdi\n\t"
		"mov %%r9, %%rsi\n\t"
		"mov %%r10, %%rdx\n\t"
		/* push code_base on binary stack, then zero ALL regs */
		"push %%rax\n\t"            /* [rsp] = code_base */
		"xor %%rax, %%rax\n\t"
		"xor %%rcx, %%rcx\n\t"
		"xor %%r8, %%r8\n\t"
		"xor %%r9, %%r9\n\t"
		"xor %%r10, %%r10\n\t"
		"xor %%r11, %%r11\n\t"
		"xor %%rbx, %%rbx\n\t"
		"xor %%r12, %%r12\n\t"
		"xor %%r13, %%r13\n\t"
		"xor %%r14, %%r14\n\t"
		"xor %%r15, %%r15\n\t"
		"xor %%rbp, %%rbp\n\t"
		/* pop code_base into RIP via RET. All regs = 0 except
		 * rdi/rsi/rdx (args). RSP = STACK_TOP after pop.
		 * Entry stub E8 will push ret addr → RSP = STACK_TOP - 8.
		 * Matches emulator exactly. */
		"ret\n\t"
		: [save] "=m" (saved_rsp)
		: [stk] "r" ((long)STACK_TOP),
		  [inp] "r" (input),
		  [len] "r" (in_len),
		  [out] "r" (output),
		  [fn]  "r" (code_base_addr)
		: "memory", "rdi", "rsi", "rdx", "rcx", "rax",
		  "r8", "r9", "r10", "r11", "cc"
	);
	/* NOT REACHED — HLT handler does siglongjmp */
}

int main(int argc, char **argv)
{
	FILE *f;
	long code_len, in_len, out_len;
	unsigned char *code, *bss, *input, *output;

	if (argc < 2) {
		fprintf(stderr, "usage: %s code.bin [< input] [> output]\n", argv[0]);
		return 1;
	}

	/* map regions — guard region at 0 is omitted; kernel prevents mmap at NULL */
	code   = map_region(CODE_BASE, PROT_READ | PROT_WRITE | PROT_EXEC);
	bss    = map_region(BSS_BASE, PROT_READ | PROT_WRITE);
	input  = map_region(INPUT_BASE, PROT_READ | PROT_WRITE);
	output = map_region(OUTPUT_BASE, PROT_READ | PROT_WRITE);
	map_region(STACK_BASE, PROT_READ | PROT_WRITE);

	/* load code */
	f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	code_len = ftell(f);
	fseek(f, 0, SEEK_SET);
	fread(code, 1, code_len, f);
	fclose(f);
	fprintf(stderr, "code: %ld bytes\n", code_len);

	/* read input from stdin */
	in_len = fread(input, 1, REGION_SIZE, stdin);
	fprintf(stderr, "in=%ld bytes\n", in_len);

	/* clear BSS */
	memset(bss, 0, REGION_SIZE);

	/* setup signal handlers */
	{
		struct sigaction sa_segv, sa_ill;

		memset(&sa_segv, 0, sizeof(sa_segv));
		sa_segv.sa_sigaction = sigsegv_handler;
		sa_segv.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa_segv, NULL);

		memset(&sa_ill, 0, sizeof(sa_ill));
		sa_ill.sa_sigaction = sigill_handler;
		sa_ill.sa_flags = SA_SIGINFO;
		sigaction(SIGILL, &sa_ill, NULL);
	}

	trap_caught = 0;
	hlt_rax = 0;

	{
		int jmp_val = sigsetjmp(trap_jmp, 1);
		if (jmp_val == 0) {
			/* jump to entry stub at CODE_BASE (E8 <rel32> F4).
			 * Entry stub CALLs main, main returns, HLT fires SIGILL,
			 * handler captures rax and longjmps back with jmp_val=2. */
#ifdef FFI
			/* FFI: write host .text address into first 8 bytes of input.
			 * Meta-C code reads *(long *)in to find the host ELF. */
			*(long *)input = (long)&run_binary;
			if (in_len < 8) in_len = 8;
#endif
			run_binary((long)CODE_BASE, (long)INPUT_BASE, in_len, (long)OUTPUT_BASE);
			/* NOT REACHED */
			out_len = 0;
		} else if (jmp_val == 2) {
			/* normal HLT — main returned, rax captured */
			out_len = hlt_rax;
		} else {
			/* fatal trap */
			fprintf(stderr, "fatal trap, aborting\n");
			return 1;
		}
	}

	fprintf(stderr, "output: %ld bytes\n", out_len);
	if (out_len > 0 && out_len <= REGION_SIZE)
		fwrite(output, 1, out_len, stdout);

	return 0;
}
