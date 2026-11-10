/*
 * disasm.h - x86_64 instruction decoder without lookup tables
 *
 * Single function x86_decode(): all opcode classification via
 * espresso-minimized (mask & op) == value predicates.
 * Zero arrays, zero static data, zero .rodata, zero libc calls.
 *
 * Supports: legacy 1/2/3-byte, FPU, VEX/EVEX/XOP (ifdef X86_DECODE_VEX).
 * Rejects:  invalid 64-bit opcodes, undefined GRP4/GRP5, truncated input.
 *
 * Tested: 36M+ instructions (369 libraries + Linux kernel), 0 errors;
 * differential-fuzzed against the frozen baseline (diff_fuzz.c, 22.5M
 * inputs, byte-exact struct compare).
 */
#ifndef DISASM_H
#define DISASM_H

#include <stdint.h>

#define X86_MAX_INSN	15

/*
 * Scalar barrier for predicate terms.
 * Empty volatile asm forces each term's input through an opaque
 * register, so the compiler cannot batch (mask & op) == value chains
 * into SIMD blocks (which would materialize 16-byte mask constants
 * in .rodata). Zero instructions emitted; works on gcc and clang.
 */
#define X86_B(x)	({ uint8_t _x = (x); __asm__ volatile("" : "+r"(_x)); _x; })

/*
 * Espresso-minimized opcode predicates (no tables).
 * IMM predicates are re-minimized with don't-care inputs — opcodes
 * that never reach them (INVALID_64 set, VEX prefixes, 0F escape and
 * the early-return special cases in _imm_size_1b) are left free,
 * letting espresso merge terms (14→12, 9→5). See gen_dc.py.
 */
#define HAS_MODRM(op)								\
	((X86_B(op) & 0xc4) == 0x00 || (X86_B(op) & 0xf0) == 0x80 ||		\
	 (X86_B(op) & 0xba) == 0x80 || (X86_B(op) & 0xbc) == 0x84 ||		\
	 (X86_B(op) & 0xbe) == 0x22 || (X86_B(op) & 0xbd) == 0x29 ||		\
	 (X86_B(op) & 0xf4) == 0xd0 || (X86_B(op) & 0xf8) == 0xd8 ||		\
	 (X86_B(op) & 0xf6) == 0xf6)

#define HAS_IMM1(op)								\
	((X86_B(op) & 0x7f) == 0x6b || (X86_B(op) & 0xff) == 0x83 ||		\
	 (X86_B(op) & 0xc7) == 0x04 || (X86_B(op) & 0xff) == 0xcd ||		\
	 (X86_B(op) & 0xbf) == 0x80 || (X86_B(op) & 0xde) == 0xc0 ||		\
	 (X86_B(op) & 0xdf) == 0xc6 || (X86_B(op) & 0xff) == 0xa8 ||		\
	 (X86_B(op) & 0xf0) == 0x70 || (X86_B(op) & 0xee) == 0x6a ||		\
	 (X86_B(op) & 0xf8) == 0xb0 || (X86_B(op) & 0xf8) == 0xe0)

#define HAS_IMM4(op)								\
	((X86_B(op) & 0xff) == 0xc7 || (X86_B(op) & 0xfe) == 0x68 ||		\
	 (X86_B(op) & 0xff) == 0x81 || (X86_B(op) & 0xff) == 0xa9 ||		\
	 (X86_B(op) & 0xc7) == 0x05)

#define HAS_IMM2(op)	((X86_B(op) & 0xf7) == 0xc2)

/* 0F second-byte predicates */
#define NO_MODRM_0F(op2)								\
	((X86_B(op2) & 0x75) == 0x04 || (X86_B(op2) & 0x76) == 0x06 ||		\
	 (X86_B(op2) & 0xdd) == 0x05 || (X86_B(op2) & 0x7c) == 0x08 ||		\
	 (X86_B(op2) & 0xf0) == 0x30 || (X86_B(op2) & 0xbf) == 0x37 ||	\
	 (X86_B(op2) & 0xbe) == 0x3a || (X86_B(op2) & 0xd5) == 0x80 ||	\
	 (X86_B(op2) & 0xd6) == 0x80 || (X86_B(op2) & 0xf0) == 0x80 ||	\
	 (X86_B(op2) & 0xb8) == 0x88)

#define HAS_IMM4_0F(op2)	((X86_B(op2) & 0xf0) == 0x80)
#define HAS_IMM1_0F(op2)								\
	((X86_B(op2) & 0xfc) == 0x70 || (X86_B(op2) & 0xf7) == 0xa4 ||		\
	 (X86_B(op2) & 0xff) == 0xba || (X86_B(op2) & 0xfb) == 0xc2 ||	\
	 (X86_B(op2) & 0xfe) == 0xc4)

