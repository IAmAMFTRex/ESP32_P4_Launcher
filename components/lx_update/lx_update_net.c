/*
 * Online update: reads latest.json from the newest GitHub release, downloads
 * launcher.bin to the SD card and checks its SHA-256. Installing is then the same
 * as an update from the card, with the same rollback.
 *
 * latest.json: {"version":"0.19.0","url":"https://...","size":1347392,"sha256":"<hex>",
 *               "boards":{"<board id>":{"url":...,"size":...,"sha256":...}, ...}}
 * Each board takes its own entry. The top-level fields are the first board's
 * (guition_p4_43), where launchers from before per-board releases look.
 *
 * A /sd/update/ota_url.txt holding one URL replaces the release address, to test
 * against another server.
 */
#include "lx_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "bsp.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "lx_update";

#define MANIFEST_URL  "https://github.com/giltal/ESP32_P4_Launcher/releases/latest/download/latest.json"
#define URL_OVERRIDE  "/sd/update/ota_url.txt"
#define TMP_FILE      "/sd/update/launcher.tmp"
#define MANIFEST_MAX  2048
#define CHUNK         4096

static volatile lx_online_state_t s_state;
static volatile int s_percent;
static char s_version[32], s_error[80], s_sha[65];
static char s_url[256];
static long s_size;
static char s_why[40];

lx_online_state_t lx_update_online_state(void) { return s_state; }
void lx_update_online_reset(void)              { s_state = LX_ONLINE_IDLE; }
int lx_update_online_percent(void)             { return s_percent; }
const char *lx_update_online_version(void)     { return s_version; }
const char *lx_update_online_error(void)       { return s_error; }

static void fail(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
    ESP_LOGW(TAG, "online update: %s", text);
    s_state = LX_ONLINE_FAILED;
}

/* Opens url, following redirects. Returns the HTTP status, or -1 */
static int http_open(esp_http_client_handle_t c)
{
    for (int hops = 0; hops < 5; hops++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
            snprintf(s_why, sizeof(s_why), "%s", esp_err_to_name(err));
            return -1;
        }
        if (esp_http_client_fetch_headers(c) < 0) {
            ESP_LOGW(TAG, "no HTTP response");
            snprintf(s_why, sizeof(s_why), "no HTTP response");
            return -1;
        }
        int status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400) return status;
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
    }
    return -1;
}

static esp_http_client_handle_t http_new(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = CHUNK,
        .buffer_size_tx = CHUNK,                /* release downloads redirect to long URLs */
        .timeout_ms = 15000,
        .user_agent = "P4-Launcher",
    };
    return esp_http_client_init(&cfg);
}

static bool newer(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

static void check_task(void *arg)
{
    char url[256] = MANIFEST_URL;
    FILE *f = fopen(URL_OVERRIDE, "r");
    if (f) {
        if (fgets(url, sizeof(url), f)) url[strcspn(url, "\r\n")] = 0;
        fclose(f);
    }
    char *body = calloc(1, MANIFEST_MAX);
    int status = -1;
    for (int tries = 0; body && status < 0 && tries < 3; tries++) {     /* a weak signal drops some */
        if (tries) vTaskDelay(pdMS_TO_TICKS(2000));
        esp_http_client_handle_t c = http_new(url);
        if (!c) break;
        status = http_open(c);
        int got = 0;
        if (status == 200) {
            int n;
            while (got < MANIFEST_MAX - 1 && (n = esp_http_client_read(c, body + got, MANIFEST_MAX - 1 - got)) > 0)
                got += n;
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    free(body);
    /* This board's entry; the top level only stands for the first board */
    const char *id = bsp_info()->id;
    cJSON *boards = cJSON_GetObjectItem(j, "boards"), *mine = cJSON_GetObjectItem(boards, id);
    bool legacy_ok = !boards && strcmp(id, "guition_p4_43") == 0;
    cJSON *e = mine ? mine : legacy_ok ? j : NULL;
    cJSON *v = cJSON_GetObjectItem(e, "version"), *u = cJSON_GetObjectItem(e, "url");
    cJSON *s = cJSON_GetObjectItem(e, "size"), *h = cJSON_GetObjectItem(e, "sha256");
    if (!v) v = cJSON_GetObjectItem(j, "version");
    if (status == 404) {
        fail("No release has been published yet.");
    } else if (j && !e) {
        fail("No release for this board yet.");
    } else if (status < 0) {
        char text[sizeof(s_error)];
        snprintf(text, sizeof(text), "Could not reach the update server (%s).", s_why);
        fail(text);
    } else if (!cJSON_IsString(v) || !cJSON_IsString(u) || !cJSON_IsNumber(s) || !cJSON_IsString(h) ||
               strlen(h->valuestring) != 64) {
        fail("The release information is not valid.");
    } else {
        snprintf(s_version, sizeof(s_version), "%s", v->valuestring);
        snprintf(s_url, sizeof(s_url), "%s", u->valuestring);
        snprintf(s_sha, sizeof(s_sha), "%s", h->valuestring);
        s_size = (long)s->valuedouble;
        s_state = newer(s_version, esp_app_get_description()->version) ? LX_ONLINE_AVAILABLE : LX_ONLINE_LATEST;
    }
    cJSON_Delete(j);
    vTaskDelete(NULL);
}

static void download_task(void *arg)
{
    mkdir("/sd/update", 0775);
    uint8_t *buf = malloc(CHUNK);
    FILE *f = fopen(TMP_FILE, "wb");
    esp_http_client_handle_t c = http_new(s_url);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    long got = 0;
    bool ok = buf && f && c && http_open(c) == 200;
    while (ok) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) ok = false;
        if (n <= 0) break;
        mbedtls_sha256_update(&sha, buf, n);
        ok = fwrite(buf, 1, n, f) == (size_t)n;
        got += n;
        s_percent = s_size ? (int)(got * 100 / s_size) : 0;
    }
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (f) fclose(f);
    free(buf);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    if (!ok || got != s_size) {
        remove(TMP_FILE);
        fail("The download did not finish.");
    } else if (strcasecmp(hex, s_sha) != 0) {
        remove(TMP_FILE);
        fail("The download is damaged (SHA-256 mismatch).");
    } else {
        remove(LX_UPDATE_FILE);
        rename(TMP_FILE, LX_UPDATE_FILE);
        s_state = LX_ONLINE_READY;
    }
    vTaskDelete(NULL);
}

void lx_update_online_check(void)
{
    s_state = LX_ONLINE_BUSY;
    if (xTaskCreate(check_task, "lx_update", 10240, NULL, 3, NULL) != pdPASS) fail("Out of memory.");
}

void lx_update_online_download(void)
{
    s_percent = 0;
    s_state = LX_ONLINE_BUSY;
    if (xTaskCreate(download_task, "lx_update", 10240, NULL, 3, NULL) != pdPASS) fail("Out of memory.");
}
