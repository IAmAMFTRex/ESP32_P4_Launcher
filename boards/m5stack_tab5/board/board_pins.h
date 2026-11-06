/* M5Stack Tab5 (ESP32-P4, 5" 720x1280 MIPI-DSI, GT911, ES8388, RX8130) pin map.
 * From M5Stack M5Tab5-UserDemo (platforms/tab5/components/m5stack_tab5). */
#pragma once

#define BOARD_LCD_H_RES     720     /* portrait panel */
#define BOARD_LCD_V_RES     1280
#define BOARD_LCD_BL_GPIO   22      /* PWM */
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_MV   2500
#define BOARD_DSI_LANE_MBPS    730

#define BOARD_I2C_SDA       31      /* touch, codec, expanders, RTC, power monitor */
#define BOARD_I2C_SCL       32
#define BOARD_I2C_PORT      1
#define BOARD_TOUCH_INT     23      /* GT911 INT; its reset is I/O expander 1 P5 */

/* PI4IOE5V6408 I/O expanders */
#define BOARD_IOX1_ADDR     0x43    /* P1 speaker amp, P4 LCD reset (released as input + pull-up) */
#define BOARD_IOX2_ADDR     0x44    /* P0 C6 Wi-Fi power, P3 USB 5 V out, P7 charging */

#define BOARD_I2S_MCLK      30
#define BOARD_I2S_BCLK      27
#define BOARD_I2S_WS        29
#define BOARD_I2S_DOUT      26      /* to the ES8388 */
#define BOARD_I2S_DIN       28      /* from the ES7210 microphones (not used) */
#define BOARD_AUDIO_RATE    48000

#define BOARD_SD_CLK        43
#define BOARD_SD_CMD        44
#define BOARD_SD_D0         39
#define BOARD_SD_D1         40
#define BOARD_SD_D2         41
#define BOARD_SD_D3         42
#define BOARD_SD_LDO_CHAN   4
