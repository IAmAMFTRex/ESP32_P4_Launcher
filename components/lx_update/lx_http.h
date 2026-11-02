/*
 * HTTPS helpers shared by the launcher update and the app catalogue (private to
 * lx_update): GitHub release files, with redirects and the certificate bundle.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_http_client.h"

esp_http_client_handle_t lx_http_new(const char *url);

/* Opens the request, following redirects. Returns the HTTP status, or -1 with
 * the reason in why. */
int lx_http_open(esp_http_client_handle_t c, char *why, size_t why_len);

/* GETs a small text file (up to max - 1 bytes) into a new buffer, with three
 * tries for a weak signal. Returns the HTTP status (-1: no answer, reason in
 * why); *out is set on 200 and freed by the caller. */
int lx_http_get_text(const char *url, size_t max, char **out, char *why, size_t why_len);

/* URL from a one-line override file if there is one, else def */
void lx_http_url(char *url, size_t len, const char *override_file, const char *def);

/* a > b for "x.y.z" versions */
bool lx_version_newer(const char *a, const char *b);
