/*
Pride and prejudice: Revisiting pseudo-random index decryption
(x) 2025, herm1t.vx@gmail.com
https://herm1tvx.blogspot.com/2025/03/pride-and-prejudice-revisiting-pseudo.html

Almost twenty-five years ago, Mental Driller discovered an interesting technique
that allows constructing a decryption cycle in such a way that each element is
accessed in a random order [1]. The algorithm looks quite simple, but even after
it caught the attention of well-known AV researchers [2], they could not explain
why the algorithm works correctly. In the conclusion of his article, Frederic
Perriot wrote:

    I suspect the bijectivity of the family of endomorphisms studied here is
    a general property of some families of functions over well-known groups
    or fields. If you have a mathematical background and would like to share
    arithmetic insights on this problem, I'd be grateful if you can send me
    the explanation...

I suggest to check out that article to see how hard it could be to prove that
water is wet.

So, here we go.

How does the original algorithm work? We have two counters, R1 and R2, with
random initial values. Then, in a loop, we increment the first counter by one,
and the second counter by a random even number (the author uses AND -2U in the
text). After that, we XOR the two counters with each other and use the result
as an index to retrieve the value that needs to be decrypted. There is also a
restriction: the data size must be aligned to the power of two.

In C code:

    int n = 1 << 5;
    int r1 = random() % n;
    int r2 = random() % n;
    int i2 = (random() % n) & -2U;
    int i1 = 1;
    int b[n];
    for (int i = 0; i < n; i++) {
        r1 = (r1 + i1) % n;
        r2 = (r2 + i2) % n;
        b[r1 ^ r2]++;
    }
    for (int i = 0; i < n; i++)
        assert(b[i] == 1);

After the loop finishes, the array b will contain only ones, meaning the loop
has gone through all elements of the array in a random order exactly once.
A couple of years later, WhiteHead [3] paid attention to the algorithm. His
article discusses how to create similar pseudo-random generators using group
theory. He also explains how linear congruential generators (LCGs), or in
group theory terms, Z_p,*, work and explicitly references PRIDE.

However, even after that, it wasn't any clearer how a mix of random constants
and counters in the group Z_{2^n}, combined with XOR (addition in the field
GF(2^n)), leads to the correct result. At least, it wasn't clear to me.

After conducting several experiments, I noticed that the initial values of
the counters don't matter; only the increments (+1 and an even constant in the
original algorithm). The necessary and sufficient condition is that one
increment is always odd, and the other is even. The values themselves can be
arbitrary. So, the variables i1 and i2 are generators in the group Z_{2^n},+

Let's see how this looks with small examples and single generator:

    for (int n = 6; n < 9; n++) {
        printf("===Z%d\n", n);
        for (int g = 1; g < n; g++) {
            int x = 0, f = x, i;
            printf("%d: ", g);
            for (i = 0; i < n; i++) {
                printf("%d ", x);
                x = (x + g) % n;
                if (x == f)
                    break;
            }
            printf(" (%d)\n", i + 1);
        }
        puts("");
    }

===Z6 (composite)
1: 0 1 2 3 4 5  (6)
2: 0 2 4  (3)
3: 0 3  (2)
4: 0 4 2  (3)
5: 0 5 4 3 2 1  (6)

===Z7 (prime)
1: 0 1 2 3 4 5 6  (7)
2: 0 2 4 6 1 3 5  (7)
3: 0 3 6 2 5 1 4  (7)
4: 0 4 1 5 2 6 3  (7)
5: 0 5 3 1 6 4 2  (7)
6: 0 6 5 4 3 2 1  (7)

===Z8 (2^n)
1: 0 1 2 3 4 5 6 7  (8)
2: 0 2 4 6  (4)
3: 0 3 6 1 4 7 2 5  (8)
4: 0 4  (2)
5: 0 5 2 7 4 1 6 3  (8)
6: 0 6 4 2  (4)
7: 0 7 6 5 4 3 2 1  (8)

For now, we are interested in the group Z8 (2^3), and we can see that all odd
generators produce a cyclic group, they are coprime with the modulus, while all
even ones generate cyclic subgroups (whose order - number of elements - is also
a power of two). When n is prime, like in Z7, every element generates a cyclic
group. In the case of composite order (Z6), there is a subgroup for each k that
divides n. This "general property of some families of functions over well-known
groups" is known as the fundamental theorem of cyclic groups [4].

Further experimentation shows that replacing XOR with modular addition still
works, and now not only with powers of two but also with groups of other orders.
However, with certain combinations, it starts to fail:

Z6

0 1 2 3 4 5
0 2 4
0 3 0 3 0 3 -

...

0 3
0 2 4
0 5 4 3 2 1 +

Z7

0 1 2 3 4 5 6
0 4 1 5 2 6 3
0 5 3 1 6 4 2 +

0 1 2 3 4 5 6
0 6 5 4 3 2 1
0 0 0 0 0 0 0 -

In the case of Z6, we see a classic example from textbooks of direct inner sum:
Z6 = (0, 3) + (0, 2, 4) = (0, 5, 4, 3, 2, 1). But sometimes we end up in the
subgroup of lower order. The only thing left is to formulate the condition under
which the sum of two elements results in a generator of cyclic group. In the
case of Z_p, the answer is clear: the algorithm fails when i1 + i2 == p;
producing zero, in all other cases, it works correctly. In a more general case,
Z_n,+, we need to determine the order. For additive groups, the order of any
element x is

    n / gcd(x, n)

So, our condition for rejecting generator is:

    if (n / gcd(i1 + i2, n) != n)
        continue;

Or the same:

    if (gcd(i1 + i2, n) != 1)
        continue;

This explains the initial heuristics. Odd + even is always odd and thus coprime
with any power of two (in the case of Z_{2^n}), and i1 + i2 should not add up
to p for GCD to be 1 (in the case of Z_p). It's the same condition.

So why does this construction work?

Let's break it down once more in the simplest terms. Addition is commutative
(or we could say the group is abelian), so we can regroup the terms:

Ri = (S1 + I1 * i) + (S2 + I2 * i) (mod n) = (S1 + S2) + (I1 + I2) * i (mod n)

We start with some x = S1 + S2 and walk with some g = I1 + I2. Since S1 and S2
are elements of the group Z_n, their sum is also in Z_n (just by definition),
which is why the initial values of the two counters are chosen randomly and
don't affect anything. The sum (I1 + I2) is a generator in the group Z_n, and
we need to check that it generates the entire cyclic group, not one of its
subgroups or identity. It must be coprime with n, so GCD(g, n) == 1.

We can verify that it works with single variable:

    int r = (s1 + s2) % n;
    int g = (i1 + i2) % n;
    for (i = 0; i < n; i++) {
        printf("%d ", r);
        r = (r + g) % n;
    }

It's this unusual combination of constraints and heuristics that hides the
idea. It's much more simple and generic than it appears. The resulting algo
works with any N and with a wider range of values for I1 and I2. One can use
modular addition and subtraction for any i1 and i2 which passed the test
and XOR with Z_{2^n} since it's additive under XOR.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gcd(int a, int b)
{
	while (b != 0) {
		a %= b;
		a ^= b;
		b ^= a;
		a ^= b;
	}
	return a;
}

int main(int argc, char **argv)
{
	for (int n = 3; n < 30; n++) {
		printf("== N=%d ", n);
		int s1, s2, i1, i2, cc = 0;
		for (s1 = 0; s1 < n; s1++)
		for (s2 = 0; s2 < n; s2++)
		for (i1 = 1; i1 < n; i1++)
		for (i2 = 1; i2 < n; i2++) {
			if (gcd(i1 + i2, n) != 1)
				continue;
			char c[n], i;
			bzero(c, sizeof(c));
			int r1 = s1, r2 = s2;
			for (i = 0; i < n; i++) {
				int r = (r1 + r2) % n;
				c[r]++;
				r1 = (r1 + i1) % n;
				r2 = (r2 + i2) % n;
			}
			for (i = 0; i < n; i++)
				if (c[i] != 1)
					break;
			cc++;
			if (i != n) {
				printf("FAILED!\n");
				return 2;
			}
		}
		printf(" (%d variants)\n", cc);
	}
}
