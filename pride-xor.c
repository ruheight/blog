/* https://herm1tvx.blogspot.com/2025/03/pride-and-prejudice-revisiting-pseudo.html */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <stdint.h>

uint64_t modinv(uint64_t a, unsigned n)
{
	uint64_t mod = 1ULL << n;
	int64_t t = 0, newt = 1;
	int64_t r = mod, newr = a;

	while (newr != 0) {
		int64_t q = r / newr;

		int64_t tmp = newt;
		newt = t - q * newt;
		t = tmp;

 		tmp = newr;
		newr = r - q * newr;
		r = tmp;
	}
	if (r != 1)
		return 0;
	if (t < 0)
		t += mod;
	return (uint64_t)t;
}

unsigned mod(int a, unsigned n)
{
    int r = a % (int)n;
    return (r < 0) ? (r + n) : r;
}

int main(int argc, char **argv)
{
	srandom(time(NULL));
	unsigned x0, y0, x, y, a, b, c, d, f;
	unsigned n = 1 << 4;

	x0 = random() % n;
	y0 = random() % n;
	a = (random() % n) | 1;
	b = (random() % n) & -2U;

	x = x0;
	y = y0;
	char s[n];
	bzero(s, n);
	for (int i = 0; i < n; i++) {
		x = (x + a) % n;
		y = (y + b) % n;
		f = x ^ y;
		printf("%d ", f);
		if (s[f]) {
			printf("FAILED");
			break;
		}
		s[f] = 1;
	}
	puts("");

	bzero(s, n);
	d = mod(b * modinv(a, n), n);
	c = mod(y0 - d * x0, n);
	x = x0;
	for (int i = 0; i < n; i++) {
		x = (x + a) % n;
		f = x ^ ((c + x * d) % n);
		printf("%d ", f);
		if (s[f]) {
			printf("FAILED");
			break;
		}
		s[f] = 1;
	}
	puts("");
}
