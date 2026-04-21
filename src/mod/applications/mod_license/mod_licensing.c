/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 *
 * mod_licensing.c -- Licensing Module
 *
 */
#include <switch.h>
#include <switch_curl.h>
#include <switch_cJSON.h>

#ifdef WIN32
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#define strtok_r strtok_s
#else
#include <syslog.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <net/if_arp.h>
#endif

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_licensing_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_licensing_load);

SWITCH_MODULE_DEFINITION(mod_licensing, mod_licensing_load, mod_licensing_shutdown, NULL);

#define MAX_KEY_SOURCES 32
#define MAX_KEY_FILES 256
#define ONLINE_KEY_LENGTH 37

typedef enum {
	KEY_SOURCE_RELATIVE = 0,
	KEY_SOURCE_ABSOLUTE = 1,
	KEY_SOURCE_URL = 2
} key_source_type_t;

typedef enum {
	KEY_TYPE_ONLINE = 0,
	KEY_TYPE_OFFLINE = 1
} key_type_t;

typedef struct {
	key_source_type_t type;
	char *value;
} key_source_t;

typedef struct {
	char *file_path;
	char *key_content;
	key_type_t type;
	switch_bool_t validated;
	char *validation_msg;
	char *domain;
	char *fingerprint;
	char *modules;
	char *expires;
	int max_machines;
	char *machine_id; /* Keygen.sh machine ID for this activation */
	char *license_id; /* Keygen.sh license ID */
} license_key_t;

#define MAX_MAC_ADDRESSES 16
#define MAX_LICENSED_MODULES 128

static struct {
	key_source_t sources[MAX_KEY_SOURCES];
	int source_count;
	char *key_files[MAX_KEY_FILES];
	int key_file_count;
	license_key_t keys[MAX_KEY_FILES];
	int validated_count;
	char fingerprint[65]; /* SHA-256 hex string */
	char *keygen_account_id;
	char *keygen_public_key;
	char mac_addresses[MAX_MAC_ADDRESSES][18]; /* "XX:XX:XX:XX:XX:XX" */
	int mac_count;
	char domain[256];
	char *licensed_modules[MAX_LICENSED_MODULES];
	int licensed_module_count;
} globals;

/* Forward declarations */
static void licensing_log(switch_log_level_t level, const char *fmt, ...);
static void generate_html_report(void);
static void open_status_report(void);

static void add_licensed_module(const char *module_name)
{
	int i;
	if (zstr(module_name)) return;

	/* Check for duplicate */
	for (i = 0; i < globals.licensed_module_count; i++) {
		if (!strcasecmp(globals.licensed_modules[i], module_name)) return;
	}

	if (globals.licensed_module_count >= MAX_LICENSED_MODULES) {
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Maximum licensed modules limit reached\n");
		return;
	}

	globals.licensed_modules[globals.licensed_module_count] = strdup(module_name);
	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Licensed module added: %s\n", module_name);
	globals.licensed_module_count++;
}

static void parse_modules_list(const char *modules_str)
{
	char *work = strdup(modules_str);
	char *token, *saveptr = NULL;

	if (!work) return;

	/* Support multiple delimiters: comma, slash, semicolon */
	for (token = strtok_r(work, ",/;", &saveptr); token; token = strtok_r(NULL, ",/;", &saveptr)) {
		/* Trim whitespace */
		while (*token == ' ' || *token == '\t') token++;
		{
			size_t len = strlen(token);
			while (len > 0 && (token[len - 1] == ' ' || token[len - 1] == '\t')) token[--len] = '\0';
		}
		if (*token) add_licensed_module(token);
	}

	free(work);
}

/*
 * Write a message to the OS-native system log:
 *   Windows: Event Log (Application)
 *   Linux:   syslog
 * Also always writes to the FreeSWITCH log at the specified level.
 */
static void licensing_log(switch_log_level_t level, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* Always log to FreeSWITCH */
	switch_log_printf(SWITCH_CHANNEL_LOG, level, "%s", buf);

#ifdef WIN32
	{
		HANDLE hEventLog = RegisterEventSourceA(NULL, "SIPServer-Licensing");
		if (hEventLog) {
			WORD wType;
			const char *messages[1];
			messages[0] = buf;

			switch (level) {
			case SWITCH_LOG_CRIT:
			case SWITCH_LOG_ALERT:
			case SWITCH_LOG_ERROR:
				wType = EVENTLOG_ERROR_TYPE;
				break;
			case SWITCH_LOG_WARNING:
				wType = EVENTLOG_WARNING_TYPE;
				break;
			default:
				wType = EVENTLOG_INFORMATION_TYPE;
				break;
			}

			ReportEventA(hEventLog, wType, 0, 0, NULL, 1, 0, messages, NULL);
			DeregisterEventSource(hEventLog);
		}
	}
#else
	{
		int priority;

		switch (level) {
		case SWITCH_LOG_CRIT:
			priority = LOG_CRIT;
			break;
		case SWITCH_LOG_ALERT:
			priority = LOG_ALERT;
			break;
		case SWITCH_LOG_ERROR:
			priority = LOG_ERR;
			break;
		case SWITCH_LOG_WARNING:
			priority = LOG_WARNING;
			break;
		case SWITCH_LOG_NOTICE:
			priority = LOG_NOTICE;
			break;
		case SWITCH_LOG_INFO:
			priority = LOG_INFO;
			break;
		default:
			priority = LOG_DEBUG;
			break;
		}

		openlog("freeswitch-licensing", LOG_PID | LOG_NDELAY, LOG_USER);
		syslog(priority, "%s", buf);
		closelog();
	}
#endif
}

/*
 * Open the status HTML report in the default browser.
 *   Windows: ShellExecute
 *   Linux:   xdg-open
 */
static void open_status_report(void)
{
	char path[1024];
	const char *base = SWITCH_GLOBAL_dirs.base_dir;

	switch_snprintf(path, sizeof(path), "%s%sProduct Keys%sstatus.html", base, SWITCH_PATH_SEPARATOR, SWITCH_PATH_SEPARATOR);

	licensing_log(SWITCH_LOG_NOTICE, "mod_licensing: Opening license status report: %s\n", path);

#ifdef WIN32
	ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
	{
		char cmd[1200];
		switch_snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", path);
		if (system(cmd) == -1) {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Failed to launch xdg-open for status report\n");
		}
	}
#endif
}

/* ============================================================
 * Machine Fingerprint Generation
 * Combines: Machine GUID/machine-id + MAC address + hostname
 * Produces: SHA-256 hex string for use with keygen.sh
 * ============================================================ */

static void sha256_hex(const char *input, char *output, size_t output_size)
{
	unsigned char hash[SHA256_DIGEST_LENGTH];
	int i;

	SHA256((const unsigned char *)input, strlen(input), hash);

	for (i = 0; i < SHA256_DIGEST_LENGTH && (size_t)(i * 2 + 2) < output_size; i++) {
		sprintf(output + (i * 2), "%02x", hash[i]);
	}
	output[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static void get_machine_guid(char *buf, size_t buf_size)
{
	memset(buf, 0, buf_size);

#ifdef WIN32
	{
		HKEY hKey;
		DWORD dwSize = (DWORD)buf_size;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
			RegQueryValueExA(hKey, "MachineGuid", NULL, NULL, (LPBYTE)buf, &dwSize);
			RegCloseKey(hKey);
		}
	}
#else
	{
		FILE *f = fopen("/etc/machine-id", "r");
		if (f) {
			if (fgets(buf, (int)buf_size, f)) {
				size_t len = strlen(buf);
				while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
			}
			fclose(f);
		}
	}
#endif
}

static void get_primary_mac(char *buf, size_t buf_size)
{
	memset(buf, 0, buf_size);

#ifdef WIN32
	{
		IP_ADAPTER_INFO adapters[16];
		DWORD dwSize = sizeof(adapters);
		if (GetAdaptersInfo(adapters, &dwSize) == ERROR_SUCCESS) {
			IP_ADAPTER_INFO *adapter = adapters;
			/* Find first non-zero MAC */
			while (adapter) {
				if (adapter->AddressLength == 6 &&
					(adapter->Address[0] || adapter->Address[1] || adapter->Address[2] ||
					 adapter->Address[3] || adapter->Address[4] || adapter->Address[5])) {
					switch_snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
						adapter->Address[0], adapter->Address[1], adapter->Address[2],
						adapter->Address[3], adapter->Address[4], adapter->Address[5]);
					break;
				}
				adapter = adapter->Next;
			}
		}
	}
#else
	{
		struct ifaddrs *ifaddr, *ifa;
		if (getifaddrs(&ifaddr) == 0) {
			for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
				if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_PACKET) {
					int fd = socket(AF_INET, SOCK_DGRAM, 0);
					if (fd >= 0) {
						struct ifreq ifr;
						memset(&ifr, 0, sizeof(ifr));
						strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
						if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
							unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
							if (mac[0] || mac[1] || mac[2] || mac[3] || mac[4] || mac[5]) {
								switch_snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
									mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
								close(fd);
								break;
							}
						}
						close(fd);
					}
				}
			}
			freeifaddrs(ifaddr);
		}
	}
