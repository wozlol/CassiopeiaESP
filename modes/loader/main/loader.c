// CassiopeiaESP mode loader. Lives in the factory slot. Copies mode images from /modes on the SD card into
// the ota_0 or ota_1 slot, checks them first, and boots a slot. Serial commands: list, install <file> <slot>, boot <slot>.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/usb_serial_jtag.h"
#include "a11_keys.h"
#include "nrf_swd.h"
#include "driver/gpio.h"
#include "display.h"
#include "settings.h"
#include "status_led.h"
#include "themes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MODES_DIR "/sdcard/modes"
#define CHIP_ID_ESP32S3 9
static sdmmc_card_t *card;
static char last_msg[400];
#define REPORT(...) do { snprintf(last_msg, sizeof last_msg, __VA_ARGS__); printf("%s", last_msg); } while (0)
static SemaphoreHandle_t op_lock;
static bool sd_ok;

static void sd_mount(bool format) {
    if (sd_ok) { esp_vfs_fat_sdcard_unmount("/sdcard", card); sd_ok = false; }
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = format, .max_files = 4, .allocation_unit_size = 16 * 1024};
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 38; slot.cmd = 40; slot.d0 = 39; slot.d1 = 41; slot.d2 = 48; slot.d3 = 47;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t e = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card);
    sd_ok = (e == ESP_OK);
    if (sd_ok) printf("SD mounted, %llu MB\n", (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    else printf("SD mount failed (%s). Needs a FAT32 card.\n", esp_err_to_name(e));
}

static const esp_partition_t *slot_by_name(const char *n) {
    if (!strcmp(n, "ota_0")) return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!strcmp(n, "ota_1")) return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!strcmp(n, "ota_2")) return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_2, NULL);
    if (!strcmp(n, "ota_3")) return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_3, NULL);
    return NULL;
}

static void cmd_list(void) {
    printf("Slots:\n");
    const char *names[] = {"ota_0", "ota_1", "ota_2", "ota_3"};
    for (int i = 0; i < 4; i++) {
        const esp_partition_t *p = slot_by_name(names[i]);
        esp_app_desc_t d;
        if (p && esp_ota_get_partition_description(p, &d) == ESP_OK)
            printf("  %s  %s %s  (%lu KB slot)\n", names[i], d.project_name, d.version, (unsigned long)(p->size / 1024));
        else
            printf("  %s  empty\n", names[i]);
    }
    printf("SD images in /modes:\n");
    DIR *dir = sd_ok ? opendir(MODES_DIR) : NULL;
    if (!dir) { printf("  none (no SD or no /modes folder)\n"); return; }
    struct dirent *en;
    while ((en = readdir(dir))) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof path, MODES_DIR "/%s", en->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) printf("  %s  %lu KB\n", en->d_name, (unsigned long)(st.st_size / 1024));
    }
    closedir(dir);
}

// Checks done before anything is written. Returns NULL if the image looks like an ESP32-S3 app image.
static const char *check_header(const uint8_t *h, size_t size, size_t slot_size) {
    if (size < 256) return "file too small";
    if (size > slot_size) return "image bigger than the slot";
    if (h[0] != 0xE9) return "not an ESP image (bad magic)";
    if ((h[12] | (h[13] << 8)) != CHIP_ID_ESP32S3) return "image is for a different chip";
    uint32_t m = h[32] | (h[33] << 8) | (h[34] << 16) | ((uint32_t)h[35] << 24);
    if (m != 0xABCD5432) return "no app description in image";
    return NULL;
}

