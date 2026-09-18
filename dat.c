/*
 * AI generated, example for
 *
 * https://herm1tvx.blogspot.com/2025/04/xz-backdoor-strings-tries-and-automata.html
 *
 * Double-Array Trie — int16_t base/check, slot-as-ID.
 *
 * base[s] + c = t;  check[t] = s;  check = -1 if empty.
 * Terminal state = DA slot (caller matches against known constants).
 *
 * 116 strings from CVE-2024-3094 (XZ Backdoor), including embedded NULs.
 *
 * vs XZ Backdoor bitmap trie (7136 bytes, 271 ns/lookup):
 *   DA:  5228 bytes (-26.7%)   13.9 ns/lookup (19.5x faster)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct str {
	const char	*s;
	int		len;
};

static const struct str strings[] = {
	{"xcalloc: zero size\0", 19},
	{"Could not chdir to home directory %s: %s\n\0", 42},
	{"list_hostkey_types\0", 19},
	{"demote_sensitive_data\0", 22},
	{"mm_terminate\0", 13},
	{"mm_pty_allocate\0", 16},
	{"mm_do_pam_account\0", 18},
	{"mm_session_pty_cleanup2\0", 24},
	{"mm_getpwnamallow\0", 17},
	{"mm_sshpam_init_ctx\0", 19},
	{"mm_sshpam_query\0", 16},
	{"mm_sshpam_respond\0", 18},
	{"mm_sshpam_free_ctx\0", 19},
	{"mm_choose_dh\0", 13},
	{"sshpam_respond\0", 15},
	{"sshpam_auth_passwd\0", 19},
	{"sshpam_query\0", 13},
	{"start_pam\0", 10},
	{"mm_request_send\0", 16},
	{"mm_log_handler\0", 15},
	{"Could not get agent socket\0", 27},
	{"auth_root_allowed\0", 18},
	{"mm_answer_authpassword\0", 23},
	{"mm_answer_keyallowed\0", 21},
	{"mm_answer_keyverify\0", 20},
	{"%.48s:%.48s():%d (pid=%ld)\0", 27},
	{"Unrecognized internal syslog level code %d\n\0", 44},
	{"EVP_Digest\0", 11},
	{"/usr/sbin/sshd\0", 15},
	{"EVP_DigestVerifyInit\0", 21},
	{"WAYLAND_DISPLAY=", 16},
	{"EVP_sm", 6},
	{"unknown\0", 8},
	{"Accepted publickey for ", 23},
	{"RSA_public_decrypt\0", 19},
	{"authenticating", 14},
	{"KRB5CCNAME\0", 11},
	{"__libc_start_main\0", 18},
	{"EC_KEY_get0_public_key\0", 23},
	{"EVP_DecryptFinal_ex\0", 20},
	{"__libc_stack_end\0", 17},
	{"ssh-rsa-cert-v01@openssh.com\0", 29},
	{"\x7f""ELF", 4},
	{"read\0", 5},
	{"getuid\0", 7},
	{"write\0", 6},
	{"LD_USE_LOAD_BIAS=", 17},
	{"EVP_DecryptUpdate\0", 18},
	{"password\0", 9},
	{"EVP_DigestVerify\0", 17},
	{"BN_free\0", 8},
	{"setlogmask\0", 11},
	{"malloc_usable_size\0", 19},
	{"DSA_get0_pub_key\0", 17},
	{"TERM=", 5},
	{"BN_num_bits\0", 12},
	{"preauth", 7},
	{"EVP_PKEY_set1_RSA\0", 18},
	{"RSA_set0_key\0", 13},
	{"liblzma.so", 10},
	{"_r_debug\0", 9},
	{"_rtld_global\0", 13},
	{"setresgid\0", 10},
	{" ssh2", 5},
	{"pselect\0", 8},
	{"GLRO(dl_naudit) <= naudit\0", 26},
	{"BN_bn2bin\0", 10},
	{"EC_POINT_point2oct\0", 19},
	{"EVP_PKEY_free\0", 14},
	{"%s", 2},
	{"rsa-sha2-256\0", 13},
	{"EVP_PKEY_new_raw_public_key\0", 28},
	{"shutdown\0", 9},
	{"Connection closed by ", 21},
	{"RSA_get0_key\0", 13},
	{"publickey\0", 10},
	{"libcrypto.so", 12},
	{"libc.so", 7},
	{"EC_KEY_get0_group\0", 18},
	{" from ", 6},
	{"EVP_CIPHER_CTX_new\0", 19},
	{"Accepted password for ", 22},
	{"__errno_location\0", 17},
	{"_exit\0", 6},
	{"GLIBC_2.2.5\0", 12},
	{"RSA_sign\0", 9},
	{"RSA_new\0", 8},
	{"libsystemd.so", 13},
	{"mm_answer_pam_start\0", 20},
	{"BN_dup\0", 7},
	{"DISPLAY=", 8},
	{"SSH-2.0", 7},
	{"_dl_audit_symbind_alt\0", 22},
	{"DSA_get0_pqg\0", 13},
	{"system\0", 7},
	{"ld-linux-x86-64.so", 18},
	{"_dl_audit_preinit\0", 18},
	{"LINES=", 6},
	{"LD_DEBUG=", 9},
	{"_rtld_global_ro\0", 16},
	{"setresuid\0", 10},
	{"RSA_free\0", 9},
	{"EVP_MD_CTX_new\0", 15},
	{"yolAbejyiejuvnup=Evjtgvsh5okmkAvj\0", 34},
	{"user", 4},
	{"EVP_CIPHER_CTX_free\0", 20},
	{"LD_PROFILE=", 11},
	{"LD_BIND_NOT=", 12},
	{"EVP_DecryptInit_ex\0", 19},
	{"EVP_chacha20\0", 13},
	{"BN_bin2bn\0", 10},
	{"parse PAM\0", 10},
	{"EVP_sha256\0", 11},
	{"LD_AUDIT=", 9},
	{"ssh-2.0", 7},
	{"EVP_MD_CTX_free\0", 16},
};

#define N_STR	(sizeof(strings) / sizeof(strings[0]))

/* build-time plain trie */