#endif
}

static void generate_machine_fingerprint(char *output, size_t output_size)
{
	char machine_guid[256] = "";
	char mac[18] = "";
	char hostname[256] = "";
	char combined[1024];

	get_machine_guid(machine_guid, sizeof(machine_guid));
	get_primary_mac(mac, sizeof(mac));
	gethostname(hostname, sizeof(hostname));

	switch_snprintf(combined, sizeof(combined), "%s|%s|%s", machine_guid, mac, hostname);
	sha256_hex(combined, output, output_size);

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Machine fingerprint generated: %s\n", output);
	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Fingerprint components - GUID:'%s' MAC:'%s' Host:'%s'\n",
		machine_guid, mac, hostname);
}

/* ============================================================
 * Collect all MAC addresses for multi-fingerprint validation
 * ============================================================ */

static void collect_all_mac_addresses(void)
{
	globals.mac_count = 0;

#ifdef WIN32
	{
		IP_ADAPTER_INFO adapters[16];
		DWORD dwSize = sizeof(adapters);
		if (GetAdaptersInfo(adapters, &dwSize) == ERROR_SUCCESS) {
			IP_ADAPTER_INFO *adapter = adapters;
			while (adapter && globals.mac_count < MAX_MAC_ADDRESSES) {
				if (adapter->AddressLength == 6 &&
					(adapter->Address[0] || adapter->Address[1] || adapter->Address[2] ||
					 adapter->Address[3] || adapter->Address[4] || adapter->Address[5])) {
					switch_snprintf(globals.mac_addresses[globals.mac_count], 18,
						"%02X:%02X:%02X:%02X:%02X:%02X",
						adapter->Address[0], adapter->Address[1], adapter->Address[2],
						adapter->Address[3], adapter->Address[4], adapter->Address[5]);
					licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Found MAC[%d]: %s\n",
						globals.mac_count, globals.mac_addresses[globals.mac_count]);
					globals.mac_count++;
				}
				adapter = adapter->Next;
			}
		}
	}
#else
	{
		struct ifaddrs *ifaddr, *ifa;
		if (getifaddrs(&ifaddr) == 0) {
			for (ifa = ifaddr; ifa && globals.mac_count < MAX_MAC_ADDRESSES; ifa = ifa->ifa_next) {
				if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_PACKET) {
					int fd = socket(AF_INET, SOCK_DGRAM, 0);
					if (fd >= 0) {
						struct ifreq ifr;
						memset(&ifr, 0, sizeof(ifr));
						strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
						if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
							unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
							if (mac[0] || mac[1] || mac[2] || mac[3] || mac[4] || mac[5]) {
								switch_snprintf(globals.mac_addresses[globals.mac_count], 18,
									"%02X:%02X:%02X:%02X:%02X:%02X",
									mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
								licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Found MAC[%d]: %s\n",
									globals.mac_count, globals.mac_addresses[globals.mac_count]);
								globals.mac_count++;
							}
						}
						close(fd);
					}
				}
			}
			freeifaddrs(ifaddr);
		}
	}
#endif

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Collected %d MAC address(es)\n", globals.mac_count);
}

/* ============================================================
 * Domain detection and matching
 * ============================================================ */

static void detect_machine_domain(void)
{
	memset(globals.domain, 0, sizeof(globals.domain));

#ifdef WIN32
	{
		char computer_name[256] = "";
		DWORD size = sizeof(computer_name);
		if (GetComputerNameExA(ComputerNameDnsDomain, computer_name, &size) && size > 0) {
			strncpy(globals.domain, computer_name, sizeof(globals.domain) - 1);
		} else {
			/* Fallback: try full DNS hostname */
			size = sizeof(computer_name);
			if (GetComputerNameExA(ComputerNameDnsFullyQualified, computer_name, &size)) {
				char *dot = strchr(computer_name, '.');
				if (dot) {
					strncpy(globals.domain, dot + 1, sizeof(globals.domain) - 1);
				}
			}
		}
	}
#else
	{
		char hostname[256] = "";
		if (gethostname(hostname, sizeof(hostname)) == 0) {
			char *dot = strchr(hostname, '.');
			if (dot) {
				strncpy(globals.domain, dot + 1, sizeof(globals.domain) - 1);
			} else {
				/* Try getdomainname */
				char domainname[256] = "";
				if (getdomainname(domainname, sizeof(domainname)) == 0 && strlen(domainname) > 0 && strcmp(domainname, "(none)") != 0) {
					strncpy(globals.domain, domainname, sizeof(globals.domain) - 1);
				}
			}
		}
	}
#endif

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Detected machine domain: '%s'\n",
		globals.domain[0] ? globals.domain : "(none)");
}

/*
 * Domain matching: the key's domain should match the end of the machine's FQDN or domain.
 * e.g., key domain "microsoft.com" matches machine domain "bob.microsoft.com" or "microsoft.com"
 */
static switch_bool_t domain_matches(const char *key_domain)
{
	size_t key_len, machine_len;
	const char *suffix;

	if (zstr(key_domain) || !globals.domain[0]) return SWITCH_FALSE;

	key_len = strlen(key_domain);
	machine_len = strlen(globals.domain);

	if (key_len > machine_len) return SWITCH_FALSE;

	/* Check if machine domain ends with key domain */
	suffix = globals.domain + (machine_len - key_len);

	if (!strcasecmp(suffix, key_domain)) {
		/* Make sure it's at a domain boundary */
		if (suffix == globals.domain || *(suffix - 1) == '.') {
			return SWITCH_TRUE;
		}
	}

	return SWITCH_FALSE;
}

/*
 * Fingerprint matching: generate a fingerprint for each MAC and compare.
 */
static switch_bool_t fingerprint_matches(const char *key_fingerprint)
{
	int i;
	char hostname[256] = "";
	char machine_guid[256] = "";
	char combined[1024];
	char test_fp[65];

	if (zstr(key_fingerprint)) return SWITCH_FALSE;

	/* First check against primary fingerprint */
	if (!strcasecmp(globals.fingerprint, key_fingerprint)) return SWITCH_TRUE;

	/* Check against each MAC-based fingerprint */
	get_machine_guid(machine_guid, sizeof(machine_guid));
	gethostname(hostname, sizeof(hostname));

	for (i = 0; i < globals.mac_count; i++) {
		switch_snprintf(combined, sizeof(combined), "%s|%s|%s", machine_guid, globals.mac_addresses[i], hostname);
		sha256_hex(combined, test_fp, sizeof(test_fp));

		if (!strcasecmp(test_fp, key_fingerprint)) {
			licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Fingerprint matched via MAC[%d]: %s\n",
				i, globals.mac_addresses[i]);
			return SWITCH_TRUE;
		}
	}

	return SWITCH_FALSE;
}

/* ============================================================
 * Online Key Validation (Keygen.sh)
 * Best Practices:
 * 1. Validate the license key with fingerprint scope
 * 2. Parse the JSON response to check maxMachines limit
 * 3. If valid and maxMachines allows, activate this machine
 * 4. Store machine ID for future check-ins and deactivation
 * ============================================================ */

static size_t http_response_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t realsize = size * nmemb;
	switch_stream_handle_t *stream = (switch_stream_handle_t *)userdata;

	stream->write_function(stream, "%.*s", (int)realsize, (char *)ptr);
	return realsize;
}

/* ============================================================
 * Machine Validation with Keygen
 * Verifies an existing machine ID is still valid
 * ============================================================ */
static switch_status_t validate_machine_with_keygen(const char *machine_id, const char *license_key)
{
	switch_CURL *curl_handle = NULL;
	switch_curl_slist_t *headers = NULL;
	char url[512];
	char auth_header[256];
	long http_code = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_stream_handle_t stream = { 0 };

	if (zstr(machine_id) || zstr(license_key) || zstr(globals.keygen_account_id)) {
		return SWITCH_STATUS_FALSE;
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Validating existing machine %s with Keygen\n", machine_id);

	/* GET the machine resource to check if it's still valid */
	switch_snprintf(url, sizeof(url),
		"https://api.keygen.sh/v1/accounts/%s/machines/%s",
		globals.keygen_account_id, machine_id);

	switch_snprintf(auth_header, sizeof(auth_header), "Authorization: License %s", license_key);

	SWITCH_STANDARD_STREAM(stream);

	curl_handle = switch_curl_easy_init();
	if (!curl_handle) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to initialize CURL for machine validation\n");
		return SWITCH_STATUS_FALSE;
	}

	headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");
	headers = switch_curl_slist_append(headers, auth_header);

	switch_curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, http_response_callback);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&stream);
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 30L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

	{
		CURLcode curl_code = switch_curl_easy_perform(curl_handle);
		if (curl_code != CURLE_OK) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: CURL error during machine validation: %s\n", 
				switch_curl_easy_strerror(curl_code));
			goto cleanup;
		}
	}

	switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Machine validation response (HTTP %ld): %s\n", 
		http_code, stream.data ? (char *)stream.data : "(null)");

	if (http_code == 200) {
		licensing_log(SWITCH_LOG_INFO, "mod_licensing: Machine %s is still valid\n", machine_id);
		status = SWITCH_STATUS_SUCCESS;
	} else if (http_code == 404) {
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Machine %s not found (may have been deleted)\n", machine_id);
	} else {
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Machine validation failed (HTTP %ld)\n", http_code);
	}

