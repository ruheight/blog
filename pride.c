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
