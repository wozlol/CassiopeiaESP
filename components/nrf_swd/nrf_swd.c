// Written from the ARM ADIv5 SWD protocol and the nRF52832 product specification, see nrf-tools/nrf_tool/swd.h.
#include "nrf_swd.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

#define SWCLK_PIN 43
#define SWDIO_PIN 44
#define HALF_US 1

#define ACK_OK 1
#define ACK_WAIT 2
#define DP_ABORT 0x00
#define DP_IDCODE 0x00
#define DP_CTRLSTAT 0x04
#define DP_SELECT 0x08
#define DP_RDBUFF 0x0C
#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0C
#define CSW_AUTOINC 0xA2000012
#define CSW_SINGLE 0xA2000002

static void swdio_out(void) { gpio_set_direction(SWDIO_PIN, GPIO_MODE_OUTPUT); }
static void swdio_in(void) { gpio_set_direction(SWDIO_PIN, GPIO_MODE_INPUT); }

static void clock_cycle(void) {
    gpio_set_level(SWCLK_PIN, 0);
    esp_rom_delay_us(HALF_US);
    gpio_set_level(SWCLK_PIN, 1);
    esp_rom_delay_us(HALF_US);
}

static void bit_out(int b) {
    gpio_set_level(SWDIO_PIN, b);
    clock_cycle();
}

static int bit_in(void) {
    gpio_set_level(SWCLK_PIN, 0);
    esp_rom_delay_us(HALF_US);
    int b = gpio_get_level(SWDIO_PIN);
    gpio_set_level(SWCLK_PIN, 1);
    esp_rom_delay_us(HALF_US);
    return b;
}

static int parity32(uint32_t v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1;
}

static uint8_t transfer(bool ap, bool read, uint8_t addr, uint32_t *data) {
    int a2 = (addr >> 2) & 1, a3 = (addr >> 3) & 1;
    int par = ap ^ read ^ a2 ^ a3;
    uint8_t req = 1 | (ap << 1) | (read << 2) | (a2 << 3) | (a3 << 4) | (par << 5) | (1 << 7);
    swdio_out();
    for (int i = 0; i < 8; i++) bit_out((req >> i) & 1);
    swdio_in();
    clock_cycle();
    uint8_t ack = 0;
    for (int i = 0; i < 3; i++) ack |= (uint8_t)bit_in() << i;
    if (ack == ACK_OK) {
        if (read) {
            uint32_t v = 0;
            for (int i = 0; i < 32; i++) v |= (uint32_t)bit_in() << i;
            int p = bit_in();
            clock_cycle();
            swdio_out();
            *data = v;
            if (p != parity32(v)) ack = 8;
        } else {
            clock_cycle();
            swdio_out();
            uint32_t v = *data;
            for (int i = 0; i < 32; i++) bit_out((v >> i) & 1);
            bit_out(parity32(v));
        }
    } else {
        clock_cycle();
        swdio_out();
    }
    gpio_set_level(SWDIO_PIN, 0);
    clock_cycle();
    clock_cycle();
    return ack;
}

static bool xfer(bool ap, bool read, uint8_t addr, uint32_t *data) {
    for (int i = 0; i < 50; i++) {
        uint8_t ack = transfer(ap, read, addr, data);
        if (ack == ACK_OK) return true;
        if (ack != ACK_WAIT) return false;
    }
    return false;
}

static bool dp_write(uint8_t addr, uint32_t v) { return xfer(false, false, addr, &v); }
static bool dp_read(uint8_t addr, uint32_t *v) { return xfer(false, true, addr, v); }
static bool ap_write(uint8_t addr, uint32_t v) { return xfer(true, false, addr, &v); }

