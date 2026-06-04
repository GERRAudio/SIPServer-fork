/*
 * mod_license.c — FreeSwitch licensing module -GERRAudio
 *
 * Binds a FreeSwitch instance to a specific Windows 10/11 machine.
 * License keys are generated offline by the vendor and installed by
 * the customer.  No network access is required for validation.
 *
 * Build:
 *   Place in freeswitch/src/mod/applications/mod_license/
 *   Run: make install  (standard FreeSWITCH module build)
 *
 * Dependencies:
 *   Windows: bcrypt.lib (CNG), iphlpapi.lib, wbemuuid.lib (WMI)
 *   OpenSSL is NOT required — we use Windows CNG for HMAC-SHA256.
 *
 * Configuration (autoload_configs/license.conf.xml):
 *   <configuration name="license.conf" description="License">
 *     <settings>
 *       <param name="license_file" value="$${conf_dir}/license.key"/>
 *     </settings>
 *   </configuration>
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>        /* CNG HMAC-SHA256            */
#  include <iphlpapi.h>      /* GetAdaptersInfo            */
#  include <wbemidl.h>       /* WMI (CPU / board serial)   */
#  include <objbase.h>
#  pragma comment(lib, "bcrypt.lib")
#  pragma comment(lib, "iphlpapi.lib")
#  pragma comment(lib, "wbemuuid.lib")
#  pragma comment(lib, "ole32.lib")
#  pragma comment(lib, "oleaut32.lib")
#else
#  error "mod_license is only supported on Windows 10/11"
#endif

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * Vendor secret — change this to a value only you know.
 * Keep it out of public source trees; embed it at compile time.
 * ----------------------------------------------------------------------- */
#define VENDOR_HMAC_SECRET   "CHANGE_THIS_SECRET_BEFORE_SHIPPING_v1"
#define SECRET_LEN           (sizeof(VENDOR_HMAC_SECRET) - 1)

/* Key file format (plain text, one line):
 *
 *   <base64(machine_id_hex:expiry_epoch:flags)>.<base64(hmac)>
 *
 * Fields
 *   machine_id_hex : 64 hex chars (SHA-256 of hardware fingerprint)
 *   expiry_epoch   : Unix timestamp (0 = no expiry)
 *   flags          : 32-bit hex feature flags (reserved, set to 00000000)
 */
#define LICENSE_FILE_DEFAULT "license.key"
#define MACHINE_ID_HEX_LEN   64   /* SHA-256 = 32 bytes = 64 hex chars */
#define MAX_KEY_FILE_BYTES   4096
#define SWITCH_PATH_MAX      256

SWITCH_MODULE_LOAD_FUNCTION(mod_license_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_license_shutdown);
SWITCH_MODULE_DEFINITION(mod_license, mod_license_load, mod_license_shutdown, NULL);

/* -----------------------------------------------------------------------
 * Internal state
 * ----------------------------------------------------------------------- */
static struct {
    char license_file[SWITCH_PATH_MAX];
    char machine_id[MACHINE_ID_HEX_LEN + 1];
    int  licensed;
} globals;

/* -----------------------------------------------------------------------
 * Utility: simple hex encoding
 * ----------------------------------------------------------------------- */
static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = h[bytes[i] >> 4];
        out[i * 2 + 1] = h[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* -----------------------------------------------------------------------
 * Utility: base64 encode / decode (RFC 4648, no line wrapping)
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

static int b64_decode(const char *in, uint8_t *out, size_t *out_len)
{
    static const int8_t dtable[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
        ['=']=0
    };
    size_t in_len = strlen(in);
    if (in_len % 4 != 0) return 0;
    *out_len = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        uint32_t t = (uint32_t)dtable[(uint8_t)in[i]]   << 18
                   | (uint32_t)dtable[(uint8_t)in[i+1]] << 12
                   | (uint32_t)dtable[(uint8_t)in[i+2]] << 6
                   | (uint32_t)dtable[(uint8_t)in[i+3]];
        out[(*out_len)++] = (t >> 16) & 0xFF;
        if (in[i+2] != '=') out[(*out_len)++] = (t >> 8) & 0xFF;
        if (in[i+3] != '=') out[(*out_len)++] = t & 0xFF;
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * Windows CNG: SHA-256 hash
 * ----------------------------------------------------------------------- */
static int cng_sha256(const uint8_t *data, size_t data_len, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE hAlg   = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS st;
    DWORD hash_len = 0, result = 0;
    int ok = 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&hash_len, sizeof(hash_len), &result, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0)))
        goto done;
    if (!BCRYPT_SUCCESS(BCryptHashData(hHash, (PUCHAR)data, (ULONG)data_len, 0)))
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
 * Windows CNG: HMAC-SHA256
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
 * WMI helper: query a single string property from a WMI class
 * Returns 1 on success, 0 on failure.  out must be at least out_size bytes.
 * ----------------------------------------------------------------------- */
