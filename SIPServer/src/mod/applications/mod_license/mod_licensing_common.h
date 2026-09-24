/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * mod_licensing_common.h -- Common types, structures, and declarations
 *
 */
#ifndef MOD_LICENSING_COMMON_H
#define MOD_LICENSING_COMMON_H

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

/* Constants */
#define MAX_KEY_SOURCES 32
#define MAX_KEY_FILES 256
#define ONLINE_KEY_LENGTH 37
#define MAX_MAC_ADDRESSES 16
#define MAX_LICENSED_MODULES 128

/* Type Definitions */
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

/* Global State Structure */
typedef struct {
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
} licensing_globals_t;

/* External global state - defined in mod_licensing.c */
extern licensing_globals_t globals;

/* Common utility functions */
void licensing_log(switch_log_level_t level, const char *fmt, ...);
void add_licensed_module(const char *module_name);
switch_bool_t domain_matches(const char *license_domain);
switch_bool_t fingerprint_matches(const char *license_fingerprint);

#endif /* MOD_LICENSING_COMMON_H */
