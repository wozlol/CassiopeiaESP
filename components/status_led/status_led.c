#include "status_led.h"
#include <math.h>
#include <string.h>
#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "status_led";

#define PULSE_US 3000000LL   // one slow pulse while charging
#define CHARGE_RISE_MV 8     // voltage up this much over two minutes counts as charging
#define QUIET_US 5000000LL   // this long with no output after a response counts as finished
#define NO_BATTERY_MV 2500   // the sense pin reads below this when nothing is connected

static led_strip_handle_t strip;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static volatile bool armed, got_output, done, off, override_on;
static volatile int64_t last_output;
static volatile int bat_mv = -1;
static volatile int screen_pct = 100;
static volatile bool charging, fast_sampling;
static volatile int64_t charge_since;
#define RING 90   // battery voltage every 10 s, fifteen minutes back
static struct { int64_t t; int mv; } ring[RING];
static int ring_n;

static int pct_from_mv(int v);
bool status_led_battery(int *mv, int *pct) {
    int v = bat_mv;
    if (v < NO_BATTERY_MV) return false;
    if (mv) *mv = v;
    if (pct) *pct = pct_from_mv(v);
    return true;
}

void status_led_set_brightness(int percent) { screen_pct = percent < 1 ? 1 : percent > 100 ? 100 : percent; }
bool status_led_charging(void) { return charging; }

static int pct_from_mv(int v) {
    // Rough LiPo curve, 4.20 V full, 3.30 V empty.
    static const int pts[][2] = {{3300, 0}, {3600, 15}, {3700, 30}, {3800, 50}, {4000, 80}, {4200, 100}};
    if (v <= pts[0][0]) return 0;
    for (int i = 1; i < 6; i++)
        if (v <= pts[i][0]) return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (v - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
    return 100;
}

// Charging is a guess from the voltage trend, the board has no charge pin we can read. Rising by a few millivolts over two minutes means charging.
static void update_charging(int mv, int64_t now) {
    static int64_t last_store;
    if (now - last_store < 9500000LL && ring_n) return;   // the trend uses one sample every 10 s
    last_store = now;
    if (ring_n == RING) { memmove(ring, ring + 1, sizeof ring - sizeof ring[0]); ring_n--; }
    ring[ring_n].t = now;
    ring[ring_n].mv = mv;
    ring_n++;
    if (ring_n < 13) return;
    int oldest = ring[ring_n - 13].mv;   // two minutes back
    if (!charging && mv - oldest >= CHARGE_RISE_MV) { charging = true; charge_since = ring[ring_n - 13].t; }
    else if (charging && mv - oldest <= -CHARGE_RISE_MV / 2) { charging = false; charge_since = 0; }
}

bool status_led_battery_info(battery_info_t *o) {
    memset(o, 0, sizeof *o);
    o->eta_min = -1;
    int mv, pct;
    if (!status_led_battery(&mv, &pct)) return false;
    o->mv = mv;
    o->pct = pct;
    o->charging = charging;
    if (!charging) return true;
    int64_t now = esp_timer_get_time();
    o->charge_secs = charge_since ? (int)((now - charge_since) / 1000000) : 0;
    // Speed: average of the first and the last minute of the last ten minutes, to smooth the readings.
    int n = ring_n < 61 ? ring_n : 61, base = ring_n - n;
    if (n >= 20) {
        int a = 0, b = 0;
        for (int i = 0; i < 6; i++) { a += ring[base + i].mv; b += ring[ring_n - 1 - i].mv; }
        a /= 6; b /= 6;
        int64_t span = (ring[ring_n - 1 - 2].t + ring[ring_n - 1 - 3].t) / 2 - (ring[base + 2].t + ring[base + 3].t) / 2;
        if (span >= 150000000LL) {
            float rate = (pct_from_mv(b) - pct_from_mv(a)) * 3600.0f / (span / 1000000.0f);
            if (rate < 0) rate = 0;
            o->rate_known = true;
            o->rate_pct_x10_per_h = (int)(rate * 10);
            o->ma = (int)(rate * BOARD_BATTERY_MAH / 100.0f);
            if (pct >= 97) o->eta_min = 0;
            else if (rate >= 1.0f) o->eta_min = (int)((100 - pct) / rate * 60);
        }
    }
    return true;
}

void status_led_fast_sampling(bool fast) { fast_sampling = fast; }

static void sample_battery(void) {
    if (!adc) return;
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw, mv;
        if (adc_oneshot_read(adc, BOARD_BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) { sum += mv; n++; }
    }
    if (n) { bat_mv = sum / n * BOARD_BAT_DIVIDER; update_charging(bat_mv, esp_timer_get_time()); }
}

static uint8_t gamma8(float v) {   // brightness steps feel even when squared
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v * v / 255.0f);
}

