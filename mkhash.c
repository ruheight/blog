/* Mirotik hash, herm1t.vx@gmail.com, 2024

Curve25519(SHA256(salt | SHA256(user ":" password)), { 9 })

./hash user3 2233 8f07611dc3f0fab5454a8db2a9cd200e
4017fdbe449ea84a285286c9da132fa5c17092197a17819e81574d06ab84064f
*/
#include <stdio.h>
#include <sys/time.h>

#include "sha256.h"
#include "sha256.c"

#include "curve25519-donna.c"
/*
Mikrotik removed cofactor cleaning and MSB-bit set

--- curve25519-donna.c.orig	2025-10-30 12:41:34.592965814 +0200
+++ curve25519-donna.c	2025-10-30 12:41:53.292499763 +0200
@@ -847,9 +847,6 @@
   int i;

   for (i = 0; i < 32; ++i) e[i] = secret[i];
-  e[0] &= 248;
-  e[31] &= 127;
-  e[31] |= 64;

   fexpand(bp, basepoint);
   cmult(x, z, e, bp);
*/

int main(int argc, char **argv)
{
	unsigned char verifier[32], base[32] = { 9 }, result[32];

	unsigned char salt[16];
	for (int i = 0; i <16; i++) {
		int byte;
		sscanf(argv[3] + i * 2, "%02x", &byte);
		salt[i] = (unsigned char)byte;
	}

	unsigned char hash[32];
	SHA256_CTX ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, (unsigned char*)argv[1], strlen(argv[1]));
	sha256_update(&ctx, (unsigned char*)":", 1);
	sha256_update(&ctx, (unsigned char*)argv[2], strlen(argv[2]));
	sha256_final(&ctx, hash);
	sha256_init(&ctx);
	sha256_update(&ctx, salt, 16);
	sha256_update(&ctx, hash, 32);
	sha256_final(&ctx, hash);

	for (int i = 0; i < 32; i++)
		result[i] = hash[31 - i];

	curve25519_donna(&verifier[0], &result[0], &base[0]);

	for (int i = 0; i < 32; i++)
		result[i] = verifier[31 - i];

	for (int i = 0; i < 32; i++)
		printf("%02x", result[i]);
	puts("");
	return 0;
}