static void cmd_install(const char *file, const char *slotname) {
    const esp_partition_t *p = slot_by_name(slotname);
    if (!p) { REPORT("FAIL unknown slot %s (use ota_0 to ota_3)\n", slotname); return; }
    if (p == esp_ota_get_running_partition()) { REPORT("FAIL slot is running\n"); return; }
    if (!sd_ok) { REPORT("FAIL no SD card\n"); return; }
    char path[300];
    snprintf(path, sizeof path, MODES_DIR "/%s", file);
    struct stat st;
    if (stat(path, &st) != 0) { REPORT("FAIL %s not found\n", path); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { REPORT("FAIL cannot open %s\n", path); return; }
    uint8_t head[64];
    if (fread(head, 1, sizeof head, f) != sizeof head) { fclose(f); REPORT("FAIL short file\n"); return; }
    const char *why = check_header(head, st.st_size, p->size);
    if (why) { fclose(f); REPORT("FAIL %s\n", why); return; }
    rewind(f);
    esp_ota_handle_t h;
    if (esp_ota_begin(p, st.st_size, &h) != ESP_OK) { fclose(f); REPORT("FAIL cannot start write\n"); return; }
    static uint8_t buf[4096];
    size_t n;
    esp_err_t e = ESP_OK;
    while (e == ESP_OK && (n = fread(buf, 1, sizeof buf, f)) > 0) e = esp_ota_write(h, buf, n);
    fclose(f);
    if (e != ESP_OK) { esp_ota_abort(h); REPORT("FAIL write error %s, slot %s cannot be booted until a good image is installed, nothing else changed\n", esp_err_to_name(e), slotname); return; }
    e = esp_ota_end(h);
    if (e != ESP_OK) { REPORT("FAIL image check failed (%s), slot %s cannot be booted until a good image is installed, nothing else changed\n", esp_err_to_name(e), slotname); return; }
    esp_app_desc_t d;
    esp_ota_get_partition_description(p, &d);
    REPORT("OK installed %s %s into %s, not booted yet\n", d.project_name, d.version, slotname);
}

static void cmd_boot(const char *slotname) {
    const esp_partition_t *p = slot_by_name(slotname);
    esp_app_desc_t d;
    if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) { REPORT("FAIL %s is empty or unknown\n", slotname); return; }
    if (esp_ota_set_boot_partition(p) != ESP_OK) { REPORT("FAIL cannot set boot slot\n"); return; }
    REPORT("OK booting %s %s\n", d.project_name, d.version);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// Receives a file over USB serial into /modes. Host sends 4096 byte chunks and waits for an A line after each one.
// Safe relative path under /sdcard: modes/..., library/..., media/..., or a plain name which goes to modes. Makes the folders.
static bool put_path(const char *name, char *path, size_t cap, char *tmp, size_t tcap) {
    if (!name[0] || strlen(name) > 120 || name[0] == '.' || name[0] == '/' || strstr(name, "..") || strstr(name, "//")) return false;
    for (const char *c = name; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '-' || *c == '/' || *c == '+')) return false;
    const char *rel = name;
    char full[200];
    if (!strchr(name, '/')) snprintf(full, sizeof full, "modes/%s", name);
    else if (!strncmp(name, "modes/", 6) || !strncmp(name, "library/", 8) || !strncmp(name, "media/", 6)) snprintf(full, sizeof full, "%s", name);
    else return false;
    rel = full;
    snprintf(path, cap, "/sdcard/%s", rel);
    for (char *q = path + 8; *q; q++) if (*q == '/') { *q = 0; mkdir(path, 0777); *q = '/'; }
    char dir[300];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    snprintf(tmp, tcap, "%s/put.part", dir);
    return true;
}