static void led_task(void *arg) {
    float blue = 0;
    int64_t last_sample = -100000000LL;
    uint32_t last_rgb = 0xFFFFFFFF;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(40));
        int64_t now = esp_timer_get_time();
        if (override_on) continue;
        if (off) {
            if (last_rgb != 0) { led_strip_clear(strip); last_rgb = 0; }
            continue;
        }
        if (now - last_sample > (fast_sampling ? 1000000LL : 10000000LL)) { sample_battery(); last_sample = now; }
        if (armed && got_output && now - last_output > QUIET_US) { done = true; armed = false; }
        float target = done ? 1.0f : 0.0f;
        blue += (target - blue) * (done ? 0.06f : 0.25f);
        if (blue < 0.002f) blue = 0;
        int mv, pct;
        float r = 0, g = 0, b = 0;
        // Brightness follows the screen, from fairly dim at the lowest screen level to bright at the top.
        float peak = 70.0f + (screen_pct - 10) * 130.0f / 90.0f;
        if (peak < 70) peak = 70;
        if (status_led_battery(&mv, &pct)) {
            float level = peak;
            if (charging) level *= 0.45f + 0.55f * (0.5f + 0.5f * sinf((float)(now % PULSE_US) * 6.2831853f / PULSE_US));
            if (pct >= 40) { g = level; }
            else if (pct >= 15) { r = level; g = level * 0.55f; }
            else { r = level; }
        }
        r *= 1 - blue; g *= 1 - blue;
        b = peak * 1.1f * blue;
        uint8_t R = gamma8(r), G = gamma8(g), B = gamma8(b);
        uint32_t rgb = (R << 16) | (G << 8) | B;
        if (rgb != last_rgb) {
            led_strip_set_pixel(strip, 0, R, G, B);
            led_strip_refresh(strip);
            last_rgb = rgb;
        }
    }
}

void status_led_user_key(bool enter) {
    done = false;
    got_output = false;
    armed = enter;
    last_output = esp_timer_get_time();
}

void status_led_output(void) {
    last_output = esp_timer_get_time();
    if (armed) got_output = true;
}

void status_led_off(bool o) { off = o; }

// A colour set directly, for effects like the trip back to the loader. It also works in modes that never started the LED.
void status_led_override(bool on, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip) {
        led_strip_config_t sc = {.strip_gpio_num = BOARD_RGB_LED_GPIO, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                                 .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};
        led_strip_rmt_config_t rc = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
        if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK) return;
    }
    override_on = on;
    if (on) { led_strip_set_pixel(strip, 0, r, g, b); led_strip_refresh(strip); }
    else led_strip_clear(strip);
}

void status_led_start(void) {
    led_strip_config_t sc = {.strip_gpio_num = BOARD_RGB_LED_GPIO, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                             .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};
    led_strip_rmt_config_t rc = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK) { ESP_LOGW(TAG, "LED did not start"); return; }
    led_strip_clear(strip);
    adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&uc, &adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        adc_oneshot_config_channel(adc, BOARD_BAT_ADC_CHANNEL, &cc);
        adc_cali_curve_fitting_config_t cf = {.unit_id = ADC_UNIT_1, .chan = BOARD_BAT_ADC_CHANNEL, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_cali_create_scheme_curve_fitting(&cf, &cali) != ESP_OK) cali = NULL;
    }
    xTaskCreate(led_task, "status_led", 3072, NULL, 1, NULL);
}
