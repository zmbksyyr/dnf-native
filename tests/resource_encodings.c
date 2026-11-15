/* Exercise the encoding modules retained in the delivered native ZIP. */
typedef __SIZE_TYPE__ size_t;
extern void *iconv_open(const char *to, const char *from);
extern size_t iconv(void *cd, char **input, size_t *left, char **output, size_t *space);
extern int iconv_close(void *cd);
extern int memcmp(const void *, const void *, size_t);

__attribute__((noreturn)) static void finish(int status) {
    __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
    __builtin_unreachable();
}
#define CHECK(value) do { if (!(value)) finish(__LINE__); } while (0)

static void convert(const char *from, const char *to, const char *input, size_t bytes,
                    const char *expected, size_t count) {
    void *cd = iconv_open(to, from);
    CHECK(cd != (void *)-1);
    char buffer[32], *in = (char *)input, *out = buffer;
    size_t left = bytes, space = sizeof(buffer);
    CHECK(iconv(cd, &in, &left, &out, &space) == 0);
    CHECK(left == 0 && (size_t)(out - buffer) == count && !memcmp(buffer, expected, count));
    CHECK(iconv_close(cd) == 0);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const struct { const char *name, *bytes, *utf8; size_t count; } cases[] = {
        {"GBK", "\xba\xba", "\xe6\xb1\x89", 2},
        {"GB18030", "\xba\xba", "\xe6\xb1\x89", 2},
        {"BIG5", "\xba\x7e", "\xe6\xbc\xa2", 2},
        {"BIG5-HKSCS", "\xba\x7e", "\xe6\xbc\xa2", 2},
        {"EUC-KR", "\xc7\xd1", "\xed\x95\x9c", 2},
        {"UHC", "\xc7\xd1", "\xed\x95\x9c", 2}
    };
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        convert(cases[n].name, "UTF-8", cases[n].bytes, cases[n].count, cases[n].utf8, 3);
        convert("UTF-8", cases[n].name, cases[n].utf8, 3, cases[n].bytes, cases[n].count);
    }
    finish(42);
}
