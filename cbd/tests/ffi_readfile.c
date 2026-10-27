/* ffi_readfile.c — read first 5 lines of /etc/passwd via host libc
 *
 * Resolves open/read/write/close from libc via ELF link_map.
 * Runs on run_x86 -DFFI (Linux x86-64, PIE).
 *
 * Build: cpp tests/ffi_readfile.c | ./c0 | ./run tmp/c2.bin > tmp/readfile.bin
 * Run:   ./run_x86_ffi tmp/readfile.bin
 */
#include "ffi_elf.h"

long _bss_end;

long main(long in, long in_len, long out)
{
	long host_text;
	long fn_open, fn_read, fn_write, fn_close;
	long fd, nread, lines, i, ch;
	char buf[1024];
	char s_open[] = "open";
	char s_read[] = "read";
	char s_write[] = "write";
	char s_close[] = "close";
	char s_path[] = "/etc/passwd";

	_bss_end = 0;

	/* host_text passed via first 8 bytes of input by run_x86 -DFFI */
	host_text = *(long *)in;
	if (host_text == 0) {
		*(long *)out = 0;
		return 8;
	}

	fn_open = ffi_resolve(host_text, (long)&s_open);
	fn_read = ffi_resolve(host_text, (long)&s_read);
	fn_write = ffi_resolve(host_text, (long)&s_write);
	fn_close = ffi_resolve(host_text, (long)&s_close);

	if (fn_open == 0 || fn_read == 0 || fn_write == 0 || fn_close == 0) {
		*(long *)out = 1;
		return 8;
	}

	fd = fn_open((long)&s_path, 0);
	if (fd < 0) {
		*(long *)out = 2;
		return 8;
	}

	nread = fn_read(fd, (long)&buf, 1024);
	lines = 0;
	i = 0;
	while (i < nread && lines < 5) {
		ch = *(unsigned char *)((long)&buf + i);
		fn_write(1, (long)&buf + i, 1);
		if (ch == 10)
			lines = lines + 1;
		i = i + 1;
	}

	fn_close(fd);
	*(long *)out = 0;
	return 8;
}
