/*
 * RX8025T real-time clock on the shared I2C bus (0x32), keeping UTC.
 * From the ESP32_P4_Internet_Radio project's clock.c, which runs on this board.
 */
#include "board_internal.h"

#include <time.h>
#include "esp_log.h"

static const char *TAG = "rtc";

#define RTC_ADDR      0x32
#define REG_SEC       0x00
#define REG_FLAG      0x0E
#define FLAG_VLF      0x02          /* oscillator stopped / battery lost: the time is garbage */
#define VALID_EPOCH   1704067200    /* 2024-01-01: anything earlier is an unset clock */
#define LAST_EPOCH    4102444800    /* 2100-01-01: anything later is a misread one */

static i2c_master_dev_handle_t s_rtc;

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(int d)     { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

/* UTC broken-down time -> seconds since 1970 (newlib has no timegm) */
static int64_t utc_to_epoch(const struct tm *t)
{
    int y = t->tm_year + 1900, m = t->tm_mon + 1, d = t->tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_rtc, &reg, 1, buf, len, 100);
}

esp_err_t bsp_rtc_init(void)
{
    if (s_rtc) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_rtc);
    if (err != ESP_OK) return err;
    uint8_t flag;
    if (rtc_read(REG_FLAG, &flag, 1) != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T not responding");
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    board_set_cap(BSP_CAP_RTC);
    return ESP_OK;
}

esp_err_t bsp_rtc_get(int64_t *utc)
{
    if (!s_rtc) return ESP_ERR_NOT_SUPPORTED;
    uint8_t flag = 0, r[7];
    if (rtc_read(REG_FLAG, &flag, 1) != ESP_OK || rtc_read(REG_SEC, r, sizeof(r)) != ESP_OK) return ESP_FAIL;
    struct tm t = {
        .tm_sec = bcd2dec(r[0] & 0x7F), .tm_min = bcd2dec(r[1] & 0x7F), .tm_hour = bcd2dec(r[2] & 0x3F),
        .tm_mday = bcd2dec(r[4] & 0x3F), .tm_mon = bcd2dec(r[5] & 0x1F) - 1, .tm_year = bcd2dec(r[6]) + 100,
    };
    bool sane = !(flag & FLAG_VLF) && t.tm_mon >= 0 && t.tm_mon < 12 && t.tm_mday >= 1 && t.tm_mday <= 31 &&
                t.tm_hour < 24 && t.tm_min < 60 && t.tm_sec < 60;
    int64_t epoch = sane ? utc_to_epoch(&t) : 0;
    if (epoch < VALID_EPOCH || epoch >= LAST_EPOCH) return ESP_ERR_INVALID_STATE;
    *utc = epoch;
    return ESP_OK;
}

esp_err_t bsp_rtc_set(int64_t utc)
{
    if (!s_rtc) return ESP_ERR_NOT_SUPPORTED;
    if (utc < VALID_EPOCH) return ESP_ERR_INVALID_ARG;
    time_t now = (time_t)utc;
    struct tm t;
    gmtime_r(&now, &t);
    uint8_t buf[8] = {
        REG_SEC, dec2bcd(t.tm_sec), dec2bcd(t.tm_min), dec2bcd(t.tm_hour),
        (uint8_t)(1 << t.tm_wday), dec2bcd(t.tm_mday), dec2bcd(t.tm_mon + 1), dec2bcd(t.tm_year - 100),
    };
    esp_err_t err = i2c_master_transmit(s_rtc, buf, sizeof(buf), 100);
    if (err == ESP_OK) {
        uint8_t clear[2] = { REG_FLAG, 0 };     /* clear VLF: the time is good now */
        err = i2c_master_transmit(s_rtc, clear, sizeof(clear), 100);
    }
    return err;
}
