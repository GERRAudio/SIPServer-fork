/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * mod_licensing_json.c -- JSON parsing and utility functions
 *
 */
#include "mod_licensing_json.h"

/* ============================================================
 * Module List Parsing
 * Supports comma, slash, and semicolon delimiters
 * ============================================================ */
void parse_modules_list(const char *modules_str)
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

/* ============================================================
 * Base64 Decode Utility
 * Uses OpenSSL BIO for decoding
 * ============================================================ */
char *base64_decode(const char *input, size_t *output_len)
{
	BIO *bio, *b64;
	char *decoded = NULL;
	size_t input_len = strlen(input);

	decoded = (char *)malloc(input_len + 1);
	if (!decoded) {
		return NULL;
	}

	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new_mem_buf(input, (int)input_len);
	bio = BIO_push(b64, bio);
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	*output_len = BIO_read(bio, decoded, (int)input_len);
	BIO_free_all(bio);

	if ((int)*output_len <= 0) {
		free(decoded);
		return NULL;
	}

	decoded[*output_len] = '\0';
	return decoded;
}

/* ============================================================
 * HTTP Response Callback for CURL
 * Writes response data to a stream
 * ============================================================ */
size_t http_response_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t realsize = size * nmemb;
	switch_stream_handle_t *stream = (switch_stream_handle_t *)userdata;

	stream->write_function(stream, "%.*s", (int)realsize, (char *)ptr);
	return realsize;
}
