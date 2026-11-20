#include <string.h>

#include "b64.h"
#include "check.h"

void test_b64(int *total, int *failed);

/* Base64 both ways, both alphabets (crypto.base64, the USB tether). */
void test_b64(int *total, int *failed)
{
    char t[32];
    uint8_t b[16];
    CHECK(b64_encode("", 0, t, false) == 0 && !strcmp(t, ""));
    CHECK(b64_encode("f", 1, t, false) == 4 && !strcmp(t, "Zg=="));
    CHECK(b64_encode("fo", 2, t, false) == 4 && !strcmp(t, "Zm8="));
    CHECK(b64_encode("foobar", 6, t, false) == 8 && !strcmp(t, "Zm9vYmFy"));
    CHECK(b64_encode("\xfb\xff", 2, t, true) == 3 && !strcmp(t, "-_8"));
    for (size_t n = 0; n < 10; n++) {
        CHECK(b64_encoded_len(n, false) == b64_encode("abcdefghij", n, t, false));
        CHECK(b64_encoded_len(n, true) == b64_encode("abcdefghij", n, t, true));
    }
    CHECK(b64_decode("Zm9vYmFy", 8, b, sizeof b) == 6 && !memcmp(b, "foobar", 6));
    CHECK(b64_decode("Zm8=", 4, b, sizeof b) == 2 && !memcmp(b, "fo", 2));
    CHECK(b64_decode("-_8", 3, b, sizeof b) == 2 && b[0] == 0xfb && b[1] == 0xff);
    CHECK(b64_decode("Zm9v\nYmFy", 9, b, sizeof b) == 6);
    CHECK(b64_decode("Zm9v*", 5, b, sizeof b) == -1);
    CHECK(b64_decode("Zm9vYmFy", 8, b, 5) == -1);
    CHECK(b64_decode("Zm9vYmFy", 8, b, 6) == 6);

    *total += check_total;
    *failed += check_failed;
}