static int wmi_get_string(const wchar_t *wmi_class, const wchar_t *property,
                          char *out, size_t out_size)
{
    IWbemLocator   *pLoc  = NULL;
    IWbemServices  *pSvc  = NULL;
    IEnumWbemClassObject *pEnum = NULL;
    IWbemClassObject     *pObj  = NULL;
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void**)&pLoc);
    if (FAILED(hr)) goto done;

    hr = pLoc->lpVtbl->ConnectServer(pLoc,
        L"ROOT\\CIMV2", NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
    if (FAILED(hr)) goto done;

    hr = CoSetProxyBlanket((IUnknown*)pSvc,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) goto done;

    {
        wchar_t query[256];
        swprintf(query, 256, L"SELECT %s FROM %s", property, wmi_class);
        hr = pSvc->lpVtbl->ExecQuery(pSvc,
            L"WQL", query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            NULL, &pEnum);
    }
    if (FAILED(hr)) goto done;

    {
        ULONG returned = 0;
        hr = pEnum->lpVtbl->Next(pEnum, WBEM_INFINITE, 1, &pObj, &returned);
        if (FAILED(hr) || returned == 0) goto done;

        VARIANT var;
        VariantInit(&var);
        hr = pObj->lpVtbl->Get(pObj, property, 0, &var, NULL, NULL);
        if (SUCCEEDED(hr) && var.vt == VT_BSTR && var.bstrVal) {
            WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1,
                                out, (int)out_size, NULL, NULL);
            ok = 1;
        }
        VariantClear(&var);
        pObj->lpVtbl->Release(pObj);
    }
done:
    if (pEnum) pEnum->lpVtbl->Release(pEnum);
    if (pSvc)  pSvc->lpVtbl->Release(pSvc);
    if (pLoc)  pLoc->lpVtbl->Release(pLoc);
    return ok;
}

/* -----------------------------------------------------------------------
 * Build machine fingerprint and return as 64-char hex SHA-256 string.
 *
 * Sources combined (any individual source may fail; overall hash still
 * provides a stable per-machine value):
 *   1. CPU ProcessorId  (WMI Win32_Processor)
 *   2. Motherboard serial (WMI Win32_BaseBoard)
 *   3. First non-loopback MAC address (IP Helper)
 *   4. System volume serial (GetVolumeInformationW on C:\)
 * ----------------------------------------------------------------------- */
static int build_machine_id(char out_hex[MACHINE_ID_HEX_LEN + 1])
{
    char buf[2048] = {0};
    char tmp[256]  = {0};
    size_t pos     = 0;

    /* 1. CPU ID */
    if (wmi_get_string(L"Win32_Processor", L"ProcessorId", tmp, sizeof(tmp))) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "CPU:%s|", tmp);
    }

    /* 2. Motherboard serial */
    memset(tmp, 0, sizeof(tmp));
    if (wmi_get_string(L"Win32_BaseBoard", L"SerialNumber", tmp, sizeof(tmp))) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "BOARD:%s|", tmp);
    }

    /* 3. First MAC address */
    {
        PIP_ADAPTER_INFO pAdapters = NULL;
        ULONG out_len = 0;
        if (GetAdaptersInfo(NULL, &out_len) == ERROR_BUFFER_OVERFLOW) {
            pAdapters = (PIP_ADAPTER_INFO)malloc(out_len);
            if (pAdapters && GetAdaptersInfo(pAdapters, &out_len) == NO_ERROR) {
                PIP_ADAPTER_INFO cur = pAdapters;
                while (cur) {
                    /* Skip loopback / software adapters with all-zero MACs */
                    uint8_t zero[6] = {0};
                    if (cur->AddressLength == 6 &&
                        memcmp(cur->Address, zero, 6) != 0 &&
                        cur->Type != MIB_IF_TYPE_LOOPBACK) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "MAC:%02X%02X%02X%02X%02X%02X|",
                            cur->Address[0], cur->Address[1], cur->Address[2],
                            cur->Address[3], cur->Address[4], cur->Address[5]);
                        break;
                    }
                    cur = cur->Next;
                }
            }
            free(pAdapters);
        }
    }

    /* 4. System volume serial (C:\) */
    {
        DWORD vol_serial = 0;
        if (GetVolumeInformationW(L"C:\\", NULL, 0, &vol_serial, NULL, NULL, NULL, 0)) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "VOL:%08X", vol_serial);
        }
    }

    if (pos == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: could not collect any hardware identifiers\n");
        return 0;
    }

    uint8_t hash[32];
    if (!cng_sha256((const uint8_t *)buf, pos, hash)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: SHA-256 failed\n");
        return 0;
    }

    bytes_to_hex(hash, 32, out_hex);
    return 1;
}

