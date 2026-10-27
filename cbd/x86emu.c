/*
 * x86emu.c — minimal x86-64 emulator for Meta-C compiler output.
 *
 * Supports the subset emitted by c1 (all-spill stack machine):
 * - MOV, ADD, SUB, AND, OR, XOR, SHL, SHR, CMP, TEST
 * - PUSH, POP, CALL, RET, JMP, Jcc, SETcc
 * - MOVZX, MOVSX, LEA, MOVSXD
 * - NEG, NOT, MUL, IMUL, DIV, IDIV
 * - HLT (halt execution)
 *
 * Included by run_emu.c. Build with -O2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* forward decl — pop64 used before its definition in step() */
typedef struct x86emu x86emu_t;
static uint64_t pop64(x86emu_t *e);

/* registers */
enum {
	RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
	R8, R9, R10, R11, R12, R13, R14, R15,
	NREGS
};

typedef struct x86emu {
	uint64_t regs[NREGS];
	uint64_t rip;
	uint64_t rflags;	/* only ZF/SF/CF/OF tracked (real-flag bit positions) */

	uint8_t *mem;		/* flat memory */
	uint64_t mem_size;
	uint8_t *ip;		/* host pointer into mem[]; avoids mem+rip each fetch */

	uint64_t code_base;	/* virtual address of code[0] */

	int halted;
	int trace;
} x86emu_t;

/* flag bit positions — match real x86 RFLAGS layout */
#define ZF  (1ULL << 6)
#define SF  (1ULL << 7)
#define CF  (1ULL << 0)
#define OF  (1ULL << 11)

/* ModRM field extraction */
#define MODRM_MOD(m)  ((m) >> 6 & 3)
#define MODRM_REG(m)  ((m) >> 3 & 7)
#define MODRM_RM(m)   ((m) & 7)
#define REG(m, ext)   (MODRM_REG(m) | ((ext) ? 8 : 0))
#define RM(m, ext)    (MODRM_RM(m)  | ((ext) ? 8 : 0))
#define IS_REG(m)     (MODRM_MOD(m) == 3)

static void set_flags_result(x86emu_t *e, uint64_t result, int bits)
{
	uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);

	result &= mask;
	e->rflags &= ~(ZF | SF | CF | OF);
	if (result == 0) e->rflags |= ZF;
	if (result >> (bits - 1)) e->rflags |= SF;
}

static void set_flags_add(x86emu_t *e, uint64_t a, uint64_t b, uint64_t result, int bits)
{
	uint64_t sign = 1ULL << (bits - 1);
	set_flags_result(e, result, bits);
	e->rflags &= ~(CF | OF);
	/* unsigned carry: result < either operand */
	if (result < a) e->rflags |= CF;
	/* signed overflow: operands same sign, result different sign */
	if (((~(a ^ b)) & (a ^ result)) & sign)
		e->rflags |= OF;
}

static void set_flags_sub(x86emu_t *e, uint64_t a, uint64_t b, uint64_t result, int bits)
{
	set_flags_result(e, result, bits);
	e->rflags &= ~(CF | OF);
	if (a < b) e->rflags |= CF;
	/* signed overflow: signs of operands differ, sign of result != sign of a */
	{
		uint64_t sign = 1ULL << (bits - 1);
		if (((a ^ b) & sign) && ((a ^ result) & sign))
			e->rflags |= OF;
	}
}

#define GUARD_END 0x2000000ULL
#define STACK_LO  0xA000000ULL
#define STACK_HI  0xC000000ULL

#ifdef DEBUG_STACK
/* call stack frame tracker: records return address slot for each active call.
 * On CALL: push frame (retaddr_slot = RSP after push).
 * On RET:  pop frame.
 * On write64 to stack: check addr != any retaddr_slot. */
#define DBG_MAX_FRAMES 256
static uint64_t dbg_retaddr_slot[DBG_MAX_FRAMES];
static int dbg_nframes;

static void dbg_push_frame(x86emu_t *e, uint64_t retaddr_slot)
{
	if (dbg_nframes < DBG_MAX_FRAMES) {
		dbg_retaddr_slot[dbg_nframes] = retaddr_slot;
		dbg_nframes++;
	}
}

static void dbg_pop_frame(void)
{
	if (dbg_nframes > 0)
		dbg_nframes--;
}

static void dbg_check_write(x86emu_t *e, uint64_t addr, uint64_t val)
{
	int i;
	if (addr < STACK_LO || addr >= STACK_HI)
		return;
	/* check: writing to any saved return address slot? */
	for (i = 0; i < dbg_nframes; i++) {
		if (addr == dbg_retaddr_slot[i]) {
			fprintf(stderr,
				"STACK_CORRUPT: write to return addr slot "
				"frame[%d] addr=%llx val=%llx "
				"rsp=%llx rbp=%llx rip=%llx\n",
				i,
				(unsigned long long)addr,
				(unsigned long long)val,
				(unsigned long long)e->regs[RSP],
				(unsigned long long)e->regs[RBP],
				(unsigned long long)(e->ip - e->mem - e->code_base));
		}
	}
}
#endif