static void cmd_put(const char *name, const char *sizestr) {
    long size = atol(sizestr);
    if (!sd_ok) { printf("FAIL no SD card\n"); return; }
    char path[300], tmp[320];
    if (!put_path(name, path, sizeof path, tmp, sizeof tmp)) { printf("FAIL bad file name\n"); return; }
    if (size <= 0 || size > 1024L * 1024 * 1024) { printf("FAIL size must be 1 byte to 1 GB\n"); return; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { printf("FAIL cannot create file on the SD card\n"); return; }
    printf("SEND\n");
    static uint8_t buf[4096];
    long left = size;
    while (left > 0) {
        int want = left > (long)sizeof buf ? (int)sizeof buf : (int)left, got = 0;
        TickType_t t0 = xTaskGetTickCount();
        while (got < want && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(3000)) {
            int r = usb_serial_jtag_read_bytes(buf + got, want - got, pdMS_TO_TICKS(100));
            if (r > 0) { got += r; t0 = xTaskGetTickCount(); }
        }
        if (got < want) { fclose(f); remove(tmp); printf("FAIL timed out, nothing kept\n"); return; }
        if (fwrite(buf, 1, want, f) != (size_t)want) { fclose(f); remove(tmp); printf("FAIL SD write error, nothing kept\n"); return; }
        left -= want;
        printf("A\n");
    }
    fclose(f);
    remove(path);
    if (rename(tmp, path) != 0) { printf("FAIL rename failed\n"); return; }
    printf("OK saved %s, %ld bytes\n", name, size);
}


// Writes an image to the nRF over SWD. The host sends the bytes like put does. Gated by the typed CONFIRM token, see the safety ledger.
static void cmd_nrf_flash(const char *sizestr) {
    long size = atol(sizestr);
    if (size <= 0 || size > 60 * 1024) { printf("FAIL size must be 1 byte to 60 KB\n"); return; }
    uint8_t *img = malloc(size);
    if (!img) { printf("FAIL no memory\n"); return; }
    printf("SEND\n");
    long got = 0;
    while (got < size) {
        int want = size - got > 4096 ? 4096 : (int)(size - got), have = 0;
        TickType_t t0 = xTaskGetTickCount();
        while (have < want && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(3000)) {
            int r = usb_serial_jtag_read_bytes(img + got + have, want - have, pdMS_TO_TICKS(100));
            if (r > 0) { have += r; t0 = xTaskGetTickCount(); }
        }
        if (have < want) { free(img); printf("FAIL timed out, nothing written\n"); return; }
        got += want;
        printf("A\n");
    }
    const char *err = nrf_swd_flash(0, img, (uint32_t)size);
    free(img);
    if (err) printf("FAIL %s\n", err); else printf("OK nRF flashed, verified and running, %ld bytes\n", size);
}

// Checks the three wires between the ESP and the nRF that the SWD link does not use. The nRF drives each pad through SWD while the ESP reads its own pin.
static void esp_in(int pin, int pull) {
    gpio_config_t c = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = pull > 0, .pull_down_en = pull < 0};
    gpio_config(&c);
}

static bool wire_line(const char *name, int esp_pin, int nrf_pad) {
    const uint32_t P0 = 0x50000000UL;
    nrf_swd_lock();
    nrf_swd_write_word(P0 + 0x700 + 4 * nrf_pad, 0x00000001);   // output, input buffer on so the nRF can see its own pin
    int hi[3], lo[3];
    for (int m = 0; m < 3; m++) {   // ESP pull: down, none, up
        esp_in(esp_pin, m - 1 == 0 ? 0 : (m == 0 ? -1 : 1));
        nrf_swd_write_word(P0 + 0x508, 1UL << nrf_pad);
        vTaskDelay(pdMS_TO_TICKS(30));
        hi[m] = gpio_get_level(esp_pin);
        nrf_swd_write_word(P0 + 0x50C, 1UL << nrf_pad);
        vTaskDelay(pdMS_TO_TICKS(30));
        lo[m] = gpio_get_level(esp_pin);
    }
    esp_in(esp_pin, 0);
    for (int step = 0; step < 6; step++) {
        bool want = step & 1;
        nrf_swd_write_word(P0 + (want ? 0x508 : 0x50C), 1UL << nrf_pad);
        vTaskDelay(pdMS_TO_TICKS(30));
        uint32_t nin = 0;
        nrf_swd_read_word(P0 + 0x510, &nin);
        printf("  step %d nRF drives %d, nRF reads its pin %d, ESP reads %d\n", step, want, (int)((nin >> nrf_pad) & 1), gpio_get_level(esp_pin));
    }
    nrf_swd_write_word(P0 + 0x700 + 4 * nrf_pad, 0x00000002);
    gpio_reset_pin(esp_pin);
    nrf_swd_unlock();
    bool ok = hi[1] == 1 && lo[1] == 0;
    printf("WIRE %s ESP GPIO%d to nRF pad %d: nRF high reads (pull down, none, up) %d %d %d, nRF low reads %d %d %d -> %s\n", name, esp_pin, nrf_pad, hi[0], hi[1], hi[2], lo[0], lo[1], lo[2], ok ? "connected" : "NOT connected");
    return ok;
}

