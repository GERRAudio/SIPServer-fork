/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * mod_licensing_online.c -- Online license validation with Keygen.sh
 *
 */
#include "mod_licensing_online.h"

/* ============================================================
 * Machine Validation with Keygen
 * Verifies an existing machine ID is still valid
 * ============================================================ */
switch_status_t validate_machine_with_keygen(const char *machine_id, const char *license_key)
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
char *find_machine_by_fingerprint(const char *license_key, const char *fingerprint)
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
void get_machine_id_file_path(const char *license_file_path, char *machine_id_path, size_t path_size)
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

char *load_machine_id_from_file(const char *machine_id_file_path)
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

switch_status_t save_machine_id_to_file(const char *machine_id_file_path, const char *machine_id)
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

/* ============================================================
 * Machine Activation
 * Creates a new machine on the license
 * ============================================================ */
switch_status_t activate_machine_on_license(const char *license_id, const char *license_key, const char *fingerprint, const char *license_token, char **machine_id_out)
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

/* ============================================================
 * Machine Deactivation
 * Deletes a machine from the license (called on shutdown)
 * ============================================================ */
switch_status_t deactivate_machine_on_license(const char *machine_id, const char *license_key)
{
	switch_CURL *curl_handle = NULL;
	switch_curl_slist_t *headers = NULL;
	char url[512];
	char auth_header[256];
	long http_code = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (zstr(machine_id) || zstr(license_key) || zstr(globals.keygen_account_id)) {
		return SWITCH_STATUS_FALSE;
	}

	licensing_log(SWITCH_LOG_INFO, "mod_licensing: Deactivating machine %s\n", machine_id);

	/* Delete the machine resource using license key authentication */
	switch_snprintf(url, sizeof(url),
		"https://api.keygen.sh/v1/accounts/%s/machines/%s",
		globals.keygen_account_id, machine_id);

	switch_snprintf(auth_header, sizeof(auth_header), "Authorization: License %s", license_key);

	curl_handle = switch_curl_easy_init();
	if (!curl_handle) {
		return SWITCH_STATUS_FALSE;
	}

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
			licensing_log(SWITCH_LOG_INFO, "mod_licensing: Machine %s deactivated successfully\n", machine_id);
			status = SWITCH_STATUS_SUCCESS;
		} else {
			licensing_log(SWITCH_LOG_WARNING, "mod_licensing: Machine deactivation failed (HTTP %ld)\n", http_code);
		}
	}

	switch_curl_slist_free_all(headers);
	switch_curl_easy_cleanup(curl_handle);

	return status;
}

/* ============================================================
 * Online Key Validation (Keygen.sh)
 * Best Practices:
 * 1. Validate the license key with fingerprint scope
 * 2. Parse the JSON response to check maxMachines limit
 * 3. If valid and maxMachines allows, activate this machine
 * 4. Store machine ID for future check-ins and deactivation
 * ============================================================ */

switch_status_t validate_key_online(const char *key_content, const char *file_path, license_key_t *key_info)
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
						/* Special handling for "all" - allows all modules */
						if (strcasecmp(modules_obj->valuestring, "all") == 0) {
							licensing_log(SWITCH_LOG_INFO, "mod_licensing: License allows ALL modules\n");
							add_licensed_module("all");
						} else {
							parse_modules_list(modules_obj->valuestring);
						}
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