void nrf_swd_init(void) {
    gpio_deep_sleep_hold_dis();   // lines were held low through deep sleep
    gpio_hold_dis(SWCLK_PIN);
    gpio_hold_dis(SWDIO_PIN);
    gpio_reset_pin(SWCLK_PIN);
    gpio_reset_pin(SWDIO_PIN);
    gpio_set_direction(SWCLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SWCLK_PIN, 1);
    gpio_set_pull_mode(SWDIO_PIN, GPIO_PULLUP_ONLY);
    swdio_in();
}

static void line_reset(void) {
    swdio_out();
    gpio_set_level(SWDIO_PIN, 1);
    for (int i = 0; i < 56; i++) clock_cycle();
    for (int i = 0; i < 16; i++) bit_out((0xE79E >> i) & 1);
    for (int i = 0; i < 56; i++) { gpio_set_level(SWDIO_PIN, 1); clock_cycle(); }
    for (int i = 0; i < 8; i++) bit_out(0);
}

static bool connect_unlocked(uint32_t *idcode) {
    line_reset();
    if (!dp_read(DP_IDCODE, idcode)) return false;
    dp_write(DP_ABORT, 0x1E);
    dp_write(DP_SELECT, 0);
    dp_write(DP_CTRLSTAT, 0x50000000);
    for (int i = 0; i < 50; i++) {
        uint32_t st;
        if (dp_read(DP_CTRLSTAT, &st) && (st & 0xA0000000) == 0xA0000000) return true;
        vTaskDelay(1);
    }
    return false;
}

static bool read_words_unlocked(uint32_t addr, uint32_t *buf, int words) {
    uint32_t dummy;
    if (words < 1 || words > 256) return false;
    if (!ap_write(AP_CSW, CSW_AUTOINC) || !ap_write(AP_TAR, addr)) return false;
    if (!xfer(true, true, AP_DRW, &dummy)) return false;
    for (int i = 0; i < words - 1; i++) {
        if (!xfer(true, true, AP_DRW, &buf[i])) return false;
    }
    if (!xfer(false, true, DP_RDBUFF, &buf[words - 1])) return false;
    uint32_t st = 0;
    if (!dp_read(DP_CTRLSTAT, &st)) return false;
    if (st & 0xB2) { dp_write(DP_ABORT, 0x1E); return false; }
    return true;
}

static SemaphoreHandle_t lk;
void nrf_swd_lock(void) {
    if (!lk) lk = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(lk, portMAX_DELAY);
}
void nrf_swd_unlock(void) { xSemaphoreGiveRecursive(lk); }

bool nrf_swd_connect(uint32_t *idcode) {
    nrf_swd_lock();
    bool ok = connect_unlocked(idcode);
    nrf_swd_unlock();
    return ok;
}

bool nrf_swd_read_words(uint32_t addr, uint32_t *buf, int words) {
    nrf_swd_lock();
    bool ok = read_words_unlocked(addr, buf, words);
    nrf_swd_unlock();
    return ok;
}

bool nrf_swd_read_word(uint32_t addr, uint32_t *v) { return nrf_swd_read_words(addr, v, 1); }

static bool write_words_unlocked(uint32_t addr, const uint32_t *buf, int words) {
    if (!ap_write(AP_CSW, CSW_AUTOINC) || !ap_write(AP_TAR, addr)) return false;
    for (int i = 0; i < words; i++)
        if (!ap_write(AP_DRW, buf[i])) return false;
    uint32_t st = 0;
    if (!dp_read(DP_CTRLSTAT, &st)) return false;
    if (st & 0xB2) { dp_write(DP_ABORT, 0x1E); return false; }
    return true;
}

bool nrf_swd_write_word(uint32_t addr, uint32_t v) {
    nrf_swd_lock();
    bool ok = ap_write(AP_CSW, CSW_SINGLE) && ap_write(AP_TAR, addr) && ap_write(AP_DRW, v);
    nrf_swd_unlock();
    return ok;
}

void nrf_swd_disconnect(void) {
    nrf_swd_lock();
    dp_write(DP_CTRLSTAT, 0);
    swdio_in();
    nrf_swd_unlock();
}