/* memory access — addresses below GUARD_END trap as null-pointer derefs */
static uint8_t mem_read8(x86emu_t *e, uint64_t addr)
{
	if (addr < GUARD_END) {
		fprintf(stderr, "x86emu: NULL deref read8 addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
		e->halted = 1;
		return 0;
	}
	if (addr < e->mem_size) return e->mem[addr];
	fprintf(stderr, "x86emu: read8 OOB addr=%llx rip=%llx rdi=%llx rsi=%llx\n",
		(unsigned long long)addr,
		(unsigned long long)e->rip - e->code_base,
		(unsigned long long)e->regs[RDI],
		(unsigned long long)e->regs[RSI]);
	e->halted = 1;
	return 0;
}

static uint64_t mem_read64(x86emu_t *e, uint64_t addr)
{
	uint64_t v;
	if (__builtin_expect(addr >= GUARD_END && addr + 8 <= e->mem_size, 1)) {
		memcpy(&v, e->mem + addr, 8);
		return v;
	}
	if (addr < GUARD_END)
		fprintf(stderr, "x86emu: NULL deref read64 addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
	else
		fprintf(stderr, "x86emu: read64 OOB addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
	e->halted = 1;
	return 0;
}

/* 32-bit memory read — zero-extends to uint64_t.
 * MOV r32,[mem] and similar dword loads only touch 4 bytes. */
static uint64_t mem_read32(x86emu_t *e, uint64_t addr)
{
	uint32_t v;
	if (__builtin_expect(addr >= GUARD_END && addr + 4 <= e->mem_size, 1)) {
		memcpy(&v, e->mem + addr, 4);
		return (uint64_t)v;
	}
	if (addr < GUARD_END)
		fprintf(stderr, "x86emu: NULL deref read32 addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
	else
		fprintf(stderr, "x86emu: read32 OOB addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
	e->halted = 1;
	return 0;
}

/* width-aware memory read: w=1 → 64-bit, w=0 → 32-bit zero-extended */
static uint64_t mem_read_w(x86emu_t *e, uint64_t addr, int w)
{
	if (w) return mem_read64(e, addr);
	return mem_read32(e, addr);
}

static void mem_write8(x86emu_t *e, uint64_t addr, uint8_t val)
{
	if (addr < GUARD_END) {
		fprintf(stderr, "x86emu: NULL deref write8 addr=%llx rip=%llx val=%x\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base),
			(unsigned)val);
		e->halted = 1;
		return;
	}
	if (addr < e->mem_size) { e->mem[addr] = val; return; }
	/* trap port at 0xFFFF0000: compiler emits byte-store here for diagnostics.
	 * rdi < 0x1000 = fatal error (halt). rdi >= 0x1000 = warning (continue). */
	if (addr == 0xFFFF0000) {
		fprintf(stderr, "TRAP %04llx val=%llu (0x%llx)\n",
			(unsigned long long)e->regs[RDI],
			(unsigned long long)e->regs[RSI],
			(unsigned long long)e->regs[RSI]);
		if (e->regs[RDI] < 0x1000)
			e->halted = 1;
		return;
	}
	fprintf(stderr, "x86emu: write8 OOB addr=%llx rip=%llx rdi=%llx rsi=%llx\n",
		(unsigned long long)addr,
		(unsigned long long)(e->rip - e->code_base),
		(unsigned long long)e->regs[RDI],
		(unsigned long long)e->regs[RSI]);
	e->halted = 1;
}

static void mem_write64(x86emu_t *e, uint64_t addr, uint64_t val)
{
	if (__builtin_expect(addr >= GUARD_END && addr + 8 <= e->mem_size, 1)) {
#ifdef DEBUG_STACK
		dbg_check_write(e, addr, val);
#endif
		memcpy(e->mem + addr, &val, 8);
		return;
	}
	if (addr < GUARD_END)
		fprintf(stderr, "x86emu: NULL deref write64 addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
	else
		fprintf(stderr, "x86emu: write64 OOB addr=%llx rip=%llx\n",
			(unsigned long long)addr,
			(unsigned long long)(e->rip - e->code_base));
	e->halted = 1;
}

static void push64(x86emu_t *e, uint64_t val)
{
	e->regs[RSP] -= 8;
	mem_write64(e, e->regs[RSP], val);
}

static uint64_t pop64(x86emu_t *e)
{
	uint64_t v = mem_read64(e, e->regs[RSP]);
	e->regs[RSP] += 8;
	return v;
}

/* fetch helpers — read from instruction stream via host pointer (e->ip) */
static uint8_t fetch8(x86emu_t *e)
{
	return *e->ip++;
}

static int32_t fetch32(x86emu_t *e)
{
	int32_t v;
	memcpy(&v, e->ip, 4);
	e->ip += 4;
	return v;
}

static int64_t fetch64(x86emu_t *e)
{
	int64_t v;
	memcpy(&v, e->ip, 8);
	e->ip += 8;
	return v;
}

/*
 * Resolve modrm memory address. Call AFTER reading modrm byte.
 * Returns address for mod=0,1,2; sets *ok=1.
 * For mod==3 (register): sets *ok=0, returns register index.
 */
static uint64_t resolve_modrm_addr(x86emu_t *e, uint8_t modrm, int b_ext, int *ok)
{
	int mod = MODRM_MOD(modrm);
	int rm = RM(modrm, b_ext);
	uint64_t addr = 0;
	*ok = 1;
	if (mod == 3) { *ok = 0; return rm; }
	if (mod == 0 && MODRM_RM(modrm) == 5) {
		int32_t disp = fetch32(e);
		return (uint64_t)(e->ip - e->mem) + disp;
	}
	addr = e->regs[rm];
	if (mod == 1) addr += (int8_t)fetch8(e);
	else if (mod == 2) addr += (int32_t)fetch32(e);
	return addr;
}

/*
 * Step one instruction.
 * Returns 0 on success, -1 on halt/error.
 *
 * This is intentionally minimal — only instructions the c1 compiler emits.
 */
static int x86emu_step(x86emu_t *e)
{
	uint8_t b, rex = 0;
	int w = 0, r_ext = 0, x_ext = 0, b_ext = 0;
	int osz = 0;  /* operand size override (0x66) */
	uint64_t step_rip;

	if (e->halted) return -1;
	if (__builtin_expect((uint64_t)(e->ip - e->mem) + 15 > e->mem_size, 0)) {
		e->halted = 1; return -1;
	}
	step_rip = (uint64_t)(e->ip - e->mem);
	b = fetch8(e);

	/* 0x66 operand-size override */
	if (b == 0x66) {
		osz = 1;
		b = fetch8(e);
	}

	/* REX prefix */
	if ((b & 0xf0) == 0x40) {
		rex = b;
		w = (rex >> 3) & 1;
		r_ext = (rex >> 2) & 1;
		x_ext = (rex >> 1) & 1;
		b_ext = rex & 1;
		b = fetch8(e);
	}

	switch (b) {
	/* INT3: debug trap. rdi=code, rsi=value */
	case 0xcc:
		fprintf(stderr, "TRAP at 0x%llx: code=%llu val=%llu rax=%llx\n",
			(unsigned long long)(step_rip - e->code_base),
			(unsigned long long)e->regs[RDI],
			(unsigned long long)e->regs[RSI],
			(unsigned long long)e->regs[RAX]);
		e->halted = 1;
		return -1;

	/* HLT — normal termination (halted=2 to distinguish from errors) */
	case 0xf4:
		e->halted = 2;
		return -1;

	/* LEAVE: mov rsp, rbp; pop rbp */
	case 0xc9:
		e->regs[RSP] = e->regs[RBP];
		e->regs[RBP] = pop64(e);
		break;

	/* RET */
	case 0xc3:
#ifdef DEBUG_STACK
		dbg_pop_frame();
#endif
		{ uint64_t ra = pop64(e); e->ip = e->mem + ra;
		if (ra == 0) e->halted = 1; }
		break;

	/* CQO: sign-extend rax into rdx:rax */
	case 0x99:
		if (w)
			e->regs[RDX] = ((int64_t)e->regs[RAX] < 0) ? ~0ULL : 0;
		else
			e->regs[RDX] = ((int32_t)e->regs[RAX] < 0) ? 0xFFFFFFFF : 0;
		break;

	/* F7 /opc: NEG(3), NOT(2), DIV(6), IDIV(7) */
	case 0xf7: {
		uint8_t modrm = fetch8(e);
		int rm = RM(modrm, b_ext);
		int opc = MODRM_REG(modrm);
		if (IS_REG(modrm)) {
			if (opc == 6 && w) { /* DIV r64 (unsigned) */
				__uint128_t dividend = ((__uint128_t)e->regs[RDX] << 64) | e->regs[RAX];
				uint64_t divisor = e->regs[rm];
				if (divisor) {
					e->regs[RAX] = (uint64_t)(dividend / divisor);
					e->regs[RDX] = (uint64_t)(dividend % divisor);
				}
			}
			if (opc == 7 && w) { /* IDIV r64 (signed) */
				__int128_t dividend = ((__int128_t)(int64_t)e->regs[RDX] << 64) | e->regs[RAX];
				int64_t divisor = (int64_t)e->regs[rm];
				if (divisor) {
					e->regs[RAX] = (uint64_t)((int64_t)(dividend / divisor));
					e->regs[RDX] = (uint64_t)((int64_t)(dividend % divisor));
				}
			}
			if (opc == 3 && w) { /* NEG r64 */
				uint64_t old = e->regs[rm];
				e->regs[rm] = (uint64_t)(-(int64_t)e->regs[rm]);
				set_flags_sub(e, 0, old, e->regs[rm], 64);
			}
			if (opc == 2 && w) { /* NOT r64 — no flags affected */
				e->regs[rm] = ~e->regs[rm];
			}
		}
		break;
	}

	/* NOP */
	case 0x90:
		break;

	/* PUSH r64 (50+rd) */
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
		push64(e, e->regs[(b - 0x50) + (b_ext ? 8 : 0)]);
		break;

	/* POP r64 (58+rd) */
	case 0x58: case 0x59: case 0x5a: case 0x5b:
	case 0x5c: case 0x5d: case 0x5e: case 0x5f:
		e->regs[(b - 0x58) + (b_ext ? 8 : 0)] = pop64(e);
		break;

	/* MOV r64, imm64 (B8+rd, REX.W) */
	case 0xb8: case 0xb9: case 0xba: case 0xbb:
	case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		if (w) {
			e->regs[(b - 0xb8) + (b_ext ? 8 : 0)] = fetch64(e);
		} else {
			e->regs[(b - 0xb8) + (b_ext ? 8 : 0)] = (uint32_t)fetch32(e);
		}
		break;

	/* XOR r/m, r — 31 /r ; XOR r, r/m — 33 /r */
	case 0x31: case 0x33: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		if (b == 0x31) {
			if (ok) mem_write64(e, addr, mem_read_w(e, addr, w) ^ e->regs[reg]);
			else e->regs[(int)addr] ^= e->regs[reg];
			set_flags_result(e, ok ? mem_read_w(e, addr, w) : e->regs[(int)addr], w ? 64 : 32);
		} else {
			uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
			e->regs[reg] ^= rm_val;
			set_flags_result(e, e->regs[reg], w ? 64 : 32);
		}
		break;
	}

	/* ADD r/m, r — 01 /r ; ADD r, r/m — 03 /r */
	case 0x01: case 0x03: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		if (b == 0x01) {
			if (ok) {
				uint64_t old = mem_read_w(e, addr, w);
				uint64_t res = old + e->regs[reg];
				mem_write64(e, addr, res);
				set_flags_add(e, old, e->regs[reg], res, w ? 64 : 32);
			} else {
				uint64_t old = e->regs[(int)addr];
				e->regs[(int)addr] += e->regs[reg];
				set_flags_add(e, old, e->regs[reg], e->regs[(int)addr], w ? 64 : 32);
			}
		} else {
			uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
			uint64_t old = e->regs[reg];
			e->regs[reg] += rm_val;
			set_flags_add(e, old, rm_val, e->regs[reg], w ? 64 : 32);
		}
		break;
	}

	/* TEST r/m, r — 85 /r */
	case 0x85: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int rm = RM(modrm, b_ext);
		if (IS_REG(modrm)) {
			uint64_t result = e->regs[rm] & e->regs[reg];
			set_flags_result(e, result, w ? 64 : 32);
			e->rflags &= ~CF;
		}
		break;
	}

	/* AND r/m, r — 21 /r ; AND r, r/m — 23 /r */
	case 0x21: case 0x23: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		if (b == 0x21) {
			if (ok) mem_write64(e, addr, mem_read_w(e, addr, w) & e->regs[reg]);
			else e->regs[(int)addr] &= e->regs[reg];
			set_flags_result(e, ok ? mem_read_w(e, addr, w) : e->regs[(int)addr], w ? 64 : 32);
		} else {
			uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
			e->regs[reg] &= rm_val;
			set_flags_result(e, e->regs[reg], w ? 64 : 32);
		}
		break;
	}

	/* OR r/m, r — 09 /r ; OR r, r/m — 0B /r */
	case 0x09: case 0x0b: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		if (b == 0x09) {
			if (ok) {
				uint64_t res = mem_read_w(e, addr, w) | e->regs[reg];
				mem_write64(e, addr, res);
				set_flags_result(e, res, w ? 64 : 32);
			} else {
				e->regs[(int)addr] |= e->regs[reg];
				set_flags_result(e, e->regs[(int)addr], w ? 64 : 32);
			}
		} else {
			uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
			e->regs[reg] |= rm_val;
			set_flags_result(e, e->regs[reg], w ? 64 : 32);
		}
		break;
	}

	/* SUB r/m, r — 29 /r ; SUB r, r/m — 2B /r */
	case 0x29: case 0x2b: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		if (b == 0x29) {
			if (ok) {
				uint64_t old = mem_read_w(e, addr, w);
				mem_write64(e, addr, old - e->regs[reg]);
				set_flags_sub(e, old, e->regs[reg], old - e->regs[reg], w ? 64 : 32);
			} else {
				uint64_t old = e->regs[(int)addr];
				e->regs[(int)addr] -= e->regs[reg];
				set_flags_sub(e, old, e->regs[reg], e->regs[(int)addr], w ? 64 : 32);
			}
		} else {
			uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
			uint64_t old = e->regs[reg];
			e->regs[reg] -= rm_val;
			set_flags_sub(e, old, rm_val, e->regs[reg], w ? 64 : 32);
		}
		break;
	}

	/* CMP r/m, r — 39 /r ; CMP r, r/m — 3B /r */
	case 0x39: case 0x3b: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
		uint64_t a = (b == 0x39) ? rm_val : e->regs[reg];
		uint64_t bv = (b == 0x39) ? e->regs[reg] : rm_val;
		set_flags_sub(e, a, bv, a - bv, w ? 64 : 32);
		break;
	}

	/* CMP AL, imm8 — 3C */
	case 0x3c: {
		uint8_t imm = fetch8(e);
		uint64_t al = e->regs[RAX] & 0xff;
		set_flags_sub(e, al, imm, al - imm, 8);
		break;
	}

	/* SUB AL, imm8 — 2C */
	case 0x2c: {
		uint8_t imm = fetch8(e);
		uint8_t al = e->regs[RAX] & 0xff;
		e->regs[RAX] = (e->regs[RAX] & ~0xffULL) | ((al - imm) & 0xff);
		break;
	}

	/* OR AL, r/m8 — 0A /r */
	case 0x0a: {
		uint8_t modrm = fetch8(e);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		uint8_t val = ok ? mem_read8(e, addr) : (e->regs[(int)addr] & 0xff);
		e->regs[RAX] = (e->regs[RAX] & ~0xffULL) | ((e->regs[RAX] & 0xff) | val);
		break;
	}

	/* SHL/SHR/ROL/ROR r/m8, imm8 — C0 /opc ib */
	case 0xc0: {
		uint8_t modrm = fetch8(e);
		int opc = MODRM_REG(modrm);
		int rm = RM(modrm, b_ext);
		uint8_t imm = fetch8(e);
		if (IS_REG(modrm)) {
			uint8_t val = e->regs[rm] & 0xff;
			if (opc == 4) val <<= imm; /* SHL */
			else if (opc == 5) val >>= imm; /* SHR */
			e->regs[rm] = (e->regs[rm] & ~0xffULL) | val;
		}
		break;
	}

	/* SHL/SHR r/m64, imm8 — C1 /4 ib, C1 /5 ib */
	case 0xc1: {
		uint8_t modrm = fetch8(e);
		int opc = MODRM_REG(modrm);
		int rm = RM(modrm, b_ext);
		uint8_t cnt = fetch8(e);
		if (IS_REG(modrm) && cnt > 0) {
			uint64_t val = e->regs[rm];
			int cf = 0;
			if (opc == 4) { /* SHL */
				cf = (val >> (64 - cnt)) & 1;
				e->regs[rm] = val << cnt;
			} else if (opc == 5) { /* SHR */
				cf = (val >> (cnt - 1)) & 1;
				e->regs[rm] = val >> cnt;
			} else if (opc == 7) { /* SAR */
				cf = (val >> (cnt - 1)) & 1;
				e->regs[rm] = (uint64_t)((int64_t)val >> cnt);
			}
			set_flags_result(e, e->regs[rm], w ? 64 : 32);
			if (cf) e->rflags |= CF;
		}
		break;
	}

	/* MOV r/m8, r8 — 88 ; MOV [rbp+disp], al etc. */
	/* (already handled in 0x88 case below via the LEA/MOV mega-case) */

	/* JMP rel8 — EB */
	case 0xeb:
		e->ip += (int8_t)fetch8(e);
		break;

	/* JMP rel32 — E9 */
	case 0xe9:
		e->ip += fetch32(e);
		break;

	/* CALL rel32 — E8 */
	case 0xe8: {
		int32_t off = fetch32(e);
		uint64_t ret_addr = (uint64_t)(e->ip - e->mem);
		uint64_t target = ret_addr + off;
		push64(e, ret_addr);
#ifdef DEBUG_STACK
		dbg_push_frame(e, e->regs[RSP]);
#endif
#ifdef DEBUG_CALL
		fprintf(stderr, "CALL %06llx"
			" rax=%llx rcx=%llx rdx=%llx rbx=%llx"
			" rsp=%llx rbp=%llx rsi=%llx rdi=%llx"
			" r8=%llx r9=%llx r10=%llx r11=%llx"
			" r12=%llx r13=%llx r14=%llx r15=%llx\n",
			(unsigned long long)(target - e->code_base),
			(unsigned long long)e->regs[RAX],
			(unsigned long long)e->regs[RCX],
			(unsigned long long)e->regs[RDX],
			(unsigned long long)e->regs[RBX],
			(unsigned long long)e->regs[RSP],
			(unsigned long long)e->regs[RBP],
			(unsigned long long)e->regs[RSI],
			(unsigned long long)e->regs[RDI],
			(unsigned long long)e->regs[R8],
			(unsigned long long)e->regs[R9],
			(unsigned long long)e->regs[R10],
			(unsigned long long)e->regs[R11],
			(unsigned long long)e->regs[R12],
			(unsigned long long)e->regs[R13],
			(unsigned long long)e->regs[R14],
			(unsigned long long)e->regs[R15]);
#endif
		e->ip = e->mem + target;
		break;
	}

	/* Jcc rel8 */
	case 0x74: { int8_t off = fetch8(e); if (e->rflags & ZF) e->ip += off; break; }   /* JE */
	case 0x75: { int8_t off = fetch8(e); if (!(e->rflags & ZF)) e->ip += off; break; } /* JNE */
	case 0x72: { int8_t off = fetch8(e); if (e->rflags & CF) e->ip += off; break; }    /* JB */
	case 0x73: { int8_t off = fetch8(e); if (!(e->rflags & CF)) e->ip += off; break; } /* JAE */
	case 0x7c: { int8_t off = fetch8(e); if (!!(e->rflags & SF) != !!(e->rflags & OF)) e->ip += off; break; } /* JL */
	case 0x7d: { int8_t off = fetch8(e); if (!!(e->rflags & SF) == !!(e->rflags & OF)) e->ip += off; break; } /* JGE */
	case 0x7e: { int8_t off = fetch8(e); if ((e->rflags & ZF) || (!!(e->rflags & SF) != !!(e->rflags & OF))) e->ip += off; break; } /* JLE */
	case 0x7f: { int8_t off = fetch8(e); if (!(e->rflags & ZF) && (!!(e->rflags & SF) == !!(e->rflags & OF))) e->ip += off; break; } /* JG */

	/* MOVSXD r64, r/m32: 63 /r (with REX.W) */
	case 0x63:
	/* LEA r, m — 8D /r */
	case 0x8d:
	/* MOV r/m, r — 88 byte store, 89 qword store, 8B load */
	case 0x88: case 0x89: case 0x8b: {
		uint8_t modrm = fetch8(e);
		int reg = REG(modrm, r_ext);
		int rm = RM(modrm, b_ext);
		int mod = MODRM_MOD(modrm);
		if (mod == 3) {
			/* register-register */
			if (b == 0x63)
				e->regs[reg] = (uint64_t)(int64_t)(int32_t)(uint32_t)e->regs[rm];
			else if (b == 0x89)
				e->regs[rm] = e->regs[reg];
			else
				e->regs[reg] = e->regs[rm];
		} else {
			/* memory: compute effective address */
			uint64_t addr = 0;
			int has_sib = (MODRM_RM(modrm) == 4);
			int base_reg = rm;
			if (has_sib) {
				uint8_t sib = fetch8(e);
				base_reg = (sib & 7) + (b_ext ? 8 : 0);
				int idx_reg = ((sib >> 3) & 7) + (x_ext ? 8 : 0);
				int scale = 1 << ((sib >> 6) & 3);
				addr = e->regs[base_reg];
				if (idx_reg != 4) /* RSP = no index */
					addr += e->regs[idx_reg] * scale;
			} else {
				if (mod == 0 && rm == 5) {
					/* RIP-relative */
					int32_t disp = fetch32(e);
					addr = (uint64_t)(e->ip - e->mem) + disp;
				} else {
					addr = e->regs[base_reg];
				}
			}
			if (mod == 1) {
				int8_t d8 = (int8_t)fetch8(e);
				addr += d8;
			} else if (mod == 2) {
				int32_t d32 = fetch32(e);
				addr += d32;
			}
			if (b == 0x8d) {
				/* LEA: just store address */
				e->regs[reg] = addr;
			} else if (b == 0x88) {
				/* byte store — REX.R selects the source register (e.g. R8B..R15B) */
				mem_write8(e, addr, (uint8_t)e->regs[reg]);
			} else if (b == 0x89) {
				if (osz) {
					/* word store */
					mem_write8(e, addr, e->regs[reg] & 0xff);
					mem_write8(e, addr+1, (e->regs[reg] >> 8) & 0xff);
				} else if (w) {
					mem_write64(e, addr, e->regs[reg]);
				} else {
					/* dword store — written byte-by-byte (no mem_write32) */
					mem_write8(e, addr, e->regs[reg] & 0xff);
					mem_write8(e, addr+1, (e->regs[reg] >> 8) & 0xff);
					mem_write8(e, addr+2, (e->regs[reg] >> 16) & 0xff);
					mem_write8(e, addr+3, (e->regs[reg] >> 24) & 0xff);
				}
			} else if (b == 0x63) {
				/* MOVSXD: sign-extend dword to qword */
				e->regs[reg] = (uint64_t)(int64_t)(int32_t)(uint32_t)mem_read32(e, addr);
			} else {
				/* load: w=1 → 64-bit, w=0 → 32-bit zero-extended */
				if (w)
					e->regs[reg] = mem_read64(e, addr);
				else
					e->regs[reg] = mem_read32(e, addr);
			}
		}
		break;
	}

	/* ALU r/m, imm32 — 81 /op */
	case 0x81: {
		uint8_t modrm = fetch8(e);
		int rm = RM(modrm, b_ext);
		int opc = MODRM_REG(modrm);
		int32_t imm = fetch32(e);
		if (IS_REG(modrm)) {
			uint64_t val = w ? (int64_t)imm : (uint32_t)imm;
			switch (opc) {
			case 0: { /* ADD */
				uint64_t old = e->regs[rm];
				e->regs[rm] += val;
				set_flags_add(e, old, val, e->regs[rm], w ? 64 : 32);
				break;
			}
			case 1: /* OR */
				e->regs[rm] |= val;
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				break;
			case 4: /* AND */
				e->regs[rm] &= val;
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				break;
			case 5: { /* SUB */
				uint64_t old = e->regs[rm];
				e->regs[rm] -= val;
				set_flags_sub(e, old, val, e->regs[rm], w ? 64 : 32);
				break;
			}
			case 6: /* XOR */
				e->regs[rm] ^= val;
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				break;
			case 7: /* CMP */
				set_flags_sub(e, e->regs[rm], val,
					      e->regs[rm] - val, w ? 64 : 32);
				break;
			default:
				fprintf(stderr, "x86emu: unhandled 81 /%d\n", opc);
				e->halted = 1;
			}
		}
		break;
	}

	/* MOV r/m, imm32 — C7 /0 */
	case 0xc7: {
		uint8_t modrm = fetch8(e);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		int32_t imm = fetch32(e);
		uint64_t val = w ? (uint64_t)(int64_t)imm : (uint32_t)imm;
		if (ok)
			mem_write64(e, addr, val);
		else
			e->regs[(int)addr] = val;
		break;
	}

	/* INC/DEC r/m — FF /0 (INC), FF /1 (DEC) */
	case 0xff: {
		uint8_t modrm = fetch8(e);
		int opc = MODRM_REG(modrm);
		int ok = 0;
		uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
		if (opc == 0) { /* INC */
			if (ok) {
				uint64_t v = mem_read_w(e, addr, w);
				mem_write64(e, addr, v + 1);
				set_flags_result(e, v + 1, w ? 64 : 32);
			} else {
				e->regs[(int)addr]++;
				set_flags_result(e, e->regs[(int)addr], w ? 64 : 32);
			}
		} else if (opc == 1) { /* DEC */
			if (ok) {
				uint64_t v = mem_read_w(e, addr, w);
				mem_write64(e, addr, v - 1);
				set_flags_result(e, v - 1, w ? 64 : 32);
			} else {
				e->regs[(int)addr]--;
				set_flags_result(e, e->regs[(int)addr], w ? 64 : 32);
			}
		} else if (opc == 2) { /* CALL r/m64 (indirect call, e.g. FF D0) */
			uint64_t target;
			if (ok)
				target = mem_read64(e, addr);
			else
				target = e->regs[(int)addr];
			push64(e, (uint64_t)(e->ip - e->mem));
#ifdef DEBUG_STACK
			dbg_push_frame(e, e->regs[RSP]);
#endif
			e->ip = e->mem + target;
		} else {
			fprintf(stderr, "x86emu: unhandled FF /%d\n", opc);
			e->halted = 1;
		}
		break;
	}

	/* ALU r/m, imm8 (sign-ext) — 83 /op */
	case 0x83: {
		uint8_t modrm = fetch8(e);
		int rm = RM(modrm, b_ext);
		int op = MODRM_REG(modrm);
		int8_t imm = fetch8(e);
		if (IS_REG(modrm)) {
			switch (op) {
			case 0: { /* ADD */
				uint64_t old = e->regs[rm];
				uint64_t val = (uint64_t)(int64_t)imm;
				e->regs[rm] += val;
				set_flags_add(e, old, val, e->regs[rm], w ? 64 : 32);
				break;
			}
			case 1: /* OR */
				e->regs[rm] |= (uint64_t)(int64_t)imm;
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				break;
			case 4: /* AND */
				e->regs[rm] &= (uint64_t)(int64_t)imm;
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				break;
			case 5: { /* SUB */
				uint64_t old = e->regs[rm];
				uint64_t val = (uint64_t)(int64_t)imm;
				e->regs[rm] -= val;
				set_flags_sub(e, old, val, e->regs[rm], w ? 64 : 32);
				break;
			}
			case 6: /* XOR */
				e->regs[rm] ^= (uint64_t)(int64_t)imm;
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				break;
			case 7: /* CMP */
				set_flags_sub(e, e->regs[rm], (uint64_t)(int64_t)imm,
					      e->regs[rm] - (int64_t)imm, w ? 64 : 32);
				break;
			default:
				fprintf(stderr, "x86emu: unhandled 83 /%d\n", op);
				e->halted = 1;
			}
		}
		break;
	}

	/* 0F xx — two-byte opcodes */
	case 0x0f: {
		uint8_t op2 = fetch8(e);
		switch (op2) {
		/* SETcc r/m8: 0F 90+cc /0 */
		case 0x92: case 0x93: case 0x94: case 0x95:
		case 0x96: case 0x97: {
			uint8_t modrm = fetch8(e);
			int rm = RM(modrm, b_ext);
			int cond = 0;
			switch (op2) {
			case 0x92: cond = (e->rflags & CF) != 0; break; /* setb */
			case 0x93: cond = (e->rflags & CF) == 0; break; /* setae */
			case 0x94: cond = (e->rflags & ZF) != 0; break; /* sete */
			case 0x95: cond = (e->rflags & ZF) == 0; break; /* setne */
			case 0x96: cond = ((e->rflags & CF) || (e->rflags & ZF)); break; /* setbe */
			case 0x97: cond = !((e->rflags & CF) || (e->rflags & ZF)); break; /* seta */
			}
			if (IS_REG(modrm))
				e->regs[rm] = (e->regs[rm] & ~0xffULL) | cond;
			break;
		}
		/* Jcc rel32: 0F 80-8F */
		case 0x84: case 0x85: case 0x82: case 0x83:
		case 0x86: case 0x87:
		case 0x8c: case 0x8d: case 0x8e: case 0x8f: {
			int32_t off = fetch32(e);
			int cond = 0;
			switch (op2) {
			case 0x84: cond = (e->rflags & ZF) != 0; break;  /* JE */
			case 0x85: cond = (e->rflags & ZF) == 0; break;  /* JNE */
			case 0x82: cond = (e->rflags & CF) != 0; break;  /* JB */
			case 0x83: cond = (e->rflags & CF) == 0; break;  /* JAE */
			case 0x86: cond = ((e->rflags & CF) != 0) || ((e->rflags & ZF) != 0); break; /* JBE */
			case 0x87: cond = ((e->rflags & CF) == 0) && ((e->rflags & ZF) == 0); break; /* JA */
			case 0x8c: cond = !!(e->rflags & SF) != !!(e->rflags & OF); break;  /* JL */
			case 0x8d: cond = !!(e->rflags & SF) == !!(e->rflags & OF); break;  /* JGE */
			case 0x8e: cond = ((e->rflags & ZF) || (!!(e->rflags & SF) != !!(e->rflags & OF))); break; /* JLE */
			case 0x8f: cond = (!(e->rflags & ZF) && (!!(e->rflags & SF) == !!(e->rflags & OF))); break; /* JG */
			}
			if (cond) e->ip += off;
			break;
		}

		/* setcc signed: 0F 9C-9F */
		case 0x9c: case 0x9d: case 0x9e: case 0x9f: {
			uint8_t modrm = fetch8(e);
			int rm = RM(modrm, b_ext);
			int cond = 0;
			switch (op2) {
			case 0x9c: cond = !!(e->rflags & SF) != !!(e->rflags & OF); break; /* setl */
			case 0x9d: cond = !!(e->rflags & SF) == !!(e->rflags & OF); break; /* setge */
			case 0x9e: cond = ((e->rflags & ZF) || (!!(e->rflags & SF) != !!(e->rflags & OF))); break; /* setle */
			case 0x9f: cond = (!(e->rflags & ZF) && (!!(e->rflags & SF) == !!(e->rflags & OF))); break; /* setg */
			}
			if (IS_REG(modrm))
				e->regs[rm] = (e->regs[rm] & ~0xffULL) | cond;
			break;
		}

		/* MOVZX r, r/m8: 0F B6 /r */
		case 0xb6: {
			uint8_t modrm = fetch8(e);
			int reg = REG(modrm, r_ext);
			int ok = 0;
			uint64_t addr;
			if (IS_REG(modrm)) {
				e->regs[reg] = e->regs[RM(modrm, b_ext)] & 0xff;
			} else {
				addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
				if (ok)
					e->regs[reg] = mem_read8(e, addr);
			}
			break;
		}
		/* MOVZX r, r/m16: 0F B7 /r */
		case 0xb7: {
			uint8_t modrm = fetch8(e);
			int reg = REG(modrm, r_ext);
			int ok = 0;
			uint64_t addr;
			if (IS_REG(modrm)) {
				e->regs[reg] = e->regs[RM(modrm, b_ext)] & 0xffff;
			} else {
				addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
				if (ok)
					e->regs[reg] = mem_read8(e, addr) | ((uint64_t)mem_read8(e, addr+1) << 8);
			}
			break;
		}
		/* MOVSX r, r/m8: 0F BE /r */
		case 0xbe: {
			uint8_t modrm = fetch8(e);
			int reg = REG(modrm, r_ext);
			int8_t val;
			int ok = 0;
			uint64_t addr;
			if (IS_REG(modrm)) {
				val = (int8_t)(e->regs[RM(modrm, b_ext)] & 0xff);
			} else {
				addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
				val = ok ? (int8_t)mem_read8(e, addr) : 0;
			}
			if (w)
				e->regs[reg] = (uint64_t)(int64_t)val;
			else
				e->regs[reg] = (uint32_t)(int32_t)val;
			break;
		}
		/* MOVSX r, r/m16: 0F BF /r */
		case 0xbf: {
			uint8_t modrm = fetch8(e);
			int reg = REG(modrm, r_ext);
			int16_t val;
			int ok = 0;
			uint64_t addr;
			if (IS_REG(modrm)) {
				val = (int16_t)(e->regs[RM(modrm, b_ext)] & 0xffff);
			} else {
				addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
				val = ok ? (int16_t)(mem_read8(e, addr) | ((uint16_t)mem_read8(e, addr+1) << 8)) : 0;
			}
			if (w)
				e->regs[reg] = (uint64_t)(int64_t)val;
			else
				e->regs[reg] = (uint32_t)(int32_t)val;
			break;
		}

		/* IMUL r, r/m: 0F AF /r */
		case 0xaf: {
			uint8_t modrm = fetch8(e);
			int reg = REG(modrm, r_ext);
			int ok = 0;
			uint64_t addr = resolve_modrm_addr(e, modrm, b_ext, &ok);
				uint64_t rm_val = ok ? mem_read_w(e, addr, w) : e->regs[(int)addr];
			e->regs[reg] *= rm_val;
			set_flags_result(e, e->regs[reg], w ? 64 : 32);
			break;
		}
		default:
			fprintf(stderr, "x86emu: unhandled 0F %02x\n", op2);
			e->halted = 1;
			return -1;
		}
		break;
	}

	/* SHL/SHR r/m, cl — D3 /4 /5 */
	case 0xd3: {
		uint8_t modrm = fetch8(e);
		int rm = RM(modrm, b_ext);
		int opc = MODRM_REG(modrm);
		if (IS_REG(modrm)) {
			uint8_t cnt = e->regs[RCX] & 63;
			if (cnt > 0) {
				uint64_t val = e->regs[rm];
				int cf = 0;
				if (opc == 4) { /* SHL */
					cf = (val >> (64 - cnt)) & 1;
					e->regs[rm] = val << cnt;
				} else if (opc == 5) { /* SHR */
					cf = (val >> (cnt - 1)) & 1;
					e->regs[rm] = val >> cnt;
				} else if (opc == 7) { /* SAR */
					cf = (val >> (cnt - 1)) & 1;
					e->regs[rm] = (uint64_t)((int64_t)val >> cnt);
				}
				set_flags_result(e, e->regs[rm], w ? 64 : 32);
				if (cf) e->rflags |= CF;
			}
		}
		break;
	}

	default:
		e->rip = (uint64_t)(e->ip - e->mem);
		fprintf(stderr, "x86emu: unhandled opcode %02x at rip=%llx (rex=%02x)\n",
			b, (unsigned long long)e->rip - 1, rex);
		e->halted = 1;
		return -1;
	}

	e->rip = (uint64_t)(e->ip - e->mem);
	if (e->trace)
		fprintf(stderr, "  [%06llx] rax=%llx rcx=%llx rdi=%llx rsp=%llx rbp=%llx zf=%d\n",
			(unsigned long long)(step_rip - e->code_base),
			(unsigned long long)e->regs[RAX],
			(unsigned long long)e->regs[RCX],
			(unsigned long long)e->regs[RDI],
			(unsigned long long)e->regs[RSP],
			(unsigned long long)e->regs[RBP],
			(int)((e->rflags & ZF) != 0));
	return 0;
}