cleanup:
	switch_curl_easy_cleanup(curl_handle);
	switch_curl_slist_free_all(headers);
	switch_safe_free(stream.data);

	return status;
}

/* ============================================================
 * Find Existing Machine by Fingerprint
 * When activation returns 422 (already exists), fetch the machine ID
 * ============================================================ */
static char *find_machine_by_fingerprint(const char *license_key, const char *fingerprint)
{
	switch_CURL *curl_handle = NULL;
	switch_curl_slist_t *headers = NULL;
	char url[1024];
	char auth_header[256];
	long http_code = 0;
	switch_stream_handle_t stream = { 0 };
	cJSON *json_response = NULL;
	cJSON *data_array = NULL;
	char *machine_id = NULL;

	if (zstr(license_key) || zstr(fingerprint) || zstr(globals.keygen_account_id)) {
		return NULL;
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Searching for existing machine with fingerprint...\n");

	/* GET machines filtered by fingerprint */
	switch_snprintf(url, sizeof(url),
		"https://api.keygen.sh/v1/accounts/%s/machines?fingerprint=%s",
		globals.keygen_account_id, fingerprint);

	switch_snprintf(auth_header, sizeof(auth_header), "Authorization: License %s", license_key);

	SWITCH_STANDARD_STREAM(stream);

	curl_handle = switch_curl_easy_init();
	if (!curl_handle) {
		return NULL;
	}

	headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");
	headers = switch_curl_slist_append(headers, auth_header);

	switch_curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, http_response_callback);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&stream);
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 30L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

	if (switch_curl_easy_perform(curl_handle) == CURLE_OK) {
		switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

		if (http_code == 200 && stream.data) {
			json_response = cJSON_Parse((char *)stream.data);
			if (json_response) {
				data_array = cJSON_GetObjectItem(json_response, "data");
				if (data_array && cJSON_IsArray(data_array)) {
					int count = cJSON_GetArraySize(data_array);
					if (count > 0) {
						cJSON *first_machine = cJSON_GetArrayItem(data_array, 0);
						if (first_machine) {
							cJSON *id = cJSON_GetObjectItem(first_machine, "id");
							if (id && cJSON_IsString(id) && id->valuestring) {
								machine_id = strdup(id->valuestring);
								licensing_log(SWITCH_LOG_INFO, "mod_licensing: Found existing machine ID: %s\n", machine_id);
							}
						}
					}
				}
				cJSON_Delete(json_response);
			}
		}
	}

	switch_curl_easy_cleanup(curl_handle);
	switch_curl_slist_free_all(headers);
	switch_safe_free(stream.data);

	return machine_id;
}

/* ============================================================
 * Machine ID Persistence Helpers
 * Save/load machine IDs to/from .machine-id files
 * ============================================================ */
static void get_machine_id_file_path(const char *license_file_path, char *machine_id_path, size_t path_size)
{
	/* Create .machine-id file path from license file path */
	/* e.g., "license.key" -> "license.machine-id" */
	const char *dot = strrchr(license_file_path, '.');
	if (dot) {
		size_t base_len = dot - license_file_path;
		switch_snprintf(machine_id_path, path_size, "%.*s.machine-id", (int)base_len, license_file_path);
	} else {
		switch_snprintf(machine_id_path, path_size, "%s.machine-id", license_file_path);
	}
}

static char *load_machine_id_from_file(const char *machine_id_file_path)
{
	FILE *fp = NULL;
	char *machine_id = NULL;
	char buffer[256];

	fp = fopen(machine_id_file_path, "r");
	if (!fp) {
		return NULL; /* File doesn't exist or can't be read */
	}

	if (fgets(buffer, sizeof(buffer), fp)) {
		/* Trim whitespace and newline */
		char *p = buffer;
		while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;

		if (*p) {
			size_t len = strlen(p);
			while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t' || p[len-1] == '\r' || p[len-1] == '\n')) {
				p[--len] = '\0';
			}
			if (len > 0) {
				machine_id = strdup(p);
				licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Loaded machine ID from %s: %s\n", 
					machine_id_file_path, machine_id);
			}
		}
	}

	fclose(fp);
	return machine_id;
}

static switch_status_t save_machine_id_to_file(const char *machine_id_file_path, const char *machine_id)
{
	FILE *fp = NULL;

	if (zstr(machine_id)) {
		return SWITCH_STATUS_FALSE;
	}

	fp = fopen(machine_id_file_path, "w");
	if (!fp) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to open %s for writing\n", machine_id_file_path);
		return SWITCH_STATUS_FALSE;
	}

	fprintf(fp, "%s\n", machine_id);
	fclose(fp);

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Saved machine ID to %s\n", machine_id_file_path);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t activate_machine_on_license(const char *license_id, const char *license_key, const char *fingerprint, const char *license_token, char **machine_id_out)
{
	switch_CURL *curl_handle = NULL;
	switch_curl_slist_t *headers = NULL;
	char url[512];
	char post_data[1024];
	char auth_header[256];
	long http_code = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_stream_handle_t stream = { 0 };
	cJSON *json_response = NULL;
	cJSON *data = NULL;
	cJSON *id = NULL;

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Activating machine on license %s\n", license_id);

	/* Create machine using the machines endpoint */
	switch_snprintf(url, sizeof(url),
		"https://api.keygen.sh/v1/accounts/%s/machines",
		globals.keygen_account_id);

	switch_snprintf(post_data, sizeof(post_data),
		"{\"data\":{\"type\":\"machines\",\"attributes\":{\"fingerprint\":\"%s\"},\"relationships\":{\"license\":{\"data\":{\"type\":\"licenses\",\"id\":\"%s\"}}}}}",
		fingerprint, license_id);

	/* Use Bearer token if available, otherwise License key authentication */
	if (license_token && strlen(license_token) > 0) {
		switch_snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", license_token);
		licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Using Bearer token authentication\n");
	} else {
		switch_snprintf(auth_header, sizeof(auth_header), "Authorization: License %s", license_key);
		licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Using License key authentication\n");
	}

	SWITCH_STANDARD_STREAM(stream);

	curl_handle = switch_curl_easy_init();
	if (!curl_handle) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to initialize CURL for machine activation\n");
		return SWITCH_STATUS_FALSE;
	}

	headers = switch_curl_slist_append(headers, "Content-Type: application/vnd.api+json");
	headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");
	headers = switch_curl_slist_append(headers, auth_header);

	switch_curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	switch_curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);
	switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(post_data));
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, http_response_callback);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&stream);
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 30L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 1L);

	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Machine activation URL: %s\n", url);
	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Machine activation POST: %s\n", post_data);

	{
		CURLcode curl_code = switch_curl_easy_perform(curl_handle);
		if (curl_code != CURLE_OK) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: CURL error during machine activation: %s\n", 
				switch_curl_easy_strerror(curl_code));
			goto cleanup;
		}
	}

	switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Machine activation response (HTTP %ld): %s\n", 
		http_code, stream.data ? (char *)stream.data : "(null)");

	if (http_code == 201 || http_code == 200) {
		/* Parse JSON to get machine ID */
		if (stream.data) {
			json_response = cJSON_Parse((char *)stream.data);
			if (json_response) {
				data = cJSON_GetObjectItem(json_response, "data");
				if (data) {
					id = cJSON_GetObjectItem(data, "id");
					if (id && cJSON_IsString(id) && id->valuestring) {
						*machine_id_out = strdup(id->valuestring);
						licensing_log(SWITCH_LOG_INFO, "mod_licensing: Machine activated successfully. Machine ID: %s\n", 
							*machine_id_out);
						status = SWITCH_STATUS_SUCCESS;
					}
				}
				cJSON_Delete(json_response);
			}
		}
	} else if (http_code == 422) {
		/* Machine already exists - try to find it by fingerprint */
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Machine activation failed (HTTP 422) - machine may already exist\n");
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Error details: %s\n", stream.data ? (char *)stream.data : "(no response body)");

		/* Try to find the existing machine */
		char *existing_machine_id = find_machine_by_fingerprint(license_key, fingerprint);
		if (existing_machine_id) {
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: Retrieved existing machine ID, treating as successful activation\n");
			*machine_id_out = existing_machine_id;
			status = SWITCH_STATUS_SUCCESS;
		} else {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Could not retrieve existing machine ID\n");
		}
	} else if (http_code == 403) {
		/* Authentication method not allowed by policy */
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Machine activation failed (HTTP 403) - License key authentication is not allowed by your Keygen account policy.\n");
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Please enable license authentication in your Keygen account settings, or configure an account/product token.\n");
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Error details: %s\n", stream.data ? (char *)stream.data : "(no response body)");
	} else if (http_code == 401) {
		/* Authentication failed */
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Machine activation failed (HTTP 401) - authentication failed. Check license key.\n");
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Error details: %s\n", stream.data ? (char *)stream.data : "(no response body)");
	} else {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Machine activation failed (HTTP %ld)\n", http_code);
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Error details: %s\n", stream.data ? (char *)stream.data : "(no response body)");
	}

