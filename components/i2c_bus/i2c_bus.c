#include "i2c_bus.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

i2c_master_bus_handle_t i2c_bus_get(void) {
    static i2c_master_bus_handle_t bus;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    static SemaphoreHandle_t lock;
    taskENTER_CRITICAL(&mux);
    if (!lock) lock = xSemaphoreCreateMutex();
    taskEXIT_CRITICAL(&mux);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!bus) {
        i2c_master_bus_config_t bc = {.i2c_port = I2C_NUM_0, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
                                      .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
        if (i2c_new_master_bus(&bc, &bus) != ESP_OK) bus = NULL;
    }
    xSemaphoreGive(lock);
    return bus;
}
