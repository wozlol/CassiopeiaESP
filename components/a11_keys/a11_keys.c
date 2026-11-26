#include "a11_keys.h"
#include <string.h>
#include "esp_log.h"
#include "nrf_swd.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"
#include "esp_sleep.h"

#define MB_BASE 0x20000000UL
#define MB_MAGIC 0x4B425343UL
#define MAX_PADS 32
static const char *TAG = "a11_keys";

static QueueHandle_t events_q;
static volatile bool slow_poll, slow_mode;
static volatile uint32_t ev_count, poll_count;
uint32_t a11_keys_event_count(void) { return ev_count; }
uint32_t a11_keys_poll_count(void) { return poll_count; }
void a11_keys_set_slow(bool slow) { slow_poll = slow; slow_mode = slow; }

static bool load_header(int *n, uint8_t *pads) {
    uint32_t w[36];
    if (!nrf_swd_read_words(MB_BASE, w, 36)) return false;
    if (w[0] != MB_MAGIC || w[2] == 0 || w[2] > MAX_PADS) return false;
    *n = (int)w[2];
    for (int i = 0; i < *n; i++) pads[i] = (uint8_t)w[4 + i];
    return true;
}

#define MB_CTRL 69
#define MB_WAKE_KEY 70
#define MB_LATCH 71
#define MB_ROWS_AT_WAKE 72
#define MB_TAG 96
#define FW_TAG 0x57414B31UL
#define IO3_GPIO 3
#define IO3_PAD 25
#define P0_OUTCLR 0x5000050CUL

// Which keys a set of scanned rows holds down, by the pad pairs in the key map.
static void rows_to_keys(int n, const uint8_t *pads, const uint32_t *rows, bool *now) {
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (!(((rows[i] >> j) | (rows[j] >> i)) & 1)) continue;
            uint8_t a = pads[i] < pads[j] ? pads[i] : pads[j], b = pads[i] < pads[j] ? pads[j] : pads[i];
            for (int k = 0; k < A11_KEY_COUNT; k++)
                if (A11_KEYMAP[k].pad_a == a && A11_KEYMAP[k].pad_b == b) { now[k] = true; break; }
        }
    }
}

static int wake_n = -1;
static uint16_t wake_codes[16];

int a11_keys_wake_held(uint16_t *codes, int max) {
    if (wake_n < 0) return -1;
    int n = wake_n < max ? wake_n : max;
    for (int i = 0; i < n; i++) codes[i] = wake_codes[i];
    return n;
}

// Reads the keys the nRF remembered from the moment On woke us, then tells it the ESP is awake and drops IO3.
static void read_wake_latch(void) {
    uint32_t id, tag = 0, latch = 0, rows[MAX_PADS];
    uint8_t pads[MAX_PADS];
    int n = 0;
    nrf_swd_lock();
    if (nrf_swd_connect(&id) && nrf_swd_read_word(MB_BASE + MB_TAG * 4, &tag) && tag == FW_TAG && load_header(&n, pads)) {
        nrf_swd_read_word(MB_BASE + MB_LATCH * 4, &latch);
        if (latch == 1 && nrf_swd_read_words(MB_BASE + MB_ROWS_AT_WAKE * 4, rows, n)) {
            bool now[A11_KEY_COUNT] = {0};
            rows_to_keys(n, pads, rows, now);
            wake_n = 0;
            for (int k = 0; k < A11_KEY_COUNT && wake_n < 16; k++)
                if (now[k]) wake_codes[wake_n++] = A11_KEYMAP[k].code;
        }
        nrf_swd_write_word(MB_BASE + MB_CTRL * 4, 0);
        nrf_swd_write_word(MB_BASE + MB_LATCH * 4, 0);
        nrf_swd_write_word(P0_OUTCLR, 1UL << IO3_PAD);
    }
    nrf_swd_disconnect();   // the debug port stays off unless the I2C link fails
    nrf_swd_unlock();
}

