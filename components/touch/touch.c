#include "touch.h"
#include <stdlib.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "i2c_bus.h"
#include "esp_log.h"

#define FT_ADDR 0x38
static const char *TAG = "touch";
static i2c_master_dev_handle_t dev;
static QueueHandle_t q;

static bool read_touch(int *n, int *rx, int *ry) {
    uint8_t reg = 0x02, buf[5];
    if (i2c_master_transmit_receive(dev, &reg, 1, buf, sizeof buf, 50) != ESP_OK) return false;
    *n = buf[0] & 0x0F;
    *rx = ((buf[1] & 0x0F) << 8) | buf[2];
    *ry = ((buf[3] & 0x0F) << 8) | buf[4];
    return true;
}

static void map(int rx, int ry, int16_t *x, int16_t *y) {
    int mx = BOARD_TOUCH_SWAP_XY ? ry : rx, my = BOARD_TOUCH_SWAP_XY ? rx : ry;
    int w = BOARD_LCD_WIDTH, h = BOARD_LCD_HEIGHT;
    if (BOARD_TOUCH_FLIP_X) mx = w - 1 - mx;
    if (BOARD_TOUCH_FLIP_Y) my = h - 1 - my;
    *x = (int16_t)(mx < 0 ? 0 : mx >= w ? w - 1 : mx);
    *y = (int16_t)(my < 0 ? 0 : my >= h ? h - 1 : my);
}

static void touch_task(void *arg) {
    bool down = false;
    int16_t lx = 0, ly = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15));
        int n, rx, ry;
        if (!read_touch(&n, &rx, &ry)) continue;
        if (n == 0 || n > 2) {
            if (down) { touch_event_t e = {TOUCH_UP, lx, ly}; xQueueSend(q, &e, 0); down = false; }
            continue;
        }
        int16_t x, y;
        map(rx, ry, &x, &y);
        if (!down) {
            ESP_LOGI(TAG, "touch raw %d,%d -> %d,%d", rx, ry, x, y);
            touch_event_t e = {TOUCH_DOWN, x, y};
            xQueueSend(q, &e, 0);
            down = true;
        } else if (abs(x - lx) >= 2 || abs(y - ly) >= 2) {
            touch_event_t e = {TOUCH_MOVE, x, y};
            xQueueSend(q, &e, 0);
        }
        lx = x; ly = y;
    }
}

bool touch_start(QueueHandle_t events) {
    q = events;
    gpio_config_t rst = {.pin_bit_mask = 1ULL << BOARD_TOUCH_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(BOARD_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) return false;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = FT_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return false;
    int n, rx, ry;
    if (!read_touch(&n, &rx, &ry)) { ESP_LOGW(TAG, "touch chip did not answer"); return false; }
    ESP_LOGI(TAG, "touch ready");
    xTaskCreate(touch_task, "touch", 4096, NULL, 5, NULL);
    return true;
}

// Puts the touch chip into hibernate. It wakes again through its reset line in touch_start.
void touch_sleep(void) {
    uint8_t b[2] = {0xA5, 0x03};
    if (dev) i2c_master_transmit(dev, b, 2, 50);
}
