/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * mod_licensing_online.h -- Online license validation with Keygen.sh
 *
 */
#ifndef MOD_LICENSING_ONLINE_H
#define MOD_LICENSING_ONLINE_H

#include "mod_licensing_common.h"
#include "mod_licensing_json.h"

/* Online validation functions */
switch_status_t validate_key_online(const char *key_content, const char *file_path, license_key_t *key_info);
switch_status_t activate_machine_on_license(const char *license_id, const char *license_key, const char *fingerprint, const char *license_token, char **machine_id_out);
switch_status_t deactivate_machine_on_license(const char *machine_id, const char *license_key);
switch_status_t validate_machine_with_keygen(const char *machine_id, const char *license_key);
char *find_machine_by_fingerprint(const char *license_key, const char *fingerprint);

/* Machine ID persistence helpers */
void get_machine_id_file_path(const char *license_file_path, char *machine_id_path, size_t path_size);
char *load_machine_id_from_file(const char *machine_id_file_path);
switch_status_t save_machine_id_to_file(const char *machine_id_file_path, const char *machine_id);

#endif /* MOD_LICENSING_ONLINE_H */