cleanup:
	switch_curl_easy_cleanup(curl_handle);
	switch_curl_slist_free_all(headers);
	switch_safe_free(stream.data);

	return status;
}

static switch_status_t validate_key_online(const char *key_content, const char *file_path, license_key_t *key_info)
{
	switch_CURL *curl_handle = NULL;
	switch_curl_slist_t *headers = NULL;
	char url[512];
	char post_data[1024];
	long http_code = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_stream_handle_t stream = { 0 };
	cJSON *json_response = NULL;
	cJSON *data = NULL;
	cJSON *attributes = NULL;
	cJSON *max_machines = NULL;
	cJSON *id = NULL;
	cJSON *meta = NULL;
	cJSON *token_obj = NULL;
	int max_machines_value = -1;
	char *license_id = NULL;
	char *license_token = NULL;

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Key is %d chars, verifying online: %s\n", (int)strlen(key_content), file_path);

	if (zstr(globals.keygen_account_id)) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Cannot validate online - no keygen-account-id configured\n");
		return SWITCH_STATUS_FALSE;
	}

	/* Step 1: Validate the license key */
	switch_snprintf(url, sizeof(url),
		"https://api.keygen.sh/v1/accounts/%s/licenses/actions/validate-key",
		globals.keygen_account_id);

	switch_snprintf(post_data, sizeof(post_data),
		"{\"meta\":{\"key\":\"%s\",\"scope\":{\"fingerprint\":\"%s\"}}}",
		key_content, globals.fingerprint);

	SWITCH_STANDARD_STREAM(stream);

	curl_handle = switch_curl_easy_init();
	if (!curl_handle) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to initialize CURL\n");
		switch_safe_free(stream.data);
		return SWITCH_STATUS_FALSE;
	}

	headers = switch_curl_slist_append(headers, "Content-Type: application/vnd.api+json");
	headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");

	switch_curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
	switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, http_response_callback);
	switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&stream);
	switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 30L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Sending validation request to: %s\n", url);
	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: POST data: %s\n", post_data);

	{
		CURLcode curl_code = switch_curl_easy_perform(curl_handle);
		if (curl_code != CURLE_OK) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: CURL error: %s\n", switch_curl_easy_strerror(curl_code));
			goto cleanup;
		}
	}
	switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Server response (HTTP %ld): %s\n", http_code, stream.data ? (char *)stream.data : "(null)");

	if (http_code == 200 && stream.data) {
		/* Step 2: Parse JSON response to extract license details */
		json_response = cJSON_Parse((char *)stream.data);
		if (!json_response) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to parse JSON response\n");
			goto cleanup;
		}

		data = cJSON_GetObjectItem(json_response, "data");
		if (!data) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: No 'data' field in JSON response\n");
			goto cleanup;
		}

		/* Extract license ID */
		id = cJSON_GetObjectItem(data, "id");
		if (id && cJSON_IsString(id) && id->valuestring) {
			license_id = strdup(id->valuestring);
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: License ID: %s\n", license_id);
		}

		/* Extract license token from meta if present */
		meta = cJSON_GetObjectItem(json_response, "meta");
		if (meta) {
			token_obj = cJSON_GetObjectItem(meta, "token");
			if (token_obj && cJSON_IsString(token_obj) && token_obj->valuestring) {
				license_token = strdup(token_obj->valuestring);
				licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: License token extracted from validation response\n");
			}
		}

		/* Extract attributes */
		attributes = cJSON_GetObjectItem(data, "attributes");
		if (attributes) {
			/* Check maxMachines limit */
			max_machines = cJSON_GetObjectItem(attributes, "maxMachines");
			if (max_machines) {
				if (cJSON_IsNull(max_machines)) {
					max_machines_value = -1; /* Unlimited */
					licensing_log(SWITCH_LOG_INFO, "mod_licensing: maxMachines: unlimited\n");
				} else if (cJSON_IsNumber(max_machines)) {
					max_machines_value = (int)max_machines->valuedouble;
					licensing_log(SWITCH_LOG_INFO, "mod_licensing: maxMachines: %d\n", max_machines_value);
				}
			}

			/* Extract other useful attributes */
			cJSON *status_obj = cJSON_GetObjectItem(attributes, "status");
			if (status_obj && cJSON_IsString(status_obj) && status_obj->valuestring) {
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: License status: %s\n", status_obj->valuestring);

				if (strcasecmp(status_obj->valuestring, "ACTIVE") != 0) {
					licensing_log(SWITCH_LOG_WARNING, "mod_licensing: License is not ACTIVE (status: %s)\n", status_obj->valuestring);
					if (key_info) {
						char msg[256];
						switch_snprintf(msg, sizeof(msg), "INVALID - license status is %s", status_obj->valuestring);
						key_info->validation_msg = strdup(msg);
					}
					goto cleanup;
				}
			}

			cJSON *expiry = cJSON_GetObjectItem(attributes, "expiry");
			if (expiry && cJSON_IsString(expiry) && expiry->valuestring) {
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: License expiry: %s\n", expiry->valuestring);
			}

			/* Extract metadata (custom fields like modules list) */
			cJSON *metadata = cJSON_GetObjectItem(attributes, "metadata");
			if (metadata && cJSON_IsObject(metadata)) {
				licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Processing license metadata...\n");

				/* Look for 'modules' field in metadata */
				cJSON *modules_obj = cJSON_GetObjectItem(metadata, "modules");
				if (modules_obj) {
					if (cJSON_IsString(modules_obj) && modules_obj->valuestring) {
						/* Modules as comma-separated string */
						licensing_log(SWITCH_LOG_INFO, "mod_licensing: Licensed modules (from metadata): %s\n", modules_obj->valuestring);
						if (key_info) {
							key_info->modules = strdup(modules_obj->valuestring);
						}
						parse_modules_list(modules_obj->valuestring);
					} else if (cJSON_IsArray(modules_obj)) {
						/* Modules as array */
						int module_count = cJSON_GetArraySize(modules_obj);
						licensing_log(SWITCH_LOG_INFO, "mod_licensing: Licensed modules (from metadata array): %d modules\n", module_count);
						for (int i = 0; i < module_count; i++) {
							cJSON *module_item = cJSON_GetArrayItem(modules_obj, i);
							if (module_item && cJSON_IsString(module_item) && module_item->valuestring) {
								licensing_log(SWITCH_LOG_INFO, "mod_licensing:   - %s\n", module_item->valuestring);
								add_licensed_module(module_item->valuestring);

								/* Build comma-separated string for key_info */
								if (key_info) {
									if (!key_info->modules) {
										key_info->modules = strdup(module_item->valuestring);
									} else {
										char *new_modules = NULL;
										switch_snprintf(new_modules, strlen(key_info->modules) + strlen(module_item->valuestring) + 2, 
											"%s,%s", key_info->modules, module_item->valuestring);
										if (new_modules) {
											free(key_info->modules);
											key_info->modules = new_modules;
										}
									}
								}
							}
						}
					}
				}

				/* Log all other metadata fields for debugging */
				cJSON *meta_item = NULL;
				cJSON_ArrayForEach(meta_item, metadata) {
					const char *key = meta_item->string;
					if (key && strcasecmp(key, "modules") != 0) { /* Skip modules, already processed */
						if (cJSON_IsString(meta_item) && meta_item->valuestring) {
							licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Metadata[%s]: %s\n", key, meta_item->valuestring);
						} else if (cJSON_IsNumber(meta_item)) {
							licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Metadata[%s]: %g\n", key, meta_item->valuedouble);
						} else if (cJSON_IsBool(meta_item)) {
							licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Metadata[%s]: %s\n", key, 
								cJSON_IsTrue(meta_item) ? "true" : "false");
						}
					}
				}
			} else {
				licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: No metadata found in license response\n");
			}
		}

		/* Step 3: Check for existing machine, validate it, or activate a new one */
		if (max_machines_value != 0 && license_id) {
			char *machine_id = NULL;
			char machine_id_file_path[512];
			switch_bool_t machine_valid = SWITCH_FALSE;

			/* Check if we have a saved machine ID */
			get_machine_id_file_path(file_path, machine_id_file_path, sizeof(machine_id_file_path));
			machine_id = load_machine_id_from_file(machine_id_file_path);

			if (machine_id) {
				/* We have a saved machine ID - validate it */
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Found existing machine ID: %s\n", machine_id);
				if (validate_machine_with_keygen(machine_id, key_content) == SWITCH_STATUS_SUCCESS) {
					licensing_log(SWITCH_LOG_INFO, "mod_licensing: Existing machine is still valid\n");
					machine_valid = SWITCH_TRUE;
				} else {
					licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Existing machine validation failed, will try to activate new machine\n");
					switch_safe_free(machine_id);
					machine_id = NULL;
				}
			}

			/* If no valid machine, activate a new one */
			if (!machine_valid) {
				if (activate_machine_on_license(license_id, key_content, globals.fingerprint, license_token, &machine_id) == SWITCH_STATUS_SUCCESS) {
					/* Save the machine ID for future runs */
					save_machine_id_to_file(machine_id_file_path, machine_id);
					licensing_log(SWITCH_LOG_INFO, "mod_licensing: New machine activated successfully\n");
					machine_valid = SWITCH_TRUE;
				} else {
					licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Machine activation failed\n");
				}
			}

			if (machine_valid && machine_id) {
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Online validation PASSED for %s\n", file_path);
				status = SWITCH_STATUS_SUCCESS;

				if (key_info) {
					key_info->validated = SWITCH_TRUE;
					key_info->validation_msg = strdup("VALID - online validation passed, machine verified");
					key_info->max_machines = max_machines_value;
					key_info->machine_id = machine_id; /* Takes ownership */
					key_info->license_id = license_id; /* Takes ownership */
					license_id = NULL; /* Prevent double-free */
				} else {
					switch_safe_free(machine_id);
				}
			} else {
				licensing_log(SWITCH_LOG_WARNING, "mod_licensing: License validation passed but machine validation/activation failed\n");
				if (key_info) {
					key_info->validation_msg = strdup("VALID - but machine activation failed (may have reached maxMachines limit)");
					key_info->max_machines = max_machines_value;
				}
			}
		} else if (max_machines_value == 0) {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: License has maxMachines=0, no activations allowed\n");
			if (key_info) {
				key_info->validation_msg = strdup("INVALID - maxMachines limit is 0");
			}
		}
	} else {
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Online validation FAILED for %s (HTTP %ld)\n", file_path, http_code);
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Server response: %s\n", stream.data ? (char *)stream.data : "(null)");
		if (key_info) {
			char msg[256];
			switch_snprintf(msg, sizeof(msg), "INVALID - online validation failed (HTTP %ld)", http_code);
			key_info->validation_msg = strdup(msg);
		}
	}