#define INVALID_64(op)								\
	((X86_B(op) & 0xe7) == 0x06 || (X86_B(op) & 0xcf) == 0x07 ||		\
	 (X86_B(op) & 0xd7) == 0x17 || (X86_B(op) & 0xe7) == 0x27 ||		\
	 (X86_B(op) & 0xfe) == 0x60 || (X86_B(op) & 0xff) == 0x82 ||		\
	 (X86_B(op) & 0xff) == 0x9a || (X86_B(op) & 0xff) == 0xce ||		\
	 (X86_B(op) & 0xfe) == 0xd4 || (X86_B(op) & 0xfd) == 0xd4 ||		\
	 (X86_B(op) & 0xff) == 0xea)

/* ModRM/SIB predicates */
#define MODRM_REG_DIRECT(m)	(((m) & 0xC0) == 0xC0)
#define MODRM_HAS_SIB(m)	(((m) & 0x47) == 0x04 || ((m) & 0x87) == 0x04)
#define MODRM_DISP8(m)		(((m) & 0xC0) == 0x40)
#define MODRM_DISP32(m)		(((m) & 0x47) == 0x05 || ((m) & 0xC0) == 0x80)
#define SIB_DISP32(m, s)	(((m) & 0xC7) == 0x04 && ((s) & 0x07) == 0x05)

#ifdef X86_DECODE_VEX
/* VEX map opcode predicates */
#define VEX_MAP1_IMM8(op)								\
	((X86_B(op) & 0xFC) == 0x70 || X86_B(op) == 0xC2 ||			\
	 (X86_B(op) & 0xFE) == 0xC4 || X86_B(op) == 0xC6)
#define EVEX_MAP4_IMM8(op)								\
	(X86_B(op) == 0x24 || X86_B(op) == 0x2C || X86_B(op) == 0x6B ||		\
	 X86_B(op) == 0x80 || X86_B(op) == 0x83 ||				\
	 (X86_B(op) & 0xFE) == 0xC0 || X86_B(op) == 0xF6)
#endif

/*  Decoded instruction */
typedef struct {
	/* Lengths */
	uint8_t len;		/* total instruction length */
	uint8_t len_opcode;	/* 1, 2, or 3 */
	uint8_t len_disp;	/* 0, 1, or 4 */
	uint8_t len_imm;	/* 0, 1, 2, 3, 4, or 8 */

	/* Opcode and prefixes */
	uint8_t opcode[3];
	uint8_t seg;		/* segment prefix (26/2E/36/3E/64/65) */
	uint8_t rep;		/* rep prefix (F2 or F3) */
#ifdef X86_DECODE_VEX
	uint8_t vex_map;
	uint8_t vex_pp;
#endif

	/* ModRM, SIB, REX */
	union { uint8_t raw; struct { uint8_t rm:3, reg:3, mod:2; }; } modrm;
	union { uint8_t raw; struct { uint8_t base:3, index:3, scale:2; }; } sib;
	union { uint8_t raw; struct { uint8_t b:1, x:1, r:1, w:1, _hi:4; }; } rex;

	/* Displacement and immediate */
	union {
		int8_t s8; int16_t s16; int32_t s32; int64_t s64;
		uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;
		uint8_t bytes[8];
	} disp, imm;

	/* Flags */
	uint16_t has_modrm : 1;
	uint16_t has_sib   : 1;
	uint16_t has_disp  : 1;
	uint16_t has_imm   : 1;
	uint16_t has_rex   : 1;
#ifdef X86_DECODE_VEX
	uint16_t has_vex   : 1;
	uint16_t has_evex  : 1;
	uint16_t has_xop   : 1;
#endif
	uint16_t has_lock  : 1;
	uint16_t has_rep   : 1;
	uint16_t has_repn  : 1;
	uint16_t has_66    : 1;
	uint16_t has_67    : 1;
	uint16_t has_seg   : 1;
} x86_insn;

/*
 * Little-endian load of n bytes (n <= 8), zero-extended.
 * Replaces memcpy into the union: on LE hosts the u64 view matches the
 * byte view, and no libc dependency is needed.
 */
#define _ld64(p, n)								\
	({ uint64_t _v = 0;							\
	   for (int _i = 0; _i < (n); _i++)					\
		_v |= (uint64_t)X86_B((p)[_i]) << (8 * _i);			\
	   _v; })