#define ALPHA		128
#define MAX_TNODES	2048

struct tnode {
	int	children[ALPHA];
	int	is_term;
};

static struct tnode tnodes[MAX_TNODES];
static int tn_cnt;

static int tnode_new(void)
{
	int id = tn_cnt++;

	memset(tnodes[id].children, 0xFF, sizeof(tnodes[id].children));
	tnodes[id].is_term = 0;
	return id;
}

static void trie_insert(const char *s, int len)
{
	int cur = 0;
	int i;

	for (i = 0; i < len; i++) {
		uint8_t c = (uint8_t)s[i];

		if (tnodes[cur].children[c] < 0)
			tnodes[cur].children[c] = tnode_new();
		cur = tnodes[cur].children[c];
	}
	tnodes[cur].is_term = 1;
}

/* double array (int16_t base/check, -1 = empty) */

#define DA_CAP		16384

static int16_t da_base[DA_CAP];
static int16_t da_check[DA_CAP];
static int da_size;

static void da_init(void)
{
	int i;

	memset(da_base, 0, sizeof(da_base));
	for (i = 0; i < DA_CAP; i++)
		da_check[i] = -1;
	da_size = 0;
}

/* find base so that base+c is free for every child c of node */
static int da_find_base(int node)
{
	uint8_t kids[ALPHA];
	int nk = 0;
	int b, i, t;

	for (i = 0; i < ALPHA; i++)
		if (tnodes[node].children[i] >= 0)
			kids[nk++] = (uint8_t)i;
	if (nk == 0)
		return 0;

	for (b = 1; b < DA_CAP - ALPHA; b++) {
		int ok = 1;

		for (i = 0; i < nk && ok; i++) {
			t = b + kids[i];
			if (t >= DA_CAP || da_check[t] >= 0)
				ok = 0;
		}
		if (ok)
			return b;
	}
	fprintf(stderr, "da_find_base: no room\n");
	exit(1);
}

/* BFS build, returns terminal count */
static int da_build(void)
{
	int queue[MAX_TNODES], qi[MAX_TNODES];
	int qh = 0, qt = 0;
	int terms = 0;

	da_init();
	queue[qt] = 0;
	qi[qt] = 0;
	qt++;
	da_check[0] = 0;
	da_size = 1;

	while (qh < qt) {
		int nd = queue[qh];
		int st = qi[qh];
		int c, base, has_kids = 0;

		qh++;
		if (tnodes[nd].is_term)
			terms++;

		for (c = 0; c < ALPHA; c++)
			if (tnodes[nd].children[c] >= 0) {
				has_kids = 1;
				break;
			}
		if (!has_kids) {
			da_base[st] = 0;
			continue;
		}

		base = da_find_base(nd);
		da_base[st] = (int16_t)base;

		for (c = 0; c < ALPHA; c++) {
			int child = tnodes[nd].children[c];
			int t;

			if (child < 0)
				continue;
			t = base + c;
			da_check[t] = (int16_t)st;
			if (t >= da_size)
				da_size = t + 1;
			queue[qt] = child;
			qi[qt] = t;
			qt++;
		}
	}
	return terms;
}

