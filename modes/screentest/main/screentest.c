// Stage 7a screen test. Corner squares and bars to check orientation and colors, GUI plus Escape returns to the loader.
#include <stdio.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "a11_keys.h"

#define PIN_MOSI 11
#define PIN_SCLK 12
#define PIN_CS 10
#define PIN_DC 46
#define PIN_BL 45
#define W 480
#define H 320

static esp_lcd_panel_handle_t panel;

static void backlight(int pct) {
    static bool init = false;
    if (!init) {
        ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT, .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
        ledc_timer_config(&t);
        ledc_channel_config_t c = {.gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        ledc_channel_config(&c);
        init = true;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static void fill(int x, int y, int w, int h, uint16_t c) {
    uint16_t *line = heap_caps_malloc(w * 2, MALLOC_CAP_DMA);
    for (int i = 0; i < w; i++) line[i] = (c >> 8) | (c << 8);   // panel expects big endian bytes
    for (int j = 0; j < h; j++) esp_lcd_panel_draw_bitmap(panel, x, y + j, x + w, y + j + 1, line);
    free(line);
}

void app_main(void) {
    spi_bus_config_t bus = {.mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = W * 2 * 4};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {.dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS, .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io));
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &pc, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, false);
    esp_lcd_panel_disp_on_off(panel, true);

    fill(0, 0, W, H, rgb(0, 0, 0));
    fill(0, 0, 60, 60, rgb(255, 0, 0));               // top left red
    fill(W - 60, 0, 60, 60, rgb(0, 255, 0));          // top right green
    fill(0, H - 60, 60, 60, rgb(0, 0, 255));          // bottom left blue
    fill(W - 60, H - 60, 60, 60, rgb(255, 255, 255)); // bottom right white
    fill(100, 140, 280, 40, rgb(0x39, 0xFF, 0x14));   // neon green bar in the middle
    fill(100, 100, 100, 20, rgb(255, 255, 0));        // yellow short bar, left of center, above
    backlight(100);
    printf("SCREENTEST drawn. Corners: top left red, top right green, bottom left blue, bottom right white.\n");

    QueueHandle_t q = xQueueCreate(16, sizeof(a11_event_t));
    a11_keys_start(q);
    bool gui = false;
    int bl = 100;
    for (;;) {
        a11_event_t ev;
        if (xQueueReceive(q, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.code == A11_HID_LEFT_GUI) gui = ev.down;
        if (ev.down && ev.code == A11_KEY_BACKLIGHT) { bl = bl <= 20 ? 100 : bl - 40; backlight(bl); printf("backlight %d\n", bl); }
        if (ev.down && gui && ev.code == A11_HID_ESCAPE) {
            esp_ota_set_boot_partition(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL));
            esp_restart();
        }
    }
}