bool a11_keys_prepare_sleep(void) {
    uint8_t pa = 0, pb = 0;
    for (int k = 0; k < A11_KEY_COUNT; k++)
        if (A11_KEYMAP[k].code == A11_KEY_ON) { pa = A11_KEYMAP[k].pad_a; pb = A11_KEYMAP[k].pad_b; }
    if (!pa) return false;
    nrf_swd_lock();
    uint32_t id, tag = 0, ctrl = 0, key = 0, latch = 1;
    bool ok = nrf_swd_connect(&id) && nrf_swd_read_word(MB_BASE + MB_TAG * 4, &tag) && tag == FW_TAG &&
              nrf_swd_write_word(MB_BASE + MB_LATCH * 4, 0) && nrf_swd_write_word(MB_BASE + MB_WAKE_KEY * 4, pa | (pb << 8)) &&
              nrf_swd_write_word(P0_OUTCLR, 1UL << IO3_PAD) && nrf_swd_write_word(MB_BASE + MB_CTRL * 4, 1) &&
              nrf_swd_read_word(MB_BASE + MB_CTRL * 4, &ctrl) && nrf_swd_read_word(MB_BASE + MB_WAKE_KEY * 4, &key) &&
              nrf_swd_read_word(MB_BASE + MB_LATCH * 4, &latch);
    ok = ok && ctrl == 1 && key == (uint32_t)(pa | (pb << 8)) && latch == 0;
    if (ok) {
        // IO3 must be low now, or the ESP would wake straight away.
        gpio_config_t c = {.pin_bit_mask = 1ULL << IO3_GPIO, .mode = GPIO_MODE_INPUT, .pull_down_en = 1};
        gpio_config(&c);
        vTaskDelay(pdMS_TO_TICKS(20));
        ok = gpio_get_level(IO3_GPIO) == 0;
        if (!ok) ESP_LOGW(TAG, "IO3 is high before sleeping, not sleeping");
    }
    if (!ok) { nrf_swd_unlock(); return false; }
    nrf_swd_disconnect();
    gpio_set_level(43, 0);   // keep the nRF clock line low through sleep so its debug port stays off
    gpio_hold_en(43);
    return true;
}

void a11_keys_deep_sleep(void) {
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_ext1_wakeup_io(1ULL << IO3_GPIO, ESP_EXT1_WAKEUP_ANY_HIGH);
    rtc_gpio_pulldown_en(IO3_GPIO);
    rtc_gpio_pullup_dis(IO3_GPIO);
    esp_deep_sleep_start();
}