/*  Immediate size, 1-byte opcode map */
static __attribute__((always_inline)) inline int
_imm_size_1b(uint8_t op, int opsz, int addrsz, int rexw)
{
	if ((op & 0xF8) == 0xB8) return rexw ? 8 : (opsz ? 2 : 4);	/* MOV r, imm */
	if ((op & 0xFC) == 0xA0) return addrsz ? 4 : 8;			/* MOV moffs */
	if (op == 0xC8) return 3;					/* ENTER iw,ib */
	if ((op & 0xFE) == 0xE8) return 4;				/* CALL/JMP rel32 */
	if (HAS_IMM4(op)) return opsz ? 2 : 4;
	if (HAS_IMM2(op)) return 2;
	if (HAS_IMM1(op)) return 1;
	return 0;
}

/*
 * ModRM decoder (returns bytes consumed, or -1 on truncation).
 * Fills modrm/sib/disp fields and their flags.
 */
static __attribute__((always_inline)) inline int
_decode_modrm(const uint8_t *p, int avail, x86_insn *insn)
{
	uint8_t m;
	int pos;

	if (avail < 1)
		return -1;

	m = p[0];
	pos = 1;
	insn->modrm.raw = m;
	insn->has_modrm = 1;

	if (MODRM_REG_DIRECT(m))
		return 1;

	if (MODRM_HAS_SIB(m)) {
		if (pos >= avail)
			return -1;
		insn->sib.raw = p[pos++];
		insn->has_sib = 1;
		if (SIB_DISP32(m, insn->sib.raw))
			insn->len_disp = 4;
	}

	/* mod field takes priority over SIB base=5 displacement */
	if (MODRM_DISP8(m))
		insn->len_disp = 1;
	else if (MODRM_DISP32(m))
		insn->len_disp = 4;

	if (insn->len_disp) {
		if (pos + insn->len_disp > avail)
			return -1;
		insn->has_disp = 1;
		insn->disp.u64 = _ld64(p + pos, insn->len_disp);
		pos += insn->len_disp;
	}

	return pos;
}

/*
 * Main decoder. Returns instruction length (>0) or 0 on failure.
 *
 * Common tail for all paths (modrm_imm): decode ModRM if the path
 * demands it, reject undefined GRP4/5 extensions, resolve deferred
 * immediate size (1-byte map only), then read the immediate.
 */