cleanup:
	if (json_response) {
		cJSON_Delete(json_response);
	}
	switch_safe_free(license_id);
	switch_safe_free(license_token);
	switch_curl_easy_cleanup(curl_handle);
	switch_curl_slist_free_all(headers);
	switch_safe_free(stream.data);

	return status;
}

/* ============================================================
 * Keygen Signed License Validation (JWT-style)
 * Format: {base64_payload}.{base64_signature}
 * Decodes JSON payload and extracts license metadata
 * ============================================================ */

static switch_status_t validate_keygen_signed_license(const char *key_content, const char *file_path, license_key_t *key_info)
{
	char *payload_copy = NULL;
	char *payload_b64 = NULL;
	char *signature_b64 = NULL;
	char *decoded_payload = NULL;
	size_t decoded_len = 0;
	cJSON *json_payload = NULL;
	cJSON *license_obj = NULL;
	cJSON *policy_obj = NULL;
	cJSON *metadata_obj = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Parsing Keygen signed license: %s\n", file_path);

	/* Split payload and signature at the dot */
	payload_copy = strdup(key_content);
	if (!payload_copy) {
		return SWITCH_STATUS_FALSE;
	}

	char *dot = strchr(payload_copy, '.');
	if (!dot) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Invalid Keygen license format (missing signature separator)\n");
		free(payload_copy);
		return SWITCH_STATUS_FALSE;
	}

	*dot = '\0';
	payload_b64 = payload_copy;
	signature_b64 = dot + 1;

	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Payload: %s\n", payload_b64);
	licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Signature: %s\n", signature_b64);

	/* Decode the payload (base64) */
	{
		BIO *bio, *b64;
		size_t payload_len = strlen(payload_b64);
		decoded_payload = (char *)malloc(payload_len + 1);
		if (!decoded_payload) {
			free(payload_copy);
			return SWITCH_STATUS_FALSE;
		}

		b64 = BIO_new(BIO_f_base64());
		bio = BIO_new_mem_buf(payload_b64, (int)payload_len);
		bio = BIO_push(b64, bio);
		BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
		decoded_len = BIO_read(bio, decoded_payload, (int)payload_len);
		BIO_free_all(bio);

		if ((int)decoded_len <= 0) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to decode Keygen license payload\n");
			free(decoded_payload);
			free(payload_copy);
			return SWITCH_STATUS_FALSE;
		}
		decoded_payload[decoded_len] = '\0';
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Decoded payload (%d bytes): %s\n", (int)decoded_len, decoded_payload);

	/* Parse JSON payload */
	json_payload = cJSON_Parse(decoded_payload);
	if (!json_payload) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to parse Keygen license JSON\n");
		free(decoded_payload);
		free(payload_copy);
		return SWITCH_STATUS_FALSE;
	}

	/* Extract license metadata */
	license_obj = cJSON_GetObjectItem(json_payload, "license");
	if (license_obj) {
		cJSON *expiry = cJSON_GetObjectItem(license_obj, "expiry");
		if (expiry && cJSON_IsString(expiry) && expiry->valuestring) {
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: License expiry: %s\n", expiry->valuestring);
			if (key_info) {
				key_info->expires = strdup(expiry->valuestring);
			}
		} else if (expiry && cJSON_IsNull(expiry)) {
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: License expiry: never\n");
		}

		cJSON *created = cJSON_GetObjectItem(license_obj, "created");
		if (created && cJSON_IsString(created) && created->valuestring) {
			licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: License created: %s\n", created->valuestring);
		}

		cJSON *id = cJSON_GetObjectItem(license_obj, "id");
		if (id && cJSON_IsString(id) && id->valuestring) {
			licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: License ID: %s\n", id->valuestring);
		}
	}

	/* Extract policy metadata */
	policy_obj = cJSON_GetObjectItem(json_payload, "policy");
	if (policy_obj) {
		cJSON *policy_id = cJSON_GetObjectItem(policy_obj, "id");
		if (policy_id && cJSON_IsString(policy_id) && policy_id->valuestring) {
			licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Policy ID: %s\n", policy_id->valuestring);
		}
	}

	/* Extract metadata (custom fields like modules, domain, etc.) */
	metadata_obj = cJSON_GetObjectItem(json_payload, "metadata");
	if (metadata_obj && cJSON_IsObject(metadata_obj)) {
		licensing_log(SWITCH_LOG_INFO, "mod_licensing: Processing license metadata from Keygen signed license...\n");

		/* Extract domain */
		cJSON *domain = cJSON_GetObjectItem(metadata_obj, "domain");
		if (domain && cJSON_IsString(domain) && domain->valuestring) {
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: License domain: %s\n", domain->valuestring);
			if (key_info) {
				key_info->domain = strdup(domain->valuestring);
			}

			/* Validate domain */
			if (domain_matches(domain->valuestring)) {
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Domain validation PASSED (machine domain '%s' matches license domain '%s')\n",
					globals.domain, domain->valuestring);
				status = SWITCH_STATUS_SUCCESS;
			} else {
				licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Domain validation FAILED (machine domain '%s' does not match license domain '%s')\n",
					globals.domain, domain->valuestring);
				if (key_info) {
					key_info->validation_msg = strdup("INVALID - domain mismatch");
				}
				goto cleanup;
			}
		}

		/* Extract modules */
		cJSON *modules = cJSON_GetObjectItem(metadata_obj, "modules");
		if (modules) {
			if (cJSON_IsString(modules) && modules->valuestring) {
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Licensed modules: %s\n", modules->valuestring);
				if (key_info) {
					key_info->modules = strdup(modules->valuestring);
				}
				parse_modules_list(modules->valuestring);
			} else if (cJSON_IsArray(modules)) {
				int module_count = cJSON_GetArraySize(modules);
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Licensed modules: %d modules\n", module_count);
				for (int i = 0; i < module_count; i++) {
					cJSON *module_item = cJSON_GetArrayItem(modules, i);
					if (module_item && cJSON_IsString(module_item) && module_item->valuestring) {
						licensing_log(SWITCH_LOG_INFO, "mod_licensing:   - %s\n", module_item->valuestring);
						add_licensed_module(module_item->valuestring);
					}
				}
			}
		}

		/* Log all other metadata fields */
		cJSON *meta_item = NULL;
		cJSON_ArrayForEach(meta_item, metadata_obj) {
			const char *key = meta_item->string;
			if (key && strcasecmp(key, "modules") != 0 && strcasecmp(key, "domain") != 0) {
				if (cJSON_IsString(meta_item) && meta_item->valuestring) {
					licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Metadata[%s]: %s\n", key, meta_item->valuestring);
				}
			}
		}
	} else {
		/* No metadata - default to allowing all (no domain restriction) */
		licensing_log(SWITCH_LOG_INFO, "mod_licensing: No metadata found - allowing all modules, no domain restriction\n");
		status = SWITCH_STATUS_SUCCESS;
	}

	if (status == SWITCH_STATUS_SUCCESS && key_info) {
		key_info->validated = SWITCH_TRUE;
		if (!key_info->validation_msg) {
			key_info->validation_msg = strdup("VALID - Keygen signed license");
		}
	}

