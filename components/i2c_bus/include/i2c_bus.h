// The one I2C bus on GPIO15 and GPIO16. The touch chip and the nRF keyboard link share it.
#pragma once
#include "driver/i2c_master.h"

// Creates the bus on first use. Returns NULL if it cannot be created.
i2c_master_bus_handle_t i2c_bus_get(void);