#define NVMC_READY 0x4001E400UL
#define NVMC_CONFIG 0x4001E504UL
#define NVMC_ERASEPAGE 0x4001E508UL
#define DHCSR 0xE000EDF0UL
#define DEMCR 0xE000EDFCUL
#define AIRCR 0xE000ED0CUL
#define PAGE_SZ 4096UL
#define PROTECT_FROM 0x7A000UL

static bool nvmc_ready(void) {
    for (int i = 0; i < 400; i++) {
        uint32_t r = 0;
        if (read_words_unlocked(NVMC_READY, &r, 1) && (r & 1)) return true;
        vTaskDelay(1);
    }
    return false;
}
static bool wr1(uint32_t a, uint32_t v) { return ap_write(AP_CSW, CSW_SINGLE) && ap_write(AP_TAR, a) && ap_write(AP_DRW, v); }

const char *nrf_swd_flash(uint32_t addr, const uint8_t *img, uint32_t len) {
    if (addr % PAGE_SZ || len == 0 || addr + len > PROTECT_FROM) return "image would touch the bootloader area or is not page aligned, refused";
    nrf_swd_lock();
    const char *err = NULL;
    uint32_t id = 0, d = 0;
    static uint32_t buf[256];
    if (!connect_unlocked(&id)) { err = "no answer from the nRF"; goto out; }
    // Stop the core before it runs any code so flash write protection set by old code is cleared.
    if (!wr1(DEMCR, 0x01000001UL) || !wr1(DHCSR, 0xA05F0003UL)) { err = "could not halt the nRF"; goto out; }
    wr1(AIRCR, 0x05FA0004UL);
    vTaskDelay(pdMS_TO_TICKS(30));
    bool halted = false;
    for (int t = 0; t < 3 && !halted; t++) {
        if (connect_unlocked(&id) && read_words_unlocked(DHCSR, &d, 1) && (d & (1UL << 17))) halted = true;
        else vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!halted) { err = "could not halt the nRF core"; goto out; }
    for (uint32_t p = addr / PAGE_SZ; p <= (addr + len - 1) / PAGE_SZ; p++) {
        bool ok = wr1(NVMC_CONFIG, 2) && wr1(NVMC_ERASEPAGE, p * PAGE_SZ) && nvmc_ready();
        wr1(NVMC_CONFIG, 0);
        if (!ok) { err = "erase failed"; goto out; }
    }
    if (!wr1(NVMC_CONFIG, 1)) { err = "could not enable flash write"; goto out; }
    uint32_t words = (len + 3) / 4;
    bool wok = true;
    for (uint32_t w = 0; w < words && wok; w += 256) {
        uint32_t n = words - w < 256 ? words - w : 256;
        memset(buf, 0xFF, sizeof buf);
        memcpy(buf, img + w * 4, (w + n) * 4 <= len ? n * 4 : len - w * 4);
        wok = write_words_unlocked(addr + w * 4, buf, n) && nvmc_ready();
    }
    wr1(NVMC_CONFIG, 0);
    if (!wok) { err = "write failed"; goto out; }
    for (uint32_t w = 0; w < words; w += 256) {
        uint32_t n = words - w < 256 ? words - w : 256;
        if (!read_words_unlocked(addr + w * 4, buf, n)) { err = "verify read failed, nRF left halted"; goto out; }
        uint32_t chunk[256] = {0};
        memset(chunk, 0xFF, sizeof chunk);
        memcpy(chunk, img + w * 4, (w + n) * 4 <= len ? n * 4 : len - w * 4);
        if (memcmp(chunk, buf, n * 4)) { err = "verify failed, nRF left halted"; goto out; }
    }
    wr1(DEMCR, 0);
    wr1(DHCSR, 0xA05F0000UL);
    wr1(AIRCR, 0x05FA0004UL);
out:
    nrf_swd_unlock();
    return err;
}
