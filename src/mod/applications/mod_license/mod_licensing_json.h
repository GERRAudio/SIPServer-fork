/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * mod_licensing_json.h -- JSON parsing and utility functions
 *
 */
#ifndef MOD_LICENSING_JSON_H
#define MOD_LICENSING_JSON_H

#include "mod_licensing_common.h"

/* Module list parsing */
void parse_modules_list(const char *modules_str);

/* Base64 decoding utility */
char *base64_decode(const char *input, size_t *output_len);

/* CURL response callback */
size_t http_response_callback(void *ptr, size_t size, size_t nmemb, void *data);

#endif /* MOD_LICENSING_JSON_H */