cleanup:
	if (json_payload) {
		cJSON_Delete(json_payload);
	}
	free(decoded_payload);
	free(payload_copy);

	return status;
}

/* ============================================================
 * Offline Key Validation
 * Only supports Keygen signed license format: {payload}.{signature}
 * Old key:value format is no longer supported.
 * ============================================================ */

static switch_status_t validate_key_offline(const char *key_content, const char *file_path, license_key_t *key_info)
{
	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Validating offline license: %s\n", file_path);

	/* Only support Keygen signed license format: {payload}.{signature} */
	if (strchr(key_content, '.') == NULL) {
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Invalid license format - expected Keygen signed license (payload.signature)\n");
		licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Old key:value format is no longer supported. Please use Keygen signed licenses.\n");
		if (key_info) {
			key_info->validation_msg = strdup("INVALID - not a Keygen signed license");
		}
		return SWITCH_STATUS_FALSE;
	}

	/* Parse as Keygen signed license */
	return validate_keygen_signed_license(key_content, file_path, key_info);
}

/* ============================================================
 * Key file processing and source scanning
 * ============================================================ */

static void process_source(key_source_t *source)
{
	char full_path[1024];
	const char *base = SWITCH_GLOBAL_dirs.base_dir;

	switch (source->type) {
	case KEY_SOURCE_RELATIVE:
		switch_snprintf(full_path, sizeof(full_path), "%s%s%s", base, SWITCH_PATH_SEPARATOR, source->value);
		break;
	case KEY_SOURCE_ABSOLUTE:
		strncpy(full_path, source->value, sizeof(full_path) - 1);
		full_path[sizeof(full_path) - 1] = '\0';
		break;
	case KEY_SOURCE_URL:
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: URL sources not yet implemented: %s\n", source->value);
		return;
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Scanning directory: %s\n", full_path);

	/* Scan directory for key files */
	{
		switch_dir_t *dir = NULL;
		const char *filename;
		switch_memory_pool_t *temp_pool = NULL;
		int file_count = 0;
		char file_buf[256];

		if (switch_core_new_memory_pool(&temp_pool) != SWITCH_STATUS_SUCCESS) {
			licensing_log(SWITCH_LOG_ERROR, "mod_licensing: Failed to create memory pool\n");
			return;
		}

		if (switch_dir_open(&dir, full_path, temp_pool) != SWITCH_STATUS_SUCCESS) {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Cannot open directory: %s\n", full_path);
			switch_core_destroy_memory_pool(&temp_pool);
			return;
		}

		licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Directory opened successfully, scanning for .key and .lic files...\n");

		while ((filename = switch_dir_next_file(dir, file_buf, sizeof(file_buf)))) {
			char file_path[1024];
			const char *ext;

			file_count++;
			licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Checking file: %s\n", filename);

			/* Only process .key or .lic files */
			ext = strrchr(filename, '.');
			if (!ext) {
				licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Skipping %s (no extension)\n", filename);
				continue;
			}
			if (strcasecmp(ext, ".key") != 0 && strcasecmp(ext, ".lic") != 0) {
				licensing_log(SWITCH_LOG_DEBUG, "mod_licensing: Skipping %s (extension: %s)\n", filename, ext);
				continue;
			}

			switch_snprintf(file_path, sizeof(file_path), "%s%s%s", full_path, SWITCH_PATH_SEPARATOR, filename);

			if (globals.key_file_count >= MAX_KEY_FILES) {
				licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Maximum key file limit reached\n");
				break;
			}

			globals.key_files[globals.key_file_count] = strdup(file_path);
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: Found key file: %s\n", file_path);
			globals.key_file_count++;
		}

		licensing_log(SWITCH_LOG_INFO, "mod_licensing: Scanned %d files in directory, found %d key files\n", 
			file_count, globals.key_file_count);

		switch_dir_close(dir);
		switch_core_destroy_memory_pool(&temp_pool);
	}
}

static char *read_file_content(const char *path)
{
	FILE *f;
	long size;
	char *content = NULL;

	f = fopen(path, "rb");
	if (!f) return NULL;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || size > 1024 * 1024) {
		fclose(f);
		return NULL;
	}

	content = (char *)malloc(size + 1);
	if (content) {
		size_t read_bytes = fread(content, 1, size, f);
		content[read_bytes] = '\0';

		/* Trim trailing whitespace/newlines */
		while (read_bytes > 0 && (content[read_bytes - 1] == '\n' || content[read_bytes - 1] == '\r' || content[read_bytes - 1] == ' ')) {
			content[--read_bytes] = '\0';
		}
	}

	fclose(f);
	return content;
}