/* -----------------------------------------------------------------------
 * Validate a license key string against the current machine.
 *
 * Key format:  <base64_payload>.<base64_hmac>
 *
 * Payload (decoded, colon-delimited):
 *   machine_id_hex:expiry_epoch:flags_hex
 * ----------------------------------------------------------------------- */
static int validate_license_key(const char *key_str, const char *machine_id)
{
    /* Split on the last '.' */
    char key_copy[MAX_KEY_FILE_BYTES];
    strncpy(key_copy, key_str, sizeof(key_copy) - 1);
    key_copy[sizeof(key_copy) - 1] = '\0';

    /* Trim trailing whitespace / newline */
    for (int i = (int)strlen(key_copy) - 1; i >= 0; i--) {
        if (key_copy[i] == '\n' || key_copy[i] == '\r' || key_copy[i] == ' ')
            key_copy[i] = '\0';
        else break;
    }

    char *dot = strrchr(key_copy, '.');
    if (!dot) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: malformed key (no '.' separator)\n");
        return 0;
    }
    *dot = '\0';
    const char *b64_payload = key_copy;
    const char *b64_hmac    = dot + 1;

    /* Decode payload */
    uint8_t payload_bytes[512];
    size_t  payload_len = 0;
    if (!b64_decode(b64_payload, payload_bytes, &payload_len) || payload_len == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: base64 decode of payload failed\n");
        return 0;
    }
    payload_bytes[payload_len] = '\0';

    /* Decode HMAC */
    uint8_t stored_hmac[64];
    size_t  hmac_len = 0;
    if (!b64_decode(b64_hmac, stored_hmac, &hmac_len) || hmac_len != 32) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: base64 decode of HMAC failed (got %zu bytes)\n", hmac_len);
        return 0;
    }

    /* Recompute HMAC over the raw payload bytes */
    uint8_t computed_hmac[32];
    if (!cng_hmac_sha256((const uint8_t *)VENDOR_HMAC_SECRET, SECRET_LEN,
                         payload_bytes, payload_len, computed_hmac)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: HMAC computation failed\n");
        return 0;
    }

    /* Constant-time compare to resist timing attacks */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= stored_hmac[i] ^ computed_hmac[i];
    if (diff != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: HMAC signature mismatch — key is invalid or tampered\n");
        return 0;
    }

    /* Parse payload fields: machine_id:expiry:flags */
    char payload_str[512];
    memcpy(payload_str, payload_bytes, payload_len);
    payload_str[payload_len] = '\0';

    char *p = payload_str;
    char *key_machine_id = strtok(p, ":");
    char *expiry_str     = strtok(NULL, ":");
    char *flags_str      = strtok(NULL, ":");

    if (!key_machine_id || !expiry_str || !flags_str) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: payload missing required fields\n");
        return 0;
    }

    /* Check machine binding */
    if (strcmp(key_machine_id, machine_id) != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: machine ID mismatch\n"
                          "  Expected : %s\n  In key   : %s\n",
                          machine_id, key_machine_id);
        return 0;
    }

    /* Check expiry (0 = perpetual) */
    time_t expiry = (time_t)strtoull(expiry_str, NULL, 10);
    if (expiry != 0 && time(NULL) > expiry) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_license: license expired\n");
        return 0;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_license: license valid (flags=0x%s, expiry=%s)\n",
                      flags_str, expiry == 0 ? "never" : expiry_str);
    return 1;
}

/* -----------------------------------------------------------------------
 * API command: license_info
 *   Prints machine ID and license status to the console / ESL.
 * ----------------------------------------------------------------------- */