static void cmd_nrf_status(void) {
    uint32_t a[4], b[4], w1 = 0, w2 = 0;
    if (!nrf_swd_read_words(0x20000000UL, a, 4)) { printf("FAIL no answer from the nRF\n"); return; }
    nrf_swd_read_word(0x20000000UL + 68 * 4, &w1);
    vTaskDelay(pdMS_TO_TICKS(300));
    nrf_swd_read_words(0x20000000UL, b, 4);
    nrf_swd_read_word(0x20000000UL + 68 * 4, &w2);
    printf("NRF magic %08lX, scans %lu then %lu, state %lu (1 scanning, 2 asleep), wakes %lu then %lu -> %s\n", (unsigned long)a[0], (unsigned long)a[1], (unsigned long)b[1], (unsigned long)b[3], (unsigned long)w1, (unsigned long)w2, a[1] == b[1] ? "not scanning, asleep" : "scanning");
    uint32_t dbg[16] = {0}, pin_in = 0;
    nrf_swd_read_words(0x20000000UL + 158 * 4, dbg, 16);   // dbg[i] is mailbox word 158 + i
    nrf_swd_read_word(0x50000510UL, &pin_in);
    printf("NRFI2C changes %lu, last sent %lu, IO3 raised %lu, reads answered %lu, IO3 lowered %lu, IO3 pin now %lu\n", (unsigned long)dbg[1], (unsigned long)dbg[2], (unsigned long)dbg[12], (unsigned long)dbg[13], (unsigned long)dbg[14], (unsigned long)((pin_in >> 25) & 1));
    printf("OK\n");
}

// Pulls one sensed keyboard pad low from inside the nRF, as a key press would, and checks the sleeping nRF wakes and sleeps again.
static void cmd_nrf_waketest(void) {
    uint32_t w1 = 0, w2 = 0, s1 = 0, s2 = 0, st = 0;
    nrf_swd_lock();
    nrf_swd_read_word(0x20000000UL + 68 * 4, &w1);
    nrf_swd_read_word(0x20000000UL + 4, &s1);
    nrf_swd_write_word(0x50000000UL + 0x700 + 4 * 13, 0x00030001UL);
    vTaskDelay(pdMS_TO_TICKS(100));
    nrf_swd_read_word(0x20000000UL + 68 * 4, &w2);
    nrf_swd_read_word(0x20000000UL + 4, &s2);
    vTaskDelay(pdMS_TO_TICKS(200));
    nrf_swd_read_word(0x20000000UL + 12, &st);
    nrf_swd_unlock();
    printf("WAKETEST wakes %lu -> %lu, scans %lu -> %lu, state afterwards %lu\n", (unsigned long)w1, (unsigned long)w2, (unsigned long)s1, (unsigned long)s2, (unsigned long)st);
    printf(w2 == w1 + 1 ? "OK woke on a pulled low pad\n" : "FAIL did not wake\n");
}

