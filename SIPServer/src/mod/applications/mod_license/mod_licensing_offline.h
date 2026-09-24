/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * mod_licensing_offline.h -- Offline license validation
 *
 */
#ifndef MOD_LICENSING_OFFLINE_H
#define MOD_LICENSING_OFFLINE_H

#include "mod_licensing_common.h"
#include "mod_licensing_json.h"

/* Offline validation functions */
switch_status_t validate_key_offline(const char *key_content, const char *file_path, license_key_t *key_info);
switch_status_t validate_keygen_signed_license(const char *key_content, const char *file_path, license_key_t *key_info);

#endif /* MOD_LICENSING_OFFLINE_H */
