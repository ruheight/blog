/* t30: advanced — struct array, nested struct, ptr->s.f, pointer arithmetic,
 *      signed division corners, signed right shift */
long _bss_end;

struct point { long x; long y; };
struct inner { long v; long w; };
struct outer { struct inner i; long z; };
struct wrap  { struct point p; long tag; };

struct point gpts[4];
struct outer glob_o;

long test_struct_array(long out)
{
	long i;
	i = 0;
	while (i < 4) {
		gpts[i].x = i * 10;
		gpts[i].y = i * 10 + 1;
		i++;
	}
	if (gpts[0].x != 0)   { *(long *)out = 101; return 1; }
	if (gpts[0].y != 1)   { *(long *)out = 102; return 1; }
	if (gpts[2].x != 20)  { *(long *)out = 103; return 1; }
	if (gpts[3].y != 31)  { *(long *)out = 104; return 1; }
	i = 1;
	if (gpts[i].x != 10)  { *(long *)out = 105; return 1; }
	if (gpts[i + 1].y != 21) { *(long *)out = 106; return 1; }
	return 0;
}

long test_nested_struct(long out)
{
	struct outer o;
	o.i.v = 42;
	o.i.w = 99;
	o.z = 7;
	if (o.i.v != 42) { *(long *)out = 201; return 1; }
	if (o.i.w != 99) { *(long *)out = 202; return 1; }
	if (o.z != 7)    { *(long *)out = 203; return 1; }
	glob_o.i.v = 100;
	glob_o.i.w = 200;
	glob_o.z = 300;
	if (glob_o.i.v != 100) { *(long *)out = 204; return 1; }
	if (glob_o.i.w != 200) { *(long *)out = 205; return 1; }
	if (glob_o.z != 300)   { *(long *)out = 206; return 1; }
	return 0;
}

long test_ptr_chain(long out)
{
	struct wrap w;
	struct wrap *pw;
	w.p.x = 55;
	w.p.y = 66;
	w.tag = 77;
	pw = &w;
	if (pw->p.x != 55) { *(long *)out = 301; return 1; }
	if (pw->p.y != 66) { *(long *)out = 302; return 1; }
	if (pw->tag != 77)  { *(long *)out = 303; return 1; }
	return 0;
}

long test_ptr_arith(long out)
{
	long arr[4];
	long *p;
	long *q;
	long diff;
	arr[0] = 10;
	arr[1] = 20;
	arr[2] = 30;
	arr[3] = 40;
	p = &arr[0];
	if (*(p + 2) != 30) { *(long *)out = 501; return 1; }
	q = &arr[3];
	diff = q - p;
	if (diff != 3) { *(long *)out = 502; return 1; }
	return 0;
}

long test_signed_div(long out)
{
	long a;
	long b;
	a = -7;
	b = 2;
	if (a / b != -3) { *(long *)out = 601; return 1; }
	if (a % b != -1) { *(long *)out = 602; return 1; }
	a = 7;
	b = -2;
	if (a / b != -3) { *(long *)out = 603; return 1; }
	if (a % b != 1)  { *(long *)out = 604; return 1; }
	a = -7;
	b = -2;
	if (a / b != 3)  { *(long *)out = 605; return 1; }
	if (a % b != -1) { *(long *)out = 606; return 1; }
	return 0;
}

long test_signed_shift(long out)
{
	long neg;
	long r;
	neg = -16;
	r = neg >> 2;
	if (r != -4) { *(long *)out = 701; return 1; }
	r = (unsigned long)neg >> 2;
	if (r < 0) { *(long *)out = 702; return 1; }
	return 0;
}

long test_local_struct_array(long out)
{
	struct point arr[3];
	long i;
	i = 0;
	while (i < 3) {
		arr[i].x = i * 100;
		arr[i].y = i * 100 + 50;
		i++;
	}
	if (arr[0].x != 0)   { *(long *)out = 801; return 1; }
	if (arr[0].y != 50)  { *(long *)out = 802; return 1; }
	if (arr[1].x != 100) { *(long *)out = 803; return 1; }
	if (arr[2].y != 250) { *(long *)out = 804; return 1; }
	i = 2;
	if (arr[i].y != 250) { *(long *)out = 805; return 1; }
	return 0;
}

long test_unsigned_cmp(long out)
{
	unsigned long a;
	unsigned long b;
	a = (unsigned long)-1;
	b = 1;
	if (a < b) { *(long *)out = 901; return 1; }
	if (!(a > b)) { *(long *)out = 902; return 1; }
	if (!((long)a < (long)b)) { *(long *)out = 903; return 1; }
	return 0;
}

long main(long in, long in_len, long out)
{
	_bss_end = 0;
	if (test_struct_array(out))      return 8;
	if (test_nested_struct(out))     return 8;
	if (test_ptr_chain(out))         return 8;
	if (test_ptr_arith(out))         return 8;
	if (test_signed_div(out))        return 8;
	if (test_signed_shift(out))      return 8;
	if (test_local_struct_array(out)) return 8;
	if (test_unsigned_cmp(out))      return 8;
	*(long *)out = 0;
	return 8;
}