// Drives one ESP pin and reports which spare nRF pads follow it. Keyboard pads are left alone.
static void cmd_nrf_find(const char *pinstr) {
    int esp_pin = atoi(pinstr);
    static const int spare[] = {0, 1, 6, 7, 25, 26, 27, 30};
    const uint32_t P0 = 0x50000000UL;
    uint32_t id, hi = 0, lo = 0;
    if (esp_pin < 1 || esp_pin > 21 || !nrf_swd_connect(&id)) { printf("FAIL bad pin or no nRF\n"); return; }
    nrf_swd_lock();
    for (unsigned i = 0; i < sizeof spare / sizeof spare[0]; i++) nrf_swd_write_word(P0 + 0x700 + 4 * spare[i], 0x00000000);   // input, buffer on, no pull
    gpio_reset_pin(esp_pin);
    gpio_set_direction(esp_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(esp_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    nrf_swd_read_word(P0 + 0x510, &hi);
    gpio_set_level(esp_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    nrf_swd_read_word(P0 + 0x510, &lo);
    gpio_reset_pin(esp_pin);
    for (unsigned i = 0; i < sizeof spare / sizeof spare[0]; i++) nrf_swd_write_word(P0 + 0x700 + 4 * spare[i], 0x00000002);
    nrf_swd_unlock();
    int n = 0;
    for (unsigned i = 0; i < sizeof spare / sizeof spare[0]; i++) {
        int b = spare[i];
        if (((hi >> b) & 1) && !((lo >> b) & 1)) { printf("FOUND ESP GPIO%d reaches nRF pad %d\n", esp_pin, b); n++; }
    }
    printf(n ? "OK\n" : "FAIL no spare nRF pad followed GPIO%d\n", esp_pin);
}

static void cmd_nrf_wire(void) {
    uint32_t id;
    if (!nrf_swd_connect(&id)) { printf("FAIL no answer from the nRF over SWD\n"); return; }
    bool a = wire_line("IO3 wake", 3, 25);
    bool b = wire_line("I2C SDA", 16, 26);
    bool c = wire_line("I2C SCL", 15, 27);
    printf(a && b && c ? "OK all three wires connected\n" : "FAIL a wire is missing or not where WIRING.md says\n");
}

static void cmd_rm(const char *name) {
    if (!sd_ok || name[0] == '.' || strchr(name, '/') || strstr(name, "..")) { printf("FAIL bad name or no SD\n"); return; }
    char path[300];
    snprintf(path, sizeof path, MODES_DIR "/%s", name);
    printf(remove(path) == 0 ? "OK removed %s\n" : "FAIL cannot remove %s\n", name);
}

// Removes one pack folder from /library with everything in it. Only plain folder names, nothing outside /library is touched.
static void rm_tree(const char *path, int depth) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[300];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (e->d_type == DT_DIR && depth < 4) rm_tree(sub, depth + 1);
        else remove(sub);
    }
    closedir(d);
    remove(path);
}
static void cmd_rmpack(const char *name) {
    if (!sd_ok || name[0] == '.' || strchr(name, '/') || strstr(name, "..")) { printf("FAIL bad name or no SD\n"); return; }
    char path[300];
    snprintf(path, sizeof path, "/sdcard/library/%s", name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) { printf("FAIL no pack %s\n", name); return; }
    rm_tree(path, 0);
    printf(stat(path, &st) != 0 ? "OK removed pack %s\n" : "FAIL could not remove all of %s\n", name);
}

static void handle(char *line) {
    char *a = strtok(line, " \r\n"), *b = strtok(NULL, " \r\n"), *c = strtok(NULL, " \r\n"), *d = strtok(NULL, " \r\n");
    if (!a) return;
    if (!strcmp(a, "list")) cmd_list();
    else if (!strcmp(a, "sd")) { sd_mount(false); cmd_list(); }
    else if (!strcmp(a, "format")) { if (b && !strcmp(b, "CONFIRM")) { printf("Formatting the SD card, this can take a minute\n"); sd_mount(true); cmd_list(); } else printf("FAIL type: format CONFIRM (erases the whole card)\n"); }
    else if (!strcmp(a, "put") && b && c) cmd_put(b, c);
    else if (!strcmp(a, "install") && b && c) cmd_install(b, c);
    else if (!strcmp(a, "rm") && b) cmd_rm(b);
    else if (!strcmp(a, "rmpack") && b) cmd_rmpack(b);
    else if (!strcmp(a, "nrf") && b && !strcmp(b, "wire")) cmd_nrf_wire();
    else if (!strcmp(a, "nrf") && b && !strcmp(b, "status")) cmd_nrf_status();
    else if (!strcmp(a, "nrf") && b && !strcmp(b, "find") && c) cmd_nrf_find(c);
    else if (!strcmp(a, "nrf") && b && !strcmp(b, "waketest")) cmd_nrf_waketest();
    else if (!strcmp(a, "nrf") && b && !strcmp(b, "flash") && c && d && !strcmp(d, "CONFIRM")) cmd_nrf_flash(c);
    else if (!strcmp(a, "nrf") && b && !strcmp(b, "flash")) printf("FAIL type: nrf flash <bytes> CONFIRM\n");
    else if (!strcmp(a, "boot") && b) cmd_boot(b);
    else printf("Commands: list, rm <file>, put, sd (remount), install <file> <ota_0..ota_3>, boot <ota_0..ota_3>\n");
    printf("READY\n");
}

// On screen loader. Up and Down select, Enter boots a flash slot or picks an SD image to install, 1 and 2 choose the slot.
#define UI_COLS 60
#define UI_ROWS 20
#define MAX_FILES 24
static terminal_cell_t screen[UI_COLS * UI_ROWS];
static char files[MAX_FILES][64];
static long file_kb[MAX_FILES];
static int nfiles, sel, pending = -1;
#define NSLOTS 4
static const char *SLOT_NAMES[NSLOTS] = {"ota_0", "ota_1", "ota_2", "ota_3"};

static void put(int r, int c, const char *s, bool rev) {
    for (; *s && c < UI_COLS; s++, c++) {
        terminal_cell_t *t = &screen[r * UI_COLS + c];
        t->cp = (unsigned char)*s;
        t->attrs = rev ? ATTR_REVERSE : 0;
    }
}

static void row_rev(int r) {
    for (int c = 0; c < UI_COLS; c++) screen[r * UI_COLS + c].attrs |= ATTR_REVERSE;
}

static void scan_files(void) {
    nfiles = 0;
    DIR *dir = sd_ok ? opendir(MODES_DIR) : NULL;
    if (!dir) return;
    struct dirent *en;
    while (nfiles < MAX_FILES && (en = readdir(dir))) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof path, MODES_DIR "/%s", en->d_name);
        size_t n = strlen(en->d_name);
        if (n < 64 && n > 4 && !strcmp(en->d_name + n - 4, ".bin") && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            strcpy(files[nfiles], en->d_name);
            file_kb[nfiles++] = st.st_size / 1024;
        }
    }
    closedir(dir);
}