/* matcher: returns state (slot-as-ID) or -1 on mismatch */
static int16_t da_match(const char *s, int len)
{
	int16_t st = 0;
	int i;

	for (i = 0; i < len; i++) {
		uint8_t c = (uint8_t)s[i];
		int t = da_base[st] + c;

		if (t < 0 || t >= da_size || da_check[t] != st)
			return -1;
		st = (int16_t)t;
	}
	return st;
}

/* generator: DFS prints all stored strings */

static int term_map[DA_CAP];

static void gen_dfs(int16_t st, uint8_t *buf, int depth)
{
	int c, k;
	int16_t b;
	int t;

	if (term_map[st]) {
		printf("  [%4d] \"", st);
		for (k = 0; k < depth; k++) {
			uint8_t ch = buf[k];

			if (ch >= 0x20 && ch < 0x7f)
				putchar(ch);
			else
				printf("\\x%02x", ch);
		}
		printf("\"\n");
	}

	b = da_base[st];
	if (b == 0 && st != 0)
		return;

	for (c = 0; c < ALPHA; c++) {
		t = b + c;
		if (t >= 0 && t < da_size && da_check[t] == st) {
			buf[depth] = (uint8_t)c;
			gen_dfs((int16_t)t, buf, depth + 1);
		}
	}
}

/* hex dump */
static void hex_dump(const char *label, const int16_t *arr, int n)
{
	int i, j;

	printf("\n%s  (%d entries = %d bytes)\n", label, n, n * 2);
	for (i = 0; i < n; i += 16) {
		printf("  %04x: ", i);
		for (j = 0; j < 16 && i + j < n; j++)
			printf("%04x", (uint16_t)arr[i + j]);
		for (; j < 16; j++)
			printf("    ");
		printf("  ");
		for (j = 0; j < 16 && i + j < n; j++) {
			uint8_t lo = (uint16_t)arr[i + j] & 0xFF;
			uint8_t hi = (uint16_t)arr[i + j] >> 8;

			putchar((lo >= 0x20 && lo < 0x7f) ? lo : '.');
			putchar((hi >= 0x20 && hi < 0x7f) ? hi : '.');
		}
		printf("\n");
	}
}

int main(void)
{
	int i, ok = 1, terms;

	tnode_new();
	for (i = 0; i < (int)N_STR; i++)
		trie_insert(strings[i].s, strings[i].len);
	printf("plain trie: %d strings, %d nodes\n", (int)N_STR, tn_cnt);

	terms = da_build();
	printf("double array: %d states, %d terminals\n", da_size, terms);
	printf("  base[]:  %5d bytes\n", da_size * 2);
	printf("  check[]: %5d bytes\n", da_size * 2);
	printf("  total:   %5d bytes\n", da_size * 4);

	memset(term_map, 0, sizeof(term_map));
	for (i = 0; i < (int)N_STR; i++) {
		int16_t s = da_match(strings[i].s, strings[i].len);

		if (s >= 0)
			term_map[s] = 1;
	}

	printf("\npositive lookups:\n");
	for (i = 0; i < (int)N_STR; i++) {
		int16_t s = da_match(strings[i].s, strings[i].len);

		if (s < 0) {
			printf("  FAIL: [%d]\n", i);
			ok = 0;
		}
	}
	if (ok)
		printf("  %d/%d OK\n", (int)N_STR, (int)N_STR);

	printf("\nnegative lookups:\n");
	{
		const char *neg[] = {
			"", "x", "mm_", "EVP_D", "foobar",
			"sshpam_query2", NULL
		};

		for (i = 0; neg[i]; i++) {
			int16_t s = da_match(neg[i], strlen(neg[i]));

			if (s >= 0 && term_map[s]) {
				printf("  FALSE POS: \"%s\"\n", neg[i]);
				ok = 0;
			} else {
				printf("  reject \"%s\"\n", neg[i]);
			}
		}
	}
	printf("\nresult: %s\n", ok ? "PASS" : "FAIL");

	printf("\ngenerated strings:\n");
	{
		uint8_t buf[512];

		gen_dfs(0, buf, 0);
	}

	hex_dump("base[]", da_base, da_size);
	hex_dump("check[]", da_check, da_size);

	return ok ? 0 : 1;
}
