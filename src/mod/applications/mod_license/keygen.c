/*
 * keygen.c — Offline license key generator for mod_license
 *
 * VENDOR USE ONLY.  This tool is never distributed to customers.
 * Keep it (and the HMAC secret) in your private build environment.
 *
 * Build (MSVC):
 *   cl keygen.c bcrypt.lib /Fe:keygen.exe
 *
 * Build (MinGW/GCC on Windows):
 *   gcc keygen.c -o keygen.exe -lbcrypt
 *
 * Usage:
 *   keygen.exe --machine-id <64-char hex ID> [--days <N>] [--flags <hex>]
 *
 * Examples:
 *   # Perpetual license
 *   keygen.exe --machine-id a3f1...
 *
 *   # 1-year trial
 *   keygen.exe --machine-id a3f1... --days 365
 *
 *   # Feature-flagged, 90-day
 *   keygen.exe --machine-id a3f1... --days 90 --flags 00000003
 *
 * The generated key is printed to stdout.  Email or deliver it to the
 * customer; they save it as license.key in the FreeSWITCH conf dir.
 *
 * To obtain the machine ID before generating a key:
 *   1. Load mod_license on the target machine (it will log the machine ID
 *      even if no key is present yet).
 *   2. Or run: fs_cli -x "license_info"
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  error "This keygen targets Windows.  For cross-platform generation,"
#  error "replace CNG with OpenSSL HMAC-SHA256 (same algorithm/format)."
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* Must match the secret compiled into mod_license.c exactly. */
#define VENDOR_HMAC_SECRET  "CHANGE_THIS_SECRET_BEFORE_SHIPPING_v1"
#define SECRET_LEN          (sizeof(VENDOR_HMAC_SECRET) - 1)

/* -----------------------------------------------------------------------
 * CNG HMAC-SHA256 (same implementation as mod_license.c)
 * ----------------------------------------------------------------------- */
static int cng_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t out[32])
{
    BCRYPT_ALG_HANDLE  hAlg  = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    int ok = 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                    NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, NULL, 0,
                                         (PUCHAR)key, (ULONG)key_len, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptHashData(hHash, (PUCHAR)msg, (ULONG)msg_len, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hHash, out, 32, 0)))
        goto done;
    ok = 1;
done:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

/* -----------------------------------------------------------------------
 * Base64 encode (same table as mod_license.c)
 * ----------------------------------------------------------------------- */
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t in_len, char *out)
{
    size_t i = 0, j = 0;
    for (; i + 2 < in_len; i += 3) {
        out[j++] = B64_TABLE[ in[i]          >> 2];
        out[j++] = B64_TABLE[(in[i]   & 0x03) << 4 | in[i+1] >> 4];
        out[j++] = B64_TABLE[(in[i+1] & 0x0F) << 2 | in[i+2] >> 6];
        out[j++] = B64_TABLE[ in[i+2] & 0x3F];
    }
    if (i < in_len) {
        out[j++] = B64_TABLE[in[i] >> 2];
        if (i + 1 < in_len) {
            out[j++] = B64_TABLE[(in[i] & 0x03) << 4 | in[i+1] >> 4];
            out[j++] = B64_TABLE[(in[i+1] & 0x0F) << 2];
        } else {
            out[j++] = B64_TABLE[(in[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *machine_id = NULL;
    long        days        = 0;    /* 0 = perpetual */
    const char *flags_str   = "00000000";

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--machine-id") && i + 1 < argc) {
            machine_id = argv[++i];
        } else if (!strcmp(argv[i], "--days") && i + 1 < argc) {
            days = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--flags") && i + 1 < argc) {
            flags_str = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: keygen --machine-id <hex64> [--days N] [--flags hex]\n");
            return 0;
        }
    }

    if (!machine_id) {
        fprintf(stderr,
                "Error: --machine-id is required.\n\n"
                "To get the target machine's ID:\n"
                "  1. Load mod_license on the target (it logs the ID on startup).\n"
                "  2. Or run:  fs_cli -x \"license_info\"\n\n"
                "Usage: keygen --machine-id <64-char hex> [--days N] [--flags hex]\n");
        return 1;
    }

    /* Validate machine_id length */
    size_t mid_len = strlen(machine_id);
    if (mid_len != 64) {
        fprintf(stderr, "Error: machine-id must be exactly 64 hex characters (got %zu)\n",
                mid_len);
        return 1;
    }

    /* Compute expiry */
    time_t expiry = 0;
    if (days > 0) {
        expiry = time(NULL) + (time_t)days * 86400;
    }

    /* Build payload string */
    char payload[512];
    snprintf(payload, sizeof(payload), "%s:%llu:%s",
             machine_id, (unsigned long long)expiry, flags_str);

    /* Compute HMAC-SHA256 over payload */
    uint8_t hmac[32];
    if (!cng_hmac_sha256((const uint8_t *)VENDOR_HMAC_SECRET, SECRET_LEN,
                         (const uint8_t *)payload, strlen(payload), hmac)) {
        fprintf(stderr, "Error: HMAC computation failed\n");
        return 1;
    }

    /* Base64 encode both parts */
    char b64_payload[768], b64_hmac[64];
    b64_encode((const uint8_t *)payload, strlen(payload), b64_payload);
    b64_encode(hmac, 32, b64_hmac);

    /* Print the license key */
    printf("%s.%s\n", b64_payload, b64_hmac);

    /* Print human-readable summary to stderr so piping the key is clean */
    fprintf(stderr, "\n--- Key generated ---\n");
    fprintf(stderr, "Machine ID : %s\n", machine_id);
    fprintf(stderr, "Expiry     : ");
    if (expiry == 0) {
        fprintf(stderr, "Never (perpetual)\n");
    } else {
        char tbuf[64];
        struct tm *tm_info = localtime(&expiry);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d", tm_info);
        fprintf(stderr, "%s (in %ld days)\n", tbuf, days);
    }
    fprintf(stderr, "Flags      : 0x%s\n", flags_str);
    fprintf(stderr, "\nSave the key above to license.key in the FreeSWITCH conf dir.\n");

    return 0;
}