// The loader wears the same theme as the other modes, from the same saved setting. T opens the list.
static bool theme_mode;
static int theme_sel, theme_saved;
static void draw_themes(void) {
    for (int i = 0; i < UI_COLS * UI_ROWS; i++) screen[i] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, 0};
    put(0, 0, " CassiopeiaESP loader  THEME", true); row_rev(0);
    int n = themes_count(), vis = UI_ROWS - 3;
    int top = theme_sel >= vis ? theme_sel - vis + 1 : 0;
    for (int j = 0; j < vis && top + j < n; j++) {
        char b[80];
        snprintf(b, sizeof b, "  %s%s%s", themes_get(top + j)->name, themes_is_custom(top + j) ? "  (saved)" : "", top + j == theme_saved ? "  *" : "");
        put(2 + j, 0, b, top + j == theme_sel);
        if (top + j == theme_sel) row_rev(2 + j);
    }
    put(UI_ROWS - 1, 0, " Up Down preview   Enter keeps it   Esc cancels", true);
    row_rev(UI_ROWS - 1);
}
static void draw(void) {
    if (theme_mode) { draw_themes(); return; }
    for (int i = 0; i < UI_COLS * UI_ROWS; i++) screen[i] = (terminal_cell_t){' ', DISPLAY_DEFAULT_FG, DISPLAY_DEFAULT_BG, 0};
    put(0, 0, " CassiopeiaESP loader", true); row_rev(0);
    put(2, 0, " START A MODE", false);
    put(2, 17, "press 1 to 4, or Enter", false);
    for (int i = 0; i < NSLOTS; i++) {
        const esp_partition_t *p = slot_by_name(SLOT_NAMES[i]);
        esp_app_desc_t d;
        char b[100];
        if (p && esp_ota_get_partition_description(p, &d) == ESP_OK) snprintf(b, sizeof b, "  [%d]  %s %s", i + 1, d.project_name, d.version);
        else snprintf(b, sizeof b, "  [%d]  (empty)", i + 1);
        put(3 + i, 0, b, sel == i);
        if (sel == i) row_rev(3 + i);
    }
    put(7, 0, " Starts at power on. GUI+Esc comes back here.", false);
    put(9, 0, sd_ok ? " INSTALL FROM THE SD CARD" : " NO SD CARD FOUND", false);
    if (sd_ok) put(9, 27, "Enter, then 1 to 4", false);
    for (int i = 0; i < nfiles && 10 + i < 18; i++) {
        char b[100];
        snprintf(b, sizeof b, "  %.40s  %ld KB", files[i], file_kb[i]);
        put(10 + i, 0, b, sel == NSLOTS + i);
        if (sel == NSLOTS + i) row_rev(10 + i);
    }
    char m[UI_COLS + 1];
    snprintf(m, sizeof m, " %.58s", last_msg);
    for (char *q = m; *q; q++) if (*q == '\n') *q = 0;
    put(18, 0, m, false);
    if (pending >= 0) {
        char h[160];
        snprintf(h, sizeof h, " Put %.26s in slot?  1 2 3 4   Esc stops", files[pending]);
        put(19, 0, h, true);
    } else if (sel < NSLOTS) put(19, 0, " Enter or 1-4 starts   Up Down move   r rescan   t theme", true);
    else put(19, 0, " Enter installs   Up Down move   r rescan", true);
    row_rev(19);
}

