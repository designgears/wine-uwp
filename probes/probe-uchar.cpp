// Check ucrtbase's char16_t/char32_t <-> UTF-8 conversions (c16rtomb, c32rtomb, mbrtoc16, mbrtoc32)
// against known byte sequences. Shapr3D's dimension entry aborted on the unimplemented c32rtomb.
// Usage: wine probe-uchar.exe   (prints one line per check and a final PASS/FAIL)
#include <uchar.h>
#include <string.h>
#include <stdio.h>

static int failures;

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main()
{
    char buf[8];
    mbstate_t st = {};

    check(c32rtomb(buf, U'5', &st) == 1 && buf[0] == '5', "c32rtomb ascii");
    check(c32rtomb(buf, 0xe9, &st) == 2 && !memcmp(buf, "\xc3\xa9", 2), "c32rtomb 2-byte");
    check(c32rtomb(buf, 0x20ac, &st) == 3 && !memcmp(buf, "\xe2\x82\xac", 3), "c32rtomb 3-byte");
    check(c32rtomb(buf, 0x1f600, &st) == 4 && !memcmp(buf, "\xf0\x9f\x98\x80", 4), "c32rtomb 4-byte");
    check(c32rtomb(buf, 0xd800, &st) == (size_t)-1, "c32rtomb surrogate rejected");

    st = {};
    check(c16rtomb(buf, 0xd83d, &st) == 0, "c16rtomb high surrogate held");
    check(c16rtomb(buf, 0xde00, &st) == 4 && !memcmp(buf, "\xf0\x9f\x98\x80", 4), "c16rtomb pair");
    check(c16rtomb(buf, u'x', &st) == 1 && buf[0] == 'x', "c16rtomb ascii");

    char32_t c32 = 0;
    st = {};
    check(mbrtoc32(&c32, "\xe2\x82\xac", 3, &st) == 3 && c32 == 0x20ac, "mbrtoc32 3-byte");
    check(mbrtoc32(&c32, "\xe2", 1, &st) == (size_t)-2 && mbrtoc32(&c32, "\x82\xac", 2, &st) == 2 && c32 == 0x20ac,
          "mbrtoc32 split sequence");

    char16_t c16 = 0;
    st = {};
    check(mbrtoc16(&c16, "\xf0\x9f\x98\x80", 4, &st) == 4 && c16 == 0xd83d, "mbrtoc16 high half");
    check(mbrtoc16(&c16, "", 0, &st) == (size_t)-3 && c16 == 0xde00, "mbrtoc16 low half");
    check(mbrtoc16(&c16, "A", 1, &st) == 1 && c16 == u'A', "mbrtoc16 ascii");

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
