/* t37: comprehensive nested control flow — while, do/while, break, continue
 * at many nesting levels.  Exercises decompiler edge cases around
 * continue-in-do/while-inside-if/else-inside-while and similar patterns. */
long _bss_end;

long main(long in, long in_len, long out)
{
	long i;
	long j;
	long k;
	long sum;
	long total;
	long flag;
	long count;

	long tmp;
	_bss_end = 0;

	/* ------------------------------------------------------------------ */
	/* CHECK 1: triple-nested while with break at each level              */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 5) {
		j = 0;
		while (j < 5) {
			k = 0;
			while (k < 5) {
				if (k == 2)
					break;
				sum = sum + 1;
				k = k + 1;
			}
			/* inner ran 2 iters (k=0,1) */
			if (j == 3)
				break;
			j = j + 1;
		}
		/* middle ran 4 iters (j=0,1,2,3; breaks on 3) */
		if (i == 4)
			break;
		i = i + 1;
	}
	/* outer: 5 iters (i=0..4, breaks on 4).  middle: 4 each.  inner: 2 each.
	 * 5 * 4 * 2 = 40 */
	if (sum != 40) { *(long *)out = 1; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 2: triple-nested while with continue at each level           */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 6) {
		i = i + 1;
		if (i == 3)
			continue;  /* skip outer iter when i==3 */
		j = 0;
		while (j < 6) {
			j = j + 1;
			if (j == 2)
				continue;  /* skip middle iter when j==2 */
			k = 0;
			while (k < 4) {
				k = k + 1;
				if (k == 3)
					continue;  /* skip inner iter when k==3 */
				sum = sum + 1;
			}
			/* inner: k=1,2,4 counted => 3 per middle iter */
		}
		/* middle: j=1,3,4,5,6 counted (skip j==2) => 5 iters, 3 each = 15 */
	}
	/* outer: i=1,2,4,5,6 counted (skip i==3) => 5 iters, 15 each = 75 */
	if (sum != 75) { *(long *)out = 2; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 3: triple-nested do/while with break at each level           */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	do {
		j = 0;
		do {
			k = 0;
			do {
				sum = sum + 1;
				k = k + 1;
				if (k == 3)
					break;
			} while (k < 10);
			/* inner: 3 iters (k goes 0->1->2->3, break) */
			j = j + 1;
			if (j == 2)
				break;
		} while (j < 10);
		/* middle: 2 iters (j goes 0->1->2, break) */
		i = i + 1;
		if (i == 4)
			break;
	} while (i < 100);
	/* outer: 4 iters.  4 * 2 * 3 = 24 */
	if (sum != 24) { *(long *)out = 3; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 4: triple-nested do/while with continue at each level        */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	do {
		i = i + 1;
		if (i == 2) {
			/* continue the outer do/while, skipping the rest of body */
			continue;
		}
		j = 0;
		do {
			j = j + 1;
			if (j == 3)
				continue;  /* skip adding when j==3 */
			k = 0;
			do {
				k = k + 1;
				if (k == 1)
					continue;  /* skip adding when k==1 */
				sum = sum + 1;
			} while (k < 3);
			/* inner: k=1(skip),2,3 => 2 counted */
		} while (j < 4);
		/* middle: j=1,2,3(skip body),4 => 3 with inner => 3*2 = 6 */
	} while (i < 5);
	/* outer: i=1,2(skip),3,4,5 => 4 with body => 4*6 = 24 */
	if (sum != 24) { *(long *)out = 4; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 5: mixed — while containing do/while containing while        */
	/*          with break and continue across levels                     */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 4) {
		j = 0;
		do {
			j = j + 1;
			k = 0;
			while (k < 6) {
				k = k + 1;
				if (k == 4)
					break;  /* inner while breaks at k==4 */
				sum = sum + 1;
			}
			/* inner: k=1,2,3 counted => 3 */
			if (j == 2)
				continue;  /* skip the sum bump below for j==2 */
			sum = sum + 10;
		} while (j < 3);
		/* j=1: inner(3)+10=13; j=2: inner(3)+skip=3; j=3: inner(3)+10=13
		 * total per outer = 29 */
		i = i + 1;
	}
	/* 4 * 29 = 116 */
	if (sum != 116) { *(long *)out = 5; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 6: if/else at multiple nesting levels inside loops           */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 8) {
		if (i < 4) {
			j = 0;
			while (j < 3) {
				if (j == 1) {
					sum = sum + 5;
				} else {
					sum = sum + 1;
				}
				j = j + 1;
			}
			/* j=0: +1, j=1: +5, j=2: +1 => 7 per iter */
		} else {
			j = 0;
			while (j < 2) {
				if (j == 0) {
					sum = sum + 10;
				} else {
					sum = sum + 20;
				}
				j = j + 1;
			}
			/* j=0: +10, j=1: +20 => 30 per iter */
		}
		i = i + 1;
	}
	/* i=0..3: 4*7 = 28.  i=4..7: 4*30 = 120.  total 148 */
	if (sum != 148) { *(long *)out = 6; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 7: continue in do/while inside if/else inside while          */
	/* (the pattern that previously had a c2d decompiler bug)             */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 6) {
		if (i < 3) {
			/* even i: run do/while with continue */
			j = 0;
			do {
				j = j + 1;
				if (j == 2)
					continue;  /* skip the add for j==2 */
				sum = sum + j;
			} while (j < 4);
			/* j=1: +1, j=2: skip, j=3: +3, j=4: +4 => 8 */
		} else {
			/* odd branch: do/while with different continue */
			j = 10;
			do {
				if (j == 11) {
					j = j + 1;
					continue;
				}
				sum = sum + 1;
				j = j + 1;
			} while (j < 13);
			/* j=10: +1, j=11: skip, j=12: +1 => 2 */
		}
		i = i + 1;
	}
	/* i=0,1,2: 3*8 = 24.  i=3,4,5: 3*2 = 6.  total 30 */
	if (sum != 30) { *(long *)out = 7; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 8: break from inner loop, continue in outer loop             */
	/* ------------------------------------------------------------------ */
	sum = 0;
	count = 0;
	i = 0;
	while (i < 10) {
		i = i + 1;
		/* inner: find first j where j*i > 12, break, add that j */
		j = 0;
		while (j < 100) {
			j = j + 1;
			if (j * i > 12) {
				break;
			}
		}
		/* j is now the first value where j*i > 12 */
		if (j > 6) {
			/* if j is big, skip this outer iter */
			continue;
		}
		sum = sum + j;
		count = count + 1;
	}
	/* i=1: j=13 >6 skip.  i=2: j=7 >6 skip.  i=3: j=5 ok sum=5.
	 * i=4: j=4 ok sum=9.  i=5: j=3 ok sum=12.  i=6: j=3 ok sum=15.
	 * i=7: j=2 ok sum=17.  i=8: j=2 ok sum=19.  i=9: j=2 ok sum=21.
	 * i=10: j=2 ok sum=23.  count=8 */
	if (sum != 23) { *(long *)out = 8; return 8; }
	if (count != 8) { *(long *)out = 9; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 10: sequential loops where results feed into each other      */
	/* ------------------------------------------------------------------ */
	/* loop A: sum of 1..7 */
	sum = 0;
	i = 1;
	while (i <= 7) {
		sum = sum + i;
		i = i + 1;
	}
	/* sum = 28 */

	/* loop B: multiply sum by countdown */
	total = 0;
	i = 3;
	while (i > 0) {
		total = total + sum;
		i = i - 1;
	}
	/* total = 28 * 3 = 84 */

	/* loop C: subtract in a do/while */
	i = 0;
	do {
		total = total - 7;
		i = i + 1;
	} while (i < 4);
	/* total = 84 - 28 = 56 */

	if (total != 56) { *(long *)out = 10; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 11: do/while with complex condition involving inner-loop val */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	do {
		/* inner loop: count odd numbers up to i*2+3 */
		tmp = 0;
		j = 1;
		while (j <= i * 2 + 3) {
			if ((j / 2) * 2 != j) {
				/* j is odd */
				tmp = tmp + 1;
			}
			j = j + 1;
		}
		sum = sum + tmp;
		i = i + 1;
	} while (i < 5 && sum < 100);
	/* i=0: odds in 1..3 => 1,3 => tmp=2, sum=2
	 * i=1: odds in 1..5 => 1,3,5 => tmp=3, sum=5
	 * i=2: odds in 1..7 => 1,3,5,7 => tmp=4, sum=9
	 * i=3: odds in 1..9 => 1,3,5,7,9 => tmp=5, sum=14
	 * i=4: odds in 1..11 => 1,3,5,7,9,11 => tmp=6, sum=20 */
	if (sum != 20) { *(long *)out = 11; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 12: 4-deep nesting — while>if>do/while>while w/ all combos  */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 3) {
		if (i != 1) {
			/* level 2: do/while */
			j = 0;
			do {
				/* level 3: while */
				k = 0;
				while (k < 4) {
					/* level 4: if with continue/break */
					if (k == 1) {
						k = k + 1;
						continue; /* skip k==1 in innermost while */
					}
					if (k == 3)
						break; /* break innermost while at k==3 */
					sum = sum + 1;
					k = k + 1;
				}
				/* k=0: +1, k=1: skip, k=2: +1, k=3: break => 2 per j iter */
				j = j + 1;
			} while (j < 3);
			/* 3 j-iters * 2 = 6 */
		} else {
			/* i==1: single do/while that breaks immediately */
			j = 0;
			do {
				sum = sum + 100;
				break;
			} while (j < 10);
			/* +100 */
		}
		i = i + 1;
	}
	/* i=0: +6, i=1: +100, i=2: +6 => 112 */
	if (sum != 112) { *(long *)out = 12; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 13: deeply nested continue in do/while with flag propagation */
	/* ------------------------------------------------------------------ */
	sum = 0;
	flag = 0;
	i = 0;
	while (i < 5) {
		j = 0;
		do {
			j = j + 1;
			if (i + j == 4) {
				flag = flag + 1;
				continue; /* skip the inner while entirely */
			}
			k = 0;
			while (k < 2) {
				sum = sum + 1;
				k = k + 1;
			}
			/* +2 per j-iter that isn't skipped */
		} while (j < 3);
		i = i + 1;
	}
	/* For each (i,j) pair, skip when i+j==4:
	 * i=0: j=1,2,3 — none skip => 3*2=6
	 * i=1: j=1,2,3 — i+j=2,3,4 skip j=3 => 2*2=4
	 * i=2: j=1,2,3 — i+j=3,4,5 skip j=2 => 2*2=4
	 * i=3: j=1,2,3 — i+j=4,5,6 skip j=1 => 2*2=4
	 * i=4: j=1,2,3 — i+j=5,6,7 none skip => 3*2=6
	 * sum = 6+4+4+4+6 = 24, flag = 3 */
	if (sum != 24) { *(long *)out = 13; return 8; }
	if (flag != 3) { *(long *)out = 14; return 8; }

	/* ------------------------------------------------------------------ */
	/* CHECK 15: 5-deep nesting stress test                               */
	/* ------------------------------------------------------------------ */
	sum = 0;
	i = 0;
	while (i < 2) {
		j = 0;
		while (j < 2) {
			k = 0;
			do {
				tmp = 0;
				while (tmp < 3) {
					/* level 5: one more condition */
					if (tmp != 1) {
						sum = sum + 1;
					}
					tmp = tmp + 1;
				}
				/* tmp=0: +1, tmp=1: skip, tmp=2: +1 => 2 */
				k = k + 1;
			} while (k < 2);
			/* k: 2 iters * 2 = 4 */
			j = j + 1;
		}
		/* j: 2 iters * 4 = 8 */
		i = i + 1;
	}
	/* i: 2 * 8 = 16 */
	if (sum != 16) { *(long *)out = 15; return 8; }

	*(long *)out = 0;
	return 8;
}