static void connect_forever(int *n, uint8_t *pads) {
    for (;;) {
        uint32_t id;
        if (nrf_swd_connect(&id) && load_header(n, pads)) {
            ESP_LOGI(TAG, "nRF connected, idcode 0x%08lX, %d pads", (unsigned long)id, *n);
            return;
        }
        ESP_LOGW(TAG, "no scanner on the nRF, retrying");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---- key state shared by both ways of reading the nRF ----
static bool key_state[A11_KEY_COUNT];
static uint8_t key_count[A11_KEY_COUNT];
static int64_t down_at[A11_KEY_COUNT], rep_at[A11_KEY_COUNT];
static int64_t last_loop;

// A change must be seen on `confirm` reads in a row before it counts (the SWD reader needs two, the nRF already debounced for I2C).
static void apply_keys(const bool *now, int confirm) {
    for (int k = 0; k < A11_KEY_COUNT; k++) {
        if (now[k] == key_state[k]) { key_count[k] = 0; continue; }
        if (++key_count[k] >= confirm) {
            key_state[k] = now[k];
            key_count[k] = 0;
            a11_event_t ev = {A11_KEYMAP[k].code, key_state[k]};
            xQueueSend(events_q, &ev, 0);
            ev_count++;
            down_at[k] = esp_timer_get_time();
            rep_at[k] = 0;
        }
    }
}

bool a11_keys_down(uint16_t code) {
    for (int k = 0; k < A11_KEY_COUNT; k++) if (A11_KEYMAP[k].code == code && key_state[k]) return true;
    return false;
}

static bool any_key_down(void) {
    for (int k = 0; k < A11_KEY_COUNT; k++) if (key_state[k]) return true;
    return false;
}

// Holding a key repeats it, after 450 ms and then every 45 ms. Modifiers, Caps Lock and the On, Off and backlight keys do not.
static void do_repeats(void) {
    int64_t t = esp_timer_get_time();
    // If the device was too busy to look at the keys for a moment, a key that was let go may not have been noticed
    // yet. Start the repeat wait again instead of repeating a key nobody is holding.
    if (t - last_loop > 120000) for (int k = 0; k < A11_KEY_COUNT; k++) { down_at[k] = t; rep_at[k] = 0; }
    last_loop = t;
    for (int k = 0; k < A11_KEY_COUNT; k++) {
        uint16_t code = A11_KEYMAP[k].code;
        if (!key_state[k] || code >= 0x100 || (code >= 224 && code <= 231) || code == 57 || code == 41) continue;   // no repeat for Esc
        if (t - down_at[k] > 450000 && t - rep_at[k] > 45000) {
            rep_at[k] = t;
            a11_event_t ev = {code, true};
            xQueueSend(events_q, &ev, 0);
        }
    }
}

// ---- I2C link: the nRF raises IO3 when the held keys change and the ESP reads the list ----
#define NRF_I2C_ADDR 0x2A
#define REPORT_BYTES 32
static i2c_master_dev_handle_t nrf_dev;
static SemaphoreHandle_t io3_sem;
static uint8_t last_seq;
static volatile int64_t stat_t0, stat_t;
static volatile uint32_t stat_awake0, stat_awake, stat_wakes;

bool a11_keys_nrf_sleep(int *asleep_tenths_pct, int *wakes, int *seconds) {
    if (!stat_t0 || stat_t - stat_t0 < 1000000) return false;
    // awake ticks are 32 microseconds, subtracting as unsigned handles the counter wrapping
    int64_t awake_us = (int64_t)(uint32_t)(stat_awake - stat_awake0) * 32;
    int64_t span = stat_t - stat_t0;
    int pct = (int)(1000 - awake_us * 1000 / span);
    *asleep_tenths_pct = pct < 0 ? 0 : pct > 1000 ? 1000 : pct;
    *wakes = (int)stat_wakes;
    *seconds = (int)(span / 1000000);
    return true;
}

static volatile uint32_t io3_edges, read_fail, read_ok;
uint32_t a11_keys_io3_edges(void) { return io3_edges; }
uint32_t a11_keys_read_fail(void) { return read_fail; }
uint32_t a11_keys_read_ok(void) { return read_ok; }
int a11_keys_io3_level(void) { return gpio_get_level(IO3_GPIO); }

static void IRAM_ATTR io3_isr(void *arg) {
    io3_edges++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(io3_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// Reads the list of held pad pairs. Returns false on a bus error or a reply that is not ours.
static bool i2c_read_keys(bool *now) {
    uint8_t r[REPORT_BYTES];
    if (i2c_master_receive(nrf_dev, r, sizeof r, 50) != ESP_OK || r[0] != 0xA5 || r[2] > 10) return false;
    last_seq = r[1];
    uint32_t awake = r[24] | (r[25] << 8) | (r[26] << 16) | ((uint32_t)r[27] << 24);
    int64_t tnow = esp_timer_get_time();
    if (!stat_t0) { stat_t0 = tnow; stat_awake0 = awake; }
    stat_t = tnow; stat_awake = awake; stat_wakes = r[28] | (r[29] << 8);
    memset(now, 0, A11_KEY_COUNT);
    for (int i = 0; i < r[2]; i++) {
        uint8_t a = r[3 + 2 * i], b = r[4 + 2 * i];
        for (int k = 0; k < A11_KEY_COUNT; k++)
            if (A11_KEYMAP[k].pad_a == a && A11_KEYMAP[k].pad_b == b) { now[k] = true; break; }
    }
    return true;
}

static bool i2c_link_start(void) {
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) return false;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = NRF_I2C_ADDR, .scl_speed_hz = 100000};
    if (i2c_master_bus_add_device(bus, &dc, &nrf_dev) != ESP_OK) return false;
    bool now[A11_KEY_COUNT];
    for (int tries = 0; tries < 3; tries++) {
        if (i2c_read_keys(now)) goto ok;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
ok:
    io3_sem = xSemaphoreCreateBinary();
    gpio_config_t c = {.pin_bit_mask = 1ULL << IO3_GPIO, .mode = GPIO_MODE_INPUT, .pull_down_en = 1, .intr_type = GPIO_INTR_ANYEDGE};
    gpio_config(&c);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(IO3_GPIO, io3_isr, NULL);
    return true;
}

static void keys_task_i2c(void) {
    bool now[A11_KEY_COUNT];
    int fails = 0;
    int64_t last_read = 0;
    for (;;) {
        // Wake on IO3, or every 10 ms for key repeat while a key is held, or now and then in case an edge was missed.
        TickType_t wait = any_key_down() ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(slow_mode ? 2000 : 500);
        bool edge = xSemaphoreTake(io3_sem, wait) == pdTRUE;
        int64_t t = esp_timer_get_time();
        // Read when the nRF signalled a change, or as a rare safety check. Every read wakes the nRF for about 40 ms, so idle reads are kept far apart.
        if (edge || gpio_get_level(IO3_GPIO) || t - last_read > (slow_mode ? 60000000LL : 20000000LL)) {
            last_read = t;
            if (i2c_read_keys(now)) { fails = 0; apply_keys(now, 1); poll_count++; read_ok++; }
            else if (read_fail++, ++fails >= 5) { ESP_LOGW(TAG, "I2C link to the nRF failing, back to the debug link"); return; }
        }
        do_repeats();
    }
}

static void keys_task_swd(void) {
    int n = 0;
    uint8_t pads[MAX_PADS];
    uint32_t rows[MAX_PADS];
    int fails = 0;
    last_loop = esp_timer_get_time();
    connect_forever(&n, pads);
    for (;;) {
        if (!nrf_swd_read_words(MB_BASE + 36 * 4, rows, n)) {
            if (++fails > 3) { connect_forever(&n, pads); fails = 0; }
            vTaskDelay(1);
            continue;
        }
        fails = 0;
        poll_count++;
        bool now[A11_KEY_COUNT] = {0};
        rows_to_keys(n, pads, rows, now);
        apply_keys(now, 2);
        do_repeats();
        vTaskDelay(slow_poll ? pdMS_TO_TICKS(40) : 1);
    }
}

static void keys_task(void *arg) {
    last_loop = esp_timer_get_time();
    if (i2c_link_start()) {
        ESP_LOGI(TAG, "keys over I2C, the debug link is off");
        keys_task_i2c();
        // The debug link takes over. Release any key the I2C reader thought was down.
    } else {
        ESP_LOGW(TAG, "nRF does not answer on I2C, using the debug link");
    }
    keys_task_swd();
}

void a11_keys_start(QueueHandle_t events) {
    events_q = events;
    nrf_swd_init();
    read_wake_latch();
    xTaskCreate(keys_task, "a11_keys", 4096, NULL, 5, NULL);
}

const char *a11_key_name(uint16_t code) {
    for (int k = 0; k < A11_KEY_COUNT; k++)
        if (A11_KEYMAP[k].code == code) return A11_KEYMAP[k].name;
    return "?";
}

char a11_hid_to_char(uint16_t code, bool shift, bool caps) {
    static const char plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\ ;'`,./";
    static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}| :\"~<>?";
    if (code < 4 || code > 56 || code == 50) return 0;
    int i = code - 4;
    bool letter = code <= 29;
    bool up = letter ? (shift != caps) : shift;
    return up ? shifted[i] : plain[i];
}
