/* ESP32 mock platform - esp_crt_bundle: the computer's trusted root
 * certificates stand in for the bundle built into the device's firmware.
 * $SIM_CA_FILE (a PEM file) if set, else the system's: Windows' certificate
 * store, macOS' keychain, or the PEM bundle Linux keeps in /etc. */
#include "esp_crt_bundle.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "host.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "crt_bundle";
static mbedtls_x509_crt s_roots;
static int s_loaded;          /* 0 = not tried, 1 = ok, -1 = failed */

static bool load_pem(const char *path, bool quiet)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!quiet) ESP_LOGE(TAG, "cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n + 1);
    bool ok = false;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        buf[n] = 0;                                   /* PEM parsing needs the NUL */
        int rc = mbedtls_x509_crt_parse(&s_roots, buf, (size_t)n + 1);
        ok = rc >= 0 && s_roots.version != 0;         /* rc > 0: some skipped */
        if (!ok) ESP_LOGE(TAG, "no usable certificates in %s (%d)", path, rc);
    }
    free(buf);
    fclose(f);
    return ok;
}

static void add_der(const unsigned char *der, size_t len, void *ctx)
{
    (void)ctx;
    mbedtls_x509_crt_parse_der(&s_roots, der, len);   /* one it can't use is skipped */
}

static void load(void)
{
    mbedtls_x509_crt_init(&s_roots);
    s_loaded = -1;
    const char *path = getenv("SIM_CA_FILE");
    if (path) {
        if (load_pem(path, false)) s_loaded = 1;
        return;
    }
    if (host_root_certs(add_der, NULL)) {
        if (s_roots.version != 0) s_loaded = 1;
        else ESP_LOGE(TAG, "no usable certificates in the system's store");
        return;
    }
    static const char *const bundles[] = {
        "/etc/ssl/cert.pem",                          /* macOS, Alpine */
        "/etc/ssl/certs/ca-certificates.crt",         /* Debian, Ubuntu, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",           /* Fedora, RHEL */
    };
    for (size_t i = 0; i < sizeof bundles / sizeof bundles[0]; i++)
        if (load_pem(bundles[i], true)) { s_loaded = 1; return; }
    ESP_LOGE(TAG, "no root certificates found (set SIM_CA_FILE to a PEM bundle)");
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    if (!s_loaded) load();
    if (s_loaded < 0) return ESP_FAIL;
    mbedtls_ssl_conf_ca_chain((mbedtls_ssl_config *)conf, &s_roots, NULL);
    return ESP_OK;
}
