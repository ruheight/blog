/*
 * cc_lib.c — shared utilities for c1/c2 pipelines
 *
 * Included by c1m.c, c2.c, c2m.c, c2d.c and any other
 * translation unit that needs the PRNG.
 */

/* xorshift64 PRNG — guarded by mutation ifdefs so non-mutating
 * builds (plain c1/c2) pay no cost.  Shared by all MUT_* passes
 * and regalloc shuffling. */
#if defined(MUT_LOCAL) || defined(MUT_GLOBAL) || defined(MUT_COMMUTE) || defined(MUT_REASSOC) || defined(MUT_IF_INVERT) || defined(MUT_REGALLOC) || defined(MUT_STORAGE) || defined(MUT_SCHEDULE) || defined(HOST_BUILD)

long rng_state;

/* seed the PRNG; zero is forced to 1 because xorshift has a zero fixed point */
void srng(long seed)
{
	rng_state = seed;
	if (rng_state == 0)
		rng_state = 1;
}

long rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

#endif