/* run until halt or step limit (20 billion instructions) */
static long long x86emu_run(x86emu_t *e)
{
	long long steps = 0;

	e->ip = e->mem + e->rip;
	while (__builtin_expect(!e->halted, 1)) {
		if (x86emu_step(e) < 0) break;
		steps++;
		if (__builtin_expect(steps > 20LL * 1000 * 1000 * 1000, 0)) {
			fprintf(stderr, "x86emu: too many steps\n");
			break;
		}
	}
	e->rip = (uint64_t)(e->ip - e->mem);
	return steps;
}

/* allocate flat memory, copy code into it, set RSP to stack top */
static void x86emu_init(x86emu_t *e, const uint8_t *code, int code_len)
{
	memset(e, 0, sizeof(*e));

	/* Memory layout (all regions in one flat allocation):
	 *   0x0000000 .. 0x1FFFFFF  32MB  guard (null-pointer trap)
	 *   0x2000000 .. 0x3FFFFFF  32MB  code
	 *   0x4000000 .. 0x5FFFFFF  32MB  BSS
	 *   0x6000000 .. 0x7FFFFFF  32MB  input
	 *   0x8000000 .. 0x9FFFFFF  32MB  output
	 *   0xA000000 .. 0xBFFFFFF  32MB  stack (grows down from 0xC000000)
	 *
	 * Total: 192MB. */
	e->mem_size = 192 * 1024 * 1024;
	e->mem = calloc(1, e->mem_size);

	/* code at 0x2000000 */
	e->code_base = 0x2000000;
	memcpy(e->mem + e->code_base, code, code_len);
	e->rip = e->code_base;
	e->ip = e->mem + e->rip;

	/* stack at top of stack region, growing down.
	 * RSP starts at STACK_HI so that after the entry stub's
	 * CALL main (push ret_addr), RSP matches the native runner
	 * (0xBFFFFFF8, aligned: RSP%16==8 per SysV ABI). */
	e->regs[RSP] = 0xc000000;
}