static void ui_task(void *arg) {
    int cols, rows;
    display_init();
    settings_init();
    theme_saved = settings_get("theme", 0);
    if (theme_saved < 0 || theme_saved >= themes_count()) theme_saved = 0;
    display_set_theme(themes_get(theme_saved));
    {   // the saved screen brightness, and the back light that follows it, like the other modes
        static const int LEVELS[] = {10, 25, 40, 60, 80, 100};
        int bl = settings_get("bl", 5);
        if (bl < 0 || bl > 5) bl = 5;
        display_set_backlight(LEVELS[bl]);
        status_led_start();
        status_led_set_brightness(LEVELS[bl]);
    }
    display_set_font(1, &cols, &rows);
    display_set_text_buffer(screen, UI_COLS, UI_ROWS);
    display_set_cursor(0, 0, CURSOR_NONE);
    QueueHandle_t q = xQueueCreate(16, sizeof(a11_event_t));
    a11_keys_start(q);
    scan_files();
    draw();
    for (;;) {
        a11_event_t ev;
        if (xQueueReceive(q, &ev, portMAX_DELAY) != pdTRUE || !ev.down) continue;
        if (ev.code == A11_KEY_BACKLIGHT) {   // steps the brightness down, and back to the top after the lowest, same as the other modes
            static const int LEVELS[] = {10, 25, 40, 60, 80, 100};
            int bl = settings_get("bl", 5);
            bl = (bl <= 0 || bl > 5) ? 5 : bl - 1;
            display_set_backlight(LEVELS[bl]);
            status_led_set_brightness(LEVELS[bl]);
            settings_set("bl", bl);
            snprintf(last_msg, sizeof last_msg, "Brightness %d%%", LEVELS[bl]);
            draw();
            continue;
        }
        if (ev.code == A11_KEY_OFF) {   // Off sleeps the whole device, On wakes it back to this loader
            status_led_off(true);   // the back light goes dark first, it keeps its last colour through sleep otherwise
            vTaskDelay(pdMS_TO_TICKS(120));
            display_sleep(true);
            if (a11_keys_prepare_sleep()) { display_power_off(); a11_keys_deep_sleep(); }
            status_led_off(false);
            display_sleep(false);
            snprintf(last_msg, sizeof last_msg, "Could not go to sleep, try again");
            draw();
            continue;
        }
        int items = NSLOTS + nfiles;
        if (theme_mode) {
            int n = themes_count();
            if (ev.code == 82) { theme_sel = theme_sel > 0 ? theme_sel - 1 : n - 1; display_set_theme(themes_get(theme_sel)); }
            else if (ev.code == 81) { theme_sel = theme_sel < n - 1 ? theme_sel + 1 : 0; display_set_theme(themes_get(theme_sel)); }
            else if (ev.code == 40) { theme_saved = theme_sel; settings_set("theme", theme_saved); theme_mode = false; }
            else if (ev.code == 41) { display_set_theme(themes_get(theme_saved)); theme_mode = false; }
            draw();
            continue;
        }
        if (ev.code == 23) { theme_mode = true; theme_sel = theme_saved; draw(); continue; }   // T
        xSemaphoreTake(op_lock, portMAX_DELAY);
        if (pending >= 0) {
            if (ev.code >= 30 && ev.code < 30 + NSLOTS) {
                snprintf(last_msg, sizeof last_msg, "Installing %s ...", files[pending]);
                draw();
                vTaskDelay(pdMS_TO_TICKS(50));
                cmd_install(files[pending], SLOT_NAMES[ev.code - 30]);
                pending = -1;
            } else if (ev.code == 41) pending = -1;
        } else if (ev.code >= 30 && ev.code < 30 + NSLOTS) {
            int slot = ev.code - 30;
            const esp_partition_t *p = slot_by_name(SLOT_NAMES[slot]);
            esp_app_desc_t d;
            if (p && esp_ota_get_partition_description(p, &d) == ESP_OK) { xSemaphoreGive(op_lock); cmd_boot(SLOT_NAMES[slot]); xSemaphoreTake(op_lock, portMAX_DELAY); }
            else snprintf(last_msg, sizeof last_msg, "Slot %d is empty, install a mode into it first", slot + 1);
        } else if (ev.code == 82) sel = sel > 0 ? sel - 1 : items - 1;   // wraps round
        else if (ev.code == 81) sel = sel < items - 1 ? sel + 1 : 0;
        else if (ev.code == 21) { scan_files(); snprintf(last_msg, sizeof last_msg, "SD rescanned"); }
        else if (ev.code == 40) {
            if (sel < NSLOTS) { xSemaphoreGive(op_lock); cmd_boot(SLOT_NAMES[sel]); xSemaphoreTake(op_lock, portMAX_DELAY); }
            else pending = sel - NSLOTS;
        }
        draw();
        xSemaphoreGive(op_lock);
    }
}

void app_main(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 8192;
    usb_serial_jtag_driver_install(&cfg);
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("\nLOADER running from %s\n", esp_ota_get_running_partition()->label);
    op_lock = xSemaphoreCreateMutex();
    sd_mount(false);
    xTaskCreate(ui_task, "loader_ui", 6144, NULL, 4, NULL);
    cmd_list();
    printf("READY\n");
    char line[200];
    int len = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) == 1) {
            if (ch == '\n' || ch == '\r') {
                line[len] = 0;
                if (len) { xSemaphoreTake(op_lock, portMAX_DELAY); handle(line); xSemaphoreGive(op_lock); }
                len = 0;
            } else if (len < (int)sizeof line - 1) line[len++] = ch;
        }
    }
}