static int x86_decode(const uint8_t *code, int max_len, x86_insn *insn)
{
	uint8_t op, op2, pfx;
	int pos, mr, immsz, need_modrm;

	if (max_len < 1)
		return 0;
	if (max_len > X86_MAX_INSN)
		max_len = X86_MAX_INSN;

	*insn = (x86_insn){0};
	pos = 0;

	/* Prefixes */
	for (; pos < max_len; pos++) {
		pfx = code[pos];
		if      ((pfx & 0xF0) == 0x40)				{ insn->rex.raw = pfx; insn->has_rex = 1; }
		else if ((pfx & 0xE7) == 0x26 || (pfx & 0xFE) == 0x64)	{ insn->seg = pfx; insn->has_seg = 1; }
		else if (pfx == 0x66) insn->has_66 = 1;
		else if (pfx == 0x67) insn->has_67 = 1;
		else if (pfx == 0xF0) insn->has_lock = 1;
		else if (pfx == 0xF3) { insn->rep = 0xF3; insn->has_rep = 1; }
		else if (pfx == 0xF2) { insn->rep = 0xF2; insn->has_repn = 1; }
		else break;
	}
	if (pos >= max_len)
		return 0;

	/* FWAIT + FPU merge (9B before D8-DF) */
	if (code[pos] == 0x9B && pos + 1 < max_len &&
	    (code[pos + 1] & 0xF8) == 0xD8)
		pos++;

	/* Opcode byte */
	op = code[pos++];
	insn->opcode[0] = op;
	insn->len_opcode = 1;

	if (INVALID_64(op))
		return 0;

#ifdef X86_DECODE_VEX
	/*
	 * VEX (C5): 1 payload byte; VEX (C4) / XOP (8F, map >= 8): 2;
	 * EVEX (62): 3. Shared header layout:
	 *   b1 = R/X/B + map,  b2 = W/vvvv/L + pp.
	 */
	{
		uint8_t b1, b2;
		int n;

		if (op == 0xC5)
			n = 1;
		else if (op == 0xC4 || (op == 0x8F && pos < max_len &&
		       (code[pos] & 0x1F) >= 8))
			n = 2;
		else if (op == 0x62)
			n = 3;
		else
			goto legacy_op;

		if (pos + n - 1 >= max_len)
			return 0;
		if (n == 1) {				/* C5: R + vvvvLpp */
			b1 = code[pos];
			insn->rex.raw = (~b1 >> 5) & 0x04;
			insn->vex_map = 1;
			insn->vex_pp = b1 & 3;
		} else {				/* C4/62/8F: b1+b2 */
			b1 = code[pos];
			b2 = code[pos + 1];
			insn->rex.raw = ((~b1 >> 5) & 0x07) | ((b2 >> 4) & 0x08);
			insn->vex_map = (op == 0x62) ? (b1 & 0x07) : (b1 & 0x1F);
			insn->vex_pp = b2 & 3;
		}
		pos += n;	/* EVEX byte 3 skipped: no length effect */
		insn->has_vex  = (op == 0xC4 || op == 0xC5);
		insn->has_xop  = (op == 0x8F);
		insn->has_evex = (op == 0x62);
		goto vex_opcode;
	}

vex_opcode:
	/* VEX/EVEX/XOP: opcode is the map opcode; immsz is map-dependent */
	if (pos >= max_len)
		return 0;
	op2 = code[pos++];
	insn->opcode[0] = op2;
	immsz = 0;
	if ((0x188 >> insn->vex_map) & 1)		/* maps 3, 7, 8: imm8 */
		immsz = 1;
	else if (insn->vex_map == 1)
		immsz = VEX_MAP1_IMM8(op2);
	else if (insn->vex_map == 4)
		immsz = EVEX_MAP4_IMM8(op2);
	else if (insn->vex_map == 0xA)
		immsz = 4;
	need_modrm = !(insn->vex_map == 1 && op2 == 0x77);	/* VZERO* */
	goto modrm_imm;

legacy_op:
#endif /* X86_DECODE_VEX */

	/* 0F escape */
	if (op == 0x0F) {
		if (pos >= max_len)
			return 0;
		op2 = code[pos];
		if (op2 == 0x04 || op2 == 0x0A || op2 == 0x0C ||
		    op2 == 0x0F || op2 == 0x27 || op2 == 0x36 ||
		    op2 == 0x39 || (op2 >= 0x3B && op2 <= 0x3F))
			return 0;
		insn->opcode[1] = op2;
		if (op2 == 0x38 || op2 == 0x3A) {	/* 3-byte escape */
			if (pos + 1 >= max_len)
				return 0;
			insn->opcode[2] = code[pos + 1];
			insn->len_opcode = 3;
			pos += 2;
			need_modrm = 1;
			immsz = (op2 == 0x3A);		/* 0F 3A: imm8 */
		} else {
			pos++;
			insn->len_opcode = 2;
			need_modrm = !NO_MODRM_0F(op2);
			immsz = HAS_IMM4_0F(op2) ? 4 :
			        (HAS_IMM1_0F(op2) ? 1 : 0);
		}
		goto modrm_imm;
	}

	/* 1-byte: immsz deferred (GRP3 TEST depends on modrm.reg) */
	need_modrm = HAS_MODRM(op);
	immsz = -1;

modrm_imm:
	/* ModRM (all paths converge here) */
	if (need_modrm) {
		mr = _decode_modrm(code + pos, max_len - pos, insn);
		if (mr < 0)
			return 0;
		pos += mr;
	}

	/* Reject undefined GRP4/GRP5 extensions (legacy FE/FF only: for
	 * 0F/VEX paths op is 0F/C4/C5/62/8F and both tests are dead) */
	if (op == 0xFE && insn->modrm.reg >= 2)
		return 0;
	if (op == 0xFF && (insn->modrm.reg == 7 ||
	    ((insn->modrm.reg == 3 || insn->modrm.reg == 5) && insn->modrm.mod == 3)))
		return 0;

	/* Deferred immediate size (1-byte map only, immsz == -1) */
	if (immsz < 0) {
		if ((op & 0xFE) == 0xF6) {		/* GRP3: TEST (reg 0,1) */
			immsz = (insn->modrm.reg <= 1) ?
			        ((op == 0xF6) ? 1 : (insn->has_66 ? 2 : 4)) : 0;
		} else {
			immsz = _imm_size_1b(op, insn->has_66,
					     insn->has_67, insn->rex.w);
		}
	}

	/* Read immediate */
	if (immsz > 0) {
		if (pos + immsz > max_len)
			return 0;
		insn->has_imm = 1;
		insn->len_imm = immsz;
		insn->imm.u64 = _ld64(code + pos, immsz);
		pos += immsz;
	}

	insn->len = pos;
	return pos;
}

#endif /* DISASM_H */