static switch_status_t validate_keys(void)
{
	int i;
	int valid_count = 0;

	for (i = 0; i < globals.key_file_count; i++) {
		char *content = read_file_content(globals.key_files[i]);
		license_key_t *key_info = &globals.keys[i];

		if (!content) {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Cannot read key file: %s\n", globals.key_files[i]);
			key_info->file_path = strdup(globals.key_files[i]);
			key_info->validation_msg = strdup("INVALID - cannot read file");
			continue;
		}

		key_info->file_path = strdup(globals.key_files[i]);
		key_info->key_content = content;

		/* Determine if online or offline based on length */
		if ((int)strlen(content) <= ONLINE_KEY_LENGTH) {
			key_info->type = KEY_TYPE_ONLINE;
			if (validate_key_online(content, globals.key_files[i], key_info) == SWITCH_STATUS_SUCCESS) {
				key_info->validated = SWITCH_TRUE;
				valid_count++;
			}
		} else {
			key_info->type = KEY_TYPE_OFFLINE;
			if (validate_key_offline(content, globals.key_files[i], key_info) == SWITCH_STATUS_SUCCESS) {
				key_info->validated = SWITCH_TRUE;
				valid_count++;
			}
		}
	}

	globals.validated_count = valid_count;
	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Validation complete. %d of %d keys valid.\n", valid_count, globals.key_file_count);

	return (valid_count > 0) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/* ============================================================
 * HTML Status Report Generation (Template-Based)
 * ============================================================ */

static char *string_replace(const char *str, const char *find, const char *replace)
{
	const char *pos;
	char *result;
	size_t find_len = strlen(find);
	size_t replace_len = strlen(replace);
	size_t result_len;
	int count = 0;
	const char *p = str;

	/* Count occurrences */
	while ((p = strstr(p, find)) != NULL) {
		count++;
		p += find_len;
	}

	if (count == 0) {
		return strdup(str);
	}

	result_len = strlen(str) + count * (replace_len - find_len) + 1;
	result = (char *)malloc(result_len);
	if (!result) return NULL;

	result[0] = '\0';
	p = str;
	while ((pos = strstr(p, find)) != NULL) {
		strncat(result, p, pos - p);
		strcat(result, replace);
		p = pos + find_len;
	}
	strcat(result, p);

	return result;
}

static void generate_html_report(void)
{
	char template_path[1024];
	char output_path[1024];
	char dir_path[1024];
	char templates_dir[1024];
	const char *base = SWITCH_GLOBAL_dirs.base_dir;
	char *template_content = NULL;
	char *html_output = NULL;
	char buffer[32768];
	FILE *f;
	int i;
	switch_memory_pool_t *temp_pool = NULL;

	/* Create the Product Keys and Templates directories */
	switch_snprintf(dir_path, sizeof(dir_path), "%s%sProduct Keys", base, SWITCH_PATH_SEPARATOR);
	switch_snprintf(templates_dir, sizeof(templates_dir), "%s%sProduct Keys%sTemplates", base, SWITCH_PATH_SEPARATOR, SWITCH_PATH_SEPARATOR);

	if (switch_core_new_memory_pool(&temp_pool) == SWITCH_STATUS_SUCCESS) {
		switch_dir_make_recursive(templates_dir, SWITCH_FPROT_OS_DEFAULT, temp_pool);
		switch_core_destroy_memory_pool(&temp_pool);
	}

	/* Try to load template */
	switch_snprintf(template_path, sizeof(template_path), "%s%sstatus.html.template", templates_dir, SWITCH_PATH_SEPARATOR);
	template_content = read_file_content(template_path);

	if (!template_content) {
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Template not found at %s, generating default report\n", template_path);
		/* Fall back to inline generation - could keep old code here or create a default template */
		return;
	}

	html_output = strdup(template_content);
	free(template_content);

	/* Replace placeholders with actual data */

	/* Section 0: Configuration Sources */
	{
		char count_str[16];
		switch_snprintf(count_str, sizeof(count_str), "%d", globals.source_count);
		char *temp = string_replace(html_output, "{{SOURCE_COUNT}}", count_str);
		free(html_output);
		html_output = temp;
	}
	buffer[0] = '\0';
	for (i = 0; i < globals.source_count; i++) {
		char source_line[1024];
		const char *type_str = "unknown";
		switch (globals.sources[i].type) {
		case KEY_SOURCE_RELATIVE: type_str = "relative"; break;
		case KEY_SOURCE_ABSOLUTE: type_str = "absolute"; break;
		case KEY_SOURCE_URL: type_str = "url"; break;
		}
		switch_snprintf(source_line, sizeof(source_line), 
			"&nbsp;&nbsp;[%s] %s<br>\n", type_str, globals.sources[i].value);
		strcat(buffer, source_line);
	}
	{
		char *temp = string_replace(html_output, "{{KEY_SOURCES_LIST}}", buffer);
		free(html_output);
		html_output = temp;
	}

	/* Section 1: Keys Found Rows */
	buffer[0] = '\0';
	for (i = 0; i < globals.key_file_count; i++) {
		char row[512];
		switch_snprintf(row, sizeof(row), 
			"<tr><td>%d</td><td>%s</td><td>%s</td><td>%d</td></tr>\n",
			i + 1,
			globals.keys[i].file_path ? globals.keys[i].file_path : "unknown",
			globals.keys[i].type == KEY_TYPE_ONLINE ? "Online" : "Offline",
			globals.keys[i].key_content ? (int)strlen(globals.keys[i].key_content) : 0);
		strcat(buffer, row);
	}
	{
		char *temp = string_replace(html_output, "{{KEYS_FOUND_ROWS}}", buffer);
		free(html_output);
		html_output = temp;
	}

	/* No keys message */
	{
		const char *msg = (globals.key_file_count == 0) ? 
			"<p class=\"invalid\">No key files found in any configured source.</p>" : "";
		char *temp = string_replace(html_output, "{{NO_KEYS_MESSAGE}}", msg);
		free(html_output);
		html_output = temp;
	}

	/* Section 2: Validation Results */
	buffer[0] = '\0';
	for (i = 0; i < globals.key_file_count; i++) {
		char row[512];
		const char *css_class = globals.keys[i].validated ? "valid" : "invalid";
		switch_snprintf(row, sizeof(row),
			"<tr><td>%d</td><td>%s</td><td class=\"%s\">%s</td></tr>\n",
			i + 1,
			globals.keys[i].file_path ? globals.keys[i].file_path : "unknown",
			css_class,
			globals.keys[i].validation_msg ? globals.keys[i].validation_msg : "unknown");
		strcat(buffer, row);
	}
	{
		char *temp = string_replace(html_output, "{{VALIDATION_ROWS}}", buffer);
		free(html_output);
		html_output = temp;
	}

	/* Section 3: Licensed Modules */
	buffer[0] = '\0';
	if (globals.licensed_module_count > 0) {
		strcat(buffer, "<table><tr><th>#</th><th>Module</th></tr>\n");
		for (i = 0; i < globals.licensed_module_count; i++) {
			char row[256];
			switch_snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td></tr>\n", 
				i + 1, globals.licensed_modules[i]);
			strcat(buffer, row);
		}
		strcat(buffer, "</table>\n");
	} else {
		strcat(buffer, "<p class=\"invalid\">No modules licensed.</p>");
	}
	{
		char *temp = string_replace(html_output, "{{LICENSED_MODULES_CONTENT}}", buffer);
		free(html_output);
		html_output = temp;
	}

	/* Section 4: Time Limits */
	buffer[0] = '\0';
	{
		int found_expiry = 0;
		for (i = 0; i < globals.key_file_count; i++) {
			if (globals.keys[i].expires) {
				char row[512];
				switch_snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td></tr>\n",
					globals.keys[i].file_path ? globals.keys[i].file_path : "unknown",
					globals.keys[i].expires);
				strcat(buffer, row);
				found_expiry = 1;
			}
		}
		{
			char *temp = string_replace(html_output, "{{TIME_LIMITS_ROWS}}", buffer);
			free(html_output);
			html_output = temp;
		}
		{
			const char *msg = found_expiry ? "" : "<p>No time limits found in any key.</p>";
			char *temp = string_replace(html_output, "{{NO_TIME_LIMITS_MESSAGE}}", msg);
			free(html_output);
			html_output = temp;
		}
	}

	/* Section 5: Domain Licensing */
	buffer[0] = '\0';
	{
		int found_domain = 0;
		for (i = 0; i < globals.key_file_count; i++) {
			if (globals.keys[i].domain) {
				char row[512];
				switch_snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td></tr>\n",
					globals.keys[i].file_path ? globals.keys[i].file_path : "unknown",
					globals.keys[i].domain);
				strcat(buffer, row);
				found_domain = 1;
			}
		}
		{
			char *temp = string_replace(html_output, "{{DOMAIN_LICENSING_ROWS}}", buffer);
			free(html_output);
			html_output = temp;
		}
		{
			const char *msg = found_domain ? "" : "<p>No domain-based licensing found.</p>";
			char *temp = string_replace(html_output, "{{NO_DOMAIN_MESSAGE}}", msg);
			free(html_output);
			html_output = temp;
		}
	}

	/* Section 6: Machine Fingerprint */
	{
		char *temp = string_replace(html_output, "{{PRIMARY_FINGERPRINT}}", globals.fingerprint);
		free(html_output);
		html_output = temp;
	}
	{
		const char *domain = globals.domain[0] ? globals.domain : "(none detected)";
		char *temp = string_replace(html_output, "{{MACHINE_DOMAIN}}", domain);
		free(html_output);
		html_output = temp;
	}
	{
		char count_str[16];
		switch_snprintf(count_str, sizeof(count_str), "%d", globals.mac_count);
		char *temp = string_replace(html_output, "{{MAC_COUNT}}", count_str);
		free(html_output);
		html_output = temp;
	}
	buffer[0] = '\0';
	for (i = 0; i < globals.mac_count; i++) {
		char mac_line[128];
		switch_snprintf(mac_line, sizeof(mac_line), "&nbsp;&nbsp;MAC[%d]: %s<br>\n", i, globals.mac_addresses[i]);
		strcat(buffer, mac_line);
	}
	{
		char *temp = string_replace(html_output, "{{MAC_ADDRESSES_LIST}}", buffer);
		free(html_output);
		html_output = temp;
	}

	/* Section 7: Fingerprint Validation */
	buffer[0] = '\0';
	{
		int found_fp = 0;
		for (i = 0; i < globals.key_file_count; i++) {
			if (globals.keys[i].fingerprint) {
				char row[512];
				switch_bool_t matches = fingerprint_matches(globals.keys[i].fingerprint);
				switch_snprintf(row, sizeof(row), 
					"<tr><td>%s</td><td>%s</td><td class=\"%s\">%s</td></tr>\n",
					globals.keys[i].file_path ? globals.keys[i].file_path : "unknown",
					globals.keys[i].fingerprint,
					matches ? "valid" : "invalid",
					matches ? "MATCH" : "NO MATCH");
				strcat(buffer, row);
				found_fp = 1;
			}
		}
		{
			char *temp = string_replace(html_output, "{{FINGERPRINT_VALIDATION_ROWS}}", buffer);
			free(html_output);
			html_output = temp;
		}
		{
			const char *msg = found_fp ? "" : "<p>No fingerprint-based keys found.</p>";
			char *temp = string_replace(html_output, "{{NO_FINGERPRINT_KEYS_MESSAGE}}", msg);
			free(html_output);
			html_output = temp;
		}
	}

	/* Write output file */
	switch_snprintf(output_path, sizeof(output_path), "%s%sProduct Keys%sstatus.html", 
		base, SWITCH_PATH_SEPARATOR, SWITCH_PATH_SEPARATOR);

	f = fopen(output_path, "w");
	if (!f) {
		licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Cannot create status report at: %s\n", output_path);
		free(html_output);
		return;
	}

	fputs(html_output, f);
	fclose(f);
	free(html_output);

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Status report written to: %s\n", output_path);
}

/* ============================================================
 * Configuration
 * ============================================================ */

