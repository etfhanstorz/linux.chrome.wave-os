// The four memory functions the compiler is allowed to call on its own (for zero-filled arrays and struct copies), even in a program
// with no C library. The volatile pointers stop the compiler from turning these loops back into calls to themselves.
void *memset(void *d, int c, unsigned long n) { volatile unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { volatile unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, unsigned long n) {
    volatile unsigned char *p = d; const unsigned char *q = s;
    if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n) { const unsigned char *p = a, *q = b; for (; n; n--, p++, q++) if (*p != *q) return *p < *q ? -1 : 1; return 0; }
