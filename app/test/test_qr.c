#include "app_config.h"
#include "check.h"
#include "qrcodegen.h"

#include <stdio.h>

void test_qr(int *total, int *failed);

/* The QR code on an app's Marketplace screen (its README address) fits the
   room it has there, for the longest app name (23 characters). */
void test_qr(int *total, int *failed)
{
    char url[160];
    snprintf(url, sizeof url, "%s%s", APPS_README_URL, "abcdefghijklmnopqrstuvw");
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    CHECK(qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, 6, qrcodegen_Mask_AUTO, true));
    int n = qrcodegen_getSize(qr);
    CHECK(n > 0 && (n + 4) * 3 <= 130);          /* 2 modules of quiet zone around, 3 px a module */

    *total += check_total;
    *failed += check_failed;
}
