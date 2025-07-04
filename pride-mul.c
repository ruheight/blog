#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <string.h>

int gcd(int a, int b)
{
	for (int t = 0; b; t = b, b = a % b, a = t)
		;
	return a;
}

int main(int argc, char **argv)
{
	unsigned a, b, c, i, x, y, n;
	unsigned o[64];

	srandom(time(NULL));
	n = argv[1] ? atoi(argv[1]) : 8;
	for (a = 1; a < n; a++)
	for (b = 1; b < n; b++) {
		/*
		for a in `seq 1 32`; do ./a.out $a|sort|uniq|wc -l; done|xargs
		GCD(b, n)	A062955 phi(n^2) - phi(n)
		GCD(a * b, n)	A127473 a(n) = phi(n)^2
		*/
		if (gcd(b, n) != 1)
			continue;
		bzero(o, sizeof(o));
		for (y = 0, i = 0; i < n; i++) {
			/* x = (a + b * i) % n; */
			x = (y + a) % n;
			y = (y + b) % n;
			o[x]++;
			printf("%d ", x);
		}
		for (i = 0; i < n; i++)
			assert(o[i] == 1);
		puts("");
	}
}