SWITCH_STANDARD_API(license_info_api)
{
    if (globals.machine_id[0]) {
        stream->write_function(stream,
            "+OK\nMachine-ID : %s\nLicensed   : %s\n",
            globals.machine_id,
            globals.licensed ? "yes" : "NO");
    } else {
        stream->write_function(stream, "-ERR machine ID not available\n");
    }
    return SWITCH_STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Module load
 *
 * Design note on ordering:
 *   SWITCH_ADD_API is called FIRST, before any license check.  This
 *   ensures "license_info" is always reachable via fs_cli even when no
 *   key file is present yet — which is exactly the state a new customer
 *   is in when they need to retrieve their machine ID.
 *
 *   If the fingerprint cannot be built (catastrophic WMI/CNG failure)
 *   we still return SWITCH_STATUS_TERM, but that is an environmental
 *   error unrelated to normal activation.
 *
 *   For missing / invalid keys the module loads successfully but sets
 *   globals.licensed = 0 and calls switch_core_set_variable() so other
 *   modules (and ESL scripts) can gate on "license_status".  The
 *   shutdown is deferred: FreeSwitch itself is not killed here; instead
 *   any module that should be restricted checks the variable.  If you
 *   want hard shutdown on bad key, uncomment the SWITCH_STATUS_TERM
 *   block marked below.
 * ----------------------------------------------------------------------- */
SWITCH_MODULE_LOAD_FUNCTION(mod_license_load)
{
    switch_api_interface_t *api_interface;
    switch_xml_t cfg, xml, settings, param;

    *module_interface = switch_loadable_module_create_module_interface(
        pool, modname);

    memset(&globals, 0, sizeof(globals));
    snprintf(globals.license_file, sizeof(globals.license_file),
             "%s/%s", SWITCH_GLOBAL_dirs.conf_dir, LICENSE_FILE_DEFAULT);

    /* ------------------------------------------------------------------
     * STEP 1: Register the API command immediately.
     * This must happen before any early-return so that fs_cli -x
     * "license_info" works even when no key has been installed yet.
     * ------------------------------------------------------------------ */
    SWITCH_ADD_API(api_interface, "license_info",
                   "Show machine ID and license status (mod_license)",
                   license_info_api, "");

    /* ------------------------------------------------------------------
     * STEP 2: Load configuration (optional license_file override).
     * ------------------------------------------------------------------ */
    if ((xml = switch_xml_open_cfg("license.conf", &cfg, NULL))) {
        if ((settings = switch_xml_child(cfg, "settings"))) {
            for (param = switch_xml_child(settings, "param"); param;
                 param = param->next) {
                const char *name = switch_xml_attr_soft(param, "name");
                const char *val  = switch_xml_attr_soft(param, "value");
                if (!strcasecmp(name, "license_file")) {
                    strncpy(globals.license_file, val,
                            sizeof(globals.license_file) - 1);
                }
            }
        }
        switch_xml_free(xml);
    }

    /* ------------------------------------------------------------------
     * STEP 3: Initialise COM and build the machine fingerprint.
     * Failure here is an environmental error (WMI broken, CNG missing).
     * ------------------------------------------------------------------ */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com_init = SUCCEEDED(hr);

    if (!build_machine_id(globals.machine_id)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                          "mod_license: failed to build machine fingerprint "
                          "(WMI or CNG unavailable)\n");
        if (com_init) CoUninitialize();
        /* API is already registered; license_info will report no machine ID */
        return SWITCH_STATUS_TERM;
    }

    if (com_init) CoUninitialize();

    /* Log the machine ID at NOTICE level every startup so it is always
     * visible in the log, regardless of license state. */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_license: machine ID = %s\n", globals.machine_id);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_license: to retrieve via CLI: fs_cli -x \"license_info\"\n");

    /* ------------------------------------------------------------------
     * STEP 4: Attempt to read and validate the license key file.
     * Missing or invalid key → licensed = 0, but module stays loaded
     * so the API remains queryable.
     * ------------------------------------------------------------------ */
    FILE *f = fopen(globals.license_file, "r");
    if (!f) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "mod_license: no license key found at: %s\n",
                          globals.license_file);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "mod_license: send machine ID to vendor to obtain a key:\n"
                          "             %s\n", globals.machine_id);
        switch_core_set_variable("license_status", "UNLICENSED");

        /* --- Hard-shutdown option (uncomment to re-enable): ---
        return SWITCH_STATUS_TERM;
        --------------------------------------------------------- */
        return SWITCH_STATUS_SUCCESS;   /* stay loaded; API is queryable */
    }

    char key_buf[MAX_KEY_FILE_BYTES] = {0};
    size_t n = fread(key_buf, 1, sizeof(key_buf) - 1, f);
    fclose(f);

    if (n == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "mod_license: license file is empty: %s\n",
                          globals.license_file);
        switch_core_set_variable("license_status", "UNLICENSED");
        return SWITCH_STATUS_SUCCESS;
    }

    globals.licensed = validate_license_key(key_buf, globals.machine_id);

    if (!globals.licensed) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
                          "mod_license: LICENSE VALIDATION FAILED\n"
                          "             machine ID : %s\n"
                          "             key file   : %s\n",
                          globals.machine_id, globals.license_file);
        switch_core_set_variable("license_status", "INVALID");

        /* --- Hard-shutdown option (uncomment to re-enable): ---
        return SWITCH_STATUS_TERM;
        --------------------------------------------------------- */
        return SWITCH_STATUS_SUCCESS;   /* stay loaded; API is queryable */
    }

    /* ------------------------------------------------------------------
     * STEP 5: Licensed — all clear.
     * ------------------------------------------------------------------ */
    switch_core_set_variable("license_status", "LICENSED");
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_license: this instance is LICENSED ✓\n");

    return SWITCH_STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Module shutdown
 * ----------------------------------------------------------------------- */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_license_shutdown)
{
    return SWITCH_STATUS_SUCCESS;
}