static switch_status_t do_config(void)
{
	switch_xml_t xml = NULL, cfg = NULL, settings = NULL, key_sources = NULL, param, source;
	int i;
	int total_keys;

	if (!(xml = switch_xml_open_cfg("licensing.conf", &cfg, NULL))) {
		licensing_log(SWITCH_LOG_CRIT, "mod_licensing: Failed to open licensing.conf\n");
		generate_html_report();
		return SWITCH_STATUS_FALSE;
	}

	/* Parse settings */
	settings = switch_xml_child(cfg, "settings");
	if (settings) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *name = switch_xml_attr_soft(param, "name");
			const char *value = switch_xml_attr_soft(param, "value");

			if (!strcasecmp(name, "keygen-account-id")) {
				globals.keygen_account_id = strdup(value);
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Keygen account ID configured\n");
			} else if (!strcasecmp(name, "keygen-public-key")) {
				globals.keygen_public_key = strdup(value);
				licensing_log(SWITCH_LOG_INFO, "mod_licensing: Keygen public key configured\n");
			}
		}
	}

	/* Parse key sources */
	key_sources = switch_xml_child(cfg, "key-sources");
	if (!key_sources) {
		licensing_log(SWITCH_LOG_CRIT, "mod_licensing: No <key-sources> section found in licensing.conf\n");
		switch_xml_free(xml);
		generate_html_report();
		return SWITCH_STATUS_FALSE;
	}

	for (source = switch_xml_child(key_sources, "source"); source; source = source->next) {
		const char *type_str = switch_xml_attr_soft(source, "type");
		const char *value = switch_xml_attr_soft(source, "value");

		if (zstr(type_str) || zstr(value)) {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Skipping source entry with missing type or value\n");
			continue;
		}

		if (globals.source_count >= MAX_KEY_SOURCES) {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Maximum source limit reached (%d)\n", MAX_KEY_SOURCES);
			break;
		}

		if (!strcasecmp(type_str, "relative")) {
			globals.sources[globals.source_count].type = KEY_SOURCE_RELATIVE;
		} else if (!strcasecmp(type_str, "absolute")) {
			globals.sources[globals.source_count].type = KEY_SOURCE_ABSOLUTE;
		} else if (!strcasecmp(type_str, "url")) {
			globals.sources[globals.source_count].type = KEY_SOURCE_URL;
		} else {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Unknown source type '%s', skipping\n", type_str);
			continue;
		}

		globals.sources[globals.source_count].value = strdup(value);
		licensing_log(SWITCH_LOG_INFO, "mod_licensing: Configured key source [%s] = '%s'\n", type_str, value);
		globals.source_count++;
	}

	switch_xml_free(xml);

	if (globals.source_count == 0) {
		licensing_log(SWITCH_LOG_CRIT, "mod_licensing: No valid key sources configured\n");
		generate_html_report();
		return SWITCH_STATUS_FALSE;
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Loaded %d key source(s), now scanning for keys...\n", globals.source_count);

	/* Process each source */
	for (i = 0; i < globals.source_count; i++) {
		process_source(&globals.sources[i]);
	}

	total_keys = globals.key_file_count;

	if (total_keys == 0) {
		licensing_log(SWITCH_LOG_CRIT, "mod_licensing: No key files found in any configured source. No valid licenses.\n");
		generate_html_report();
		return SWITCH_STATUS_FALSE;
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Total key files discovered: %d\n", total_keys);

	/* Validate each key - online (37 chars) or offline (longer) */
	if (validate_keys() != SWITCH_STATUS_SUCCESS) {
		licensing_log(SWITCH_LOG_CRIT, "mod_licensing: No valid licenses found after key validation.\n");
		generate_html_report();
		return SWITCH_STATUS_FALSE;
	}

	generate_html_report();
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(check_module_function)
{
	int i;
	const char *module_name = cmd;

	if (zstr(module_name)) {
		stream->write_function(stream, "-ERR Usage: check_module <module_name>\n");
		return SWITCH_STATUS_SUCCESS;
	}

	for (i = 0; i < globals.licensed_module_count; i++) {
		if (!strcasecmp(globals.licensed_modules[i], module_name)) {
			stream->write_function(stream, "+OK licensed\n");
			return SWITCH_STATUS_SUCCESS;
		}
	}

	stream->write_function(stream, "-ERR not licensed\n");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(license_function)
{
	int i;
	stream->write_function(stream, "Licensing Module Status\n");
	stream->write_function(stream, "=======================\n");
	stream->write_function(stream, "Machine Fingerprint: %s\n", globals.fingerprint);
	stream->write_function(stream, "Configured sources: %d\n", globals.source_count);
	stream->write_function(stream, "Key files found: %d\n\n", globals.key_file_count);

	for (i = 0; i < globals.source_count; i++) {
		const char *type_str = "unknown";
		switch (globals.sources[i].type) {
		case KEY_SOURCE_RELATIVE: type_str = "relative"; break;
		case KEY_SOURCE_ABSOLUTE: type_str = "absolute"; break;
		case KEY_SOURCE_URL: type_str = "url"; break;
		}
		stream->write_function(stream, "  Source %d: [%s] %s\n", i + 1, type_str, globals.sources[i].value);
	}

	stream->write_function(stream, "\nKey files:\n");
	for (i = 0; i < globals.key_file_count; i++) {
		stream->write_function(stream, "  %d: %s\n", i + 1, globals.key_files[i]);
	}

	stream->write_function(stream, "\nLicensed modules (%d):\n", globals.licensed_module_count);
	for (i = 0; i < globals.licensed_module_count; i++) {
		stream->write_function(stream, "  %s\n", globals.licensed_modules[i]);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_licensing_load)
{
	switch_api_interface_t *api_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	licensing_log(SWITCH_LOG_NOTICE, "mod_licensing: Module loading...\n");

	/* Generate machine fingerprint for keygen.sh */
	generate_machine_fingerprint(globals.fingerprint, sizeof(globals.fingerprint));

	/* Collect all MAC addresses for multi-fingerprint validation */
	collect_all_mac_addresses();

	/* Detect machine domain for domain-based validation */
	detect_machine_domain();

	if (do_config() != SWITCH_STATUS_SUCCESS) {
		licensing_log(SWITCH_LOG_CRIT, "mod_licensing: Configuration failed or no valid licenses found. Module will NOT load.\n");
		open_status_report();
		return SWITCH_STATUS_GENERR;
	}

	SWITCH_ADD_API(api_interface, "check_licensing", "Licensing status API", license_function, "syntax");
	SWITCH_ADD_API(api_interface, "check_module", "Check if a module is licensed", check_module_function, "<module_name>");


	licensing_log(SWITCH_LOG_NOTICE, "mod_licensing: Module loaded successfully.\n");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_licensing_shutdown)
{
	int i;

	/* Deactivate machines on shutdown (Keygen.sh best practice) */
	for (i = 0; i < globals.key_file_count; i++) {
		if (globals.keys[i].machine_id && globals.keys[i].license_id && globals.keygen_account_id) {
			switch_CURL *curl_handle = NULL;
			switch_curl_slist_t *headers = NULL;
			char url[512];
			char auth_header[256];
			long http_code = 0;

			licensing_log(SWITCH_LOG_INFO, "mod_licensing: Deactivating machine %s on shutdown\n", globals.keys[i].machine_id);

			/* Delete the machine resource using license key authentication */
			switch_snprintf(url, sizeof(url),
				"https://api.keygen.sh/v1/accounts/%s/machines/%s",
				globals.keygen_account_id, globals.keys[i].machine_id);

			switch_snprintf(auth_header, sizeof(auth_header), "Authorization: License %s", globals.keys[i].key_content);

			curl_handle = switch_curl_easy_init();
			if (curl_handle) {
				headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");
				headers = switch_curl_slist_append(headers, auth_header);

				switch_curl_easy_setopt(curl_handle, CURLOPT_URL, url);
				switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
				switch_curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
				switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
				switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
				switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
				switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

				if (switch_curl_easy_perform(curl_handle) == CURLE_OK) {
					switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
					if (http_code == 204 || http_code == 200) {
						licensing_log(SWITCH_LOG_INFO, "mod_licensing: Machine %s deactivated successfully\n", globals.keys[i].machine_id);
					} else {
						licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Machine deactivation failed (HTTP %ld)\n", http_code);
					}
				}
				switch_curl_slist_free_all(headers);
				switch_curl_easy_cleanup(curl_handle);
			}
		}
	}

	/* Free allocated source values */
	for (i = 0; i < globals.source_count; i++) {
		switch_safe_free(globals.sources[i].value);
	}

	/* Free key file paths */
	for (i = 0; i < globals.key_file_count; i++) {
		switch_safe_free(globals.key_files[i]);
	}

	/* Free key info */
	for (i = 0; i < globals.key_file_count; i++) {
		switch_safe_free(globals.keys[i].file_path);
		switch_safe_free(globals.keys[i].key_content);
		switch_safe_free(globals.keys[i].validation_msg);
		switch_safe_free(globals.keys[i].domain);
		switch_safe_free(globals.keys[i].fingerprint);
		switch_safe_free(globals.keys[i].modules);
		switch_safe_free(globals.keys[i].expires);
		switch_safe_free(globals.keys[i].machine_id);
		switch_safe_free(globals.keys[i].license_id);
	}

	/* Free licensed modules */
	for (i = 0; i < globals.licensed_module_count; i++) {
		switch_safe_free(globals.licensed_modules[i]);
	}

	/* Free keygen config */
	switch_safe_free(globals.keygen_account_id);
	switch_safe_free(globals.keygen_public_key);

	licensing_log(SWITCH_LOG_NOTICE, "mod_licensing: Module shut down.\n");

	return SWITCH_STATUS_SUCCESS;
}
