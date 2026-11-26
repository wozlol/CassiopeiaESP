#include "sdcard.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "sdcard";
static sdmmc_card_t *card;
static bool ok;

bool sdcard_ok(void) { return ok; }

bool sdcard_mount(void) {
    if (ok) return true;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024};
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_SD_CLK; slot.cmd = BOARD_SD_CMD; slot.d0 = BOARD_SD_D0; slot.d1 = BOARD_SD_D1; slot.d2 = BOARD_SD_D2; slot.d3 = BOARD_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t e = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT, &host, &slot, &mc, &card);
    ok = e == ESP_OK;
    if (ok) ESP_LOGI(TAG, "SD mounted, %llu MB", (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    else ESP_LOGW(TAG, "SD mount failed (%s), it needs a FAT32 card", esp_err_to_name(e));
    return ok;
}
