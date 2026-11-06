/*
 * Capacitive touch over the shared I2C bus: GT911 (ILI9881C panel) or the touch built into
 * the ST7123 / ST7121 panels at 0x55 (protocol from espressif/esp_lcd_touch_st7123). Polled:
 * the board's INT pin is not wired to the P4. Raw points are in the controller's
 * resolution (panel native orientation) and are mapped to landscape here.
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "touch";

#define GT911_ADDR_A      0x5D
#define GT911_ADDR_B      0x14
#define GT911_REG_CONFIG  0x8047    /* config version, then x_max u16, y_max u16 */
#define GT911_REG_PID     0x8140
#define GT911_REG_STATUS  0x814E
#define GT911_REG_POINTS  0x814F
#define GT911_POINT_SIZE  8

#define ST_ADDR           0x55
#define ST_REG_MAX_XY     0x0005    /* x_max u16 BE, y_max u16 BE, max touches */
#define ST_REG_ADV_INFO   0x0010    /* bit 3: report has coordinates */
#define ST_REG_MAX_TOUCH  0x0009
#define ST_REG_POINTS     0x0014    /* 7 bytes each: valid|x_h, x_l, y_h, y_l, area, intensity, - */
#define ST_POINT_SIZE     7

static bool s_st;                   /* ST7123 / ST7121 touch, not the GT911 */

static i2c_master_dev_handle_t s_dev;
static uint16_t s_raw_w = BOARD_LCD_H_RES, s_raw_h = BOARD_LCD_V_RES;
static bsp_touch_point_t s_last[BSP_TOUCH_MAX_POINTS];   /* last report: the GT911 only */
static int s_last_n;                                     /* flags data when it changes  */

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, addr, 2, buf, len, 50);
}

static esp_err_t gt_write8(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_master_transmit(s_dev, buf, 3, 50);
}

esp_err_t bsp_touch_init(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    uint16_t addr = 0;
    s_st = board_panel() != BOARD_PANEL_ILI9881C;
    if (!s_st) board_touch_reset();
    if (s_st) addr = ST_ADDR;
    else if (i2c_master_probe(bus, GT911_ADDR_A, 50) == ESP_OK) addr = GT911_ADDR_A;
    else if (i2c_master_probe(bus, GT911_ADDR_B, 50) == ESP_OK) addr = GT911_ADDR_B;
    if (!addr) {
        ESP_LOGW(TAG, "GT911 not found on I2C");
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = s_st ? 100000 : 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "add device");

    if (s_st) {
        uint8_t info[5] = { 0 };
        if (gt_read(ST_REG_MAX_XY, info, sizeof(info)) != ESP_OK) {
            ESP_LOGE(TAG, "%s touch read failed", board_panel_name());
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            return ESP_FAIL;
        }
        uint16_t w = info[0] << 8 | info[1], h = info[2] << 8 | info[3];
        if (w && h) { s_raw_w = w; s_raw_h = h; }
        ESP_LOGI(TAG, "%s touch at 0x%02X, %ux%u, %u points", board_panel_name(), ST_ADDR, s_raw_w, s_raw_h, info[4]);
        board_set_cap(BSP_CAP_TOUCH);
        return ESP_OK;
    }

    uint8_t pid[4] = { 0 };
    uint8_t cfg[5] = { 0 };
    if (gt_read(GT911_REG_PID, pid, sizeof(pid)) != ESP_OK ||
        gt_read(GT911_REG_CONFIG, cfg, sizeof(cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 read failed");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_FAIL;
    }
    uint16_t w = cfg[1] | cfg[2] << 8, h = cfg[3] | cfg[4] << 8;
    if (w && h) { s_raw_w = w; s_raw_h = h; }
    ESP_LOGI(TAG, "GT%.4s at 0x%02X, config v%u, %ux%u", (char *)pid, addr, cfg[0], s_raw_w, s_raw_h);

    board_set_cap(BSP_CAP_TOUCH);
    return ESP_OK;
}

/* raw controller point -> landscape */
static bsp_touch_point_t to_landscape(int rx, int ry, int size, int id)
{
    const bsp_info_t *b = bsp_info();
    int px = rx * b->display_w / s_raw_w;       /* raw -> panel native pixels */
    int py = ry * b->display_h / s_raw_h;
    if (px >= b->display_w) px = b->display_w - 1;
    if (py >= b->display_h) py = b->display_h - 1;
    int x = px, y = py;                         /* panel -> landscape: inverse of the mapping in bsp.h */
    if (b->display_rot == 270)     { x = py; y = b->display_w - 1 - px; }
    else if (b->display_rot == 90) { x = b->display_h - 1 - py; y = px; }
    return (bsp_touch_point_t){ .x = x, .y = y, .size = size, .id = id };
}

static int st_read(bsp_touch_point_t *pts, int max)
{
    uint8_t adv, count = 0;
    if (gt_read(ST_REG_ADV_INFO, &adv, 1) != ESP_OK) return -1;
    uint8_t raw[10 * ST_POINT_SIZE];
    if (adv & 0x08) {
        if (gt_read(ST_REG_MAX_TOUCH, &count, 1) != ESP_OK) return -1;
        if (count > 10) count = 10;
        if (count && gt_read(ST_REG_POINTS, raw, count * ST_POINT_SIZE) != ESP_OK) return -1;
    }
    int n = 0;
    for (int i = 0; i < count && n < max && n < BSP_TOUCH_MAX_POINTS; i++) {
        const uint8_t *p = raw + i * ST_POINT_SIZE;
        if (!(p[0] & 0x80)) continue;
        pts[n++] = to_landscape((p[0] & 0x3F) << 8 | p[1], p[2] << 8 | p[3], p[4], i);
    }
    return n;
}

int bsp_touch_read(bsp_touch_point_t *pts, int max)
{
    if (!s_dev) return -1;
    if (s_st) return st_read(pts, max);
    uint8_t status;
    if (gt_read(GT911_REG_STATUS, &status, 1) != ESP_OK) return -1;
    if (!(status & 0x80)) goto out;     /* nothing new since the last report */

    int n = status & 0x0F;
    if (n > BSP_TOUCH_MAX_POINTS) n = BSP_TOUCH_MAX_POINTS;
    uint8_t raw[BSP_TOUCH_MAX_POINTS * GT911_POINT_SIZE];
    esp_err_t err = n ? gt_read(GT911_REG_POINTS, raw, n * GT911_POINT_SIZE) : ESP_OK;
    gt_write8(GT911_REG_STATUS, 0);
    if (err != ESP_OK) return -1;

    for (int i = 0; i < n; i++) {
        const uint8_t *p = raw + i * GT911_POINT_SIZE;
        s_last[i] = to_landscape(p[1] | p[2] << 8, p[3] | p[4] << 8, p[5] | p[6] << 8, p[0]);
    }
    s_last_n = n;
out:
    n = s_last_n < max ? s_last_n : max;
    memcpy(pts, s_last, n * sizeof(*pts));
    return n;
}
