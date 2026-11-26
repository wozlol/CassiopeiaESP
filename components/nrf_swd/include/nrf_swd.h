// Bit banged SWD master for reading an nRF52832 while it runs. Port of firmware/nrf_tool/swd.h to ESP-IDF.
// ESP GPIO43 (UART TX pin) -> nRF C (SWDCLK), ESP GPIO44 (UART RX pin) -> nRF D (SWDIO).
#pragma once
#include <stdbool.h>
#include <stdint.h>

void nrf_swd_init(void);
// Line reset, reads the IDCODE and powers up the debug port. Call again after any read failure.
bool nrf_swd_connect(uint32_t *idcode);
// Reads 1 to 256 words from RAM or flash. The range must stay inside one 1 KB block.
bool nrf_swd_read_words(uint32_t addr, uint32_t *buf, int words);

// The SWD lines are shared by the key reader task and the tools, take this around any sequence that must not be interleaved.
void nrf_swd_lock(void);
void nrf_swd_unlock(void);
bool nrf_swd_read_word(uint32_t addr, uint32_t *v);
bool nrf_swd_write_word(uint32_t addr, uint32_t v);
// Powers the debug port down again so the nRF can sleep at its lowest current. Connect again before the next access.
void nrf_swd_disconnect(void);
// Halts the nRF, erases the pages the image covers, writes, verifies and runs it. Refuses anything touching the SoftDevice area (0x7A000 up) or not page aligned.
// Returns NULL on success or a short message.
const char *nrf_swd_flash(uint32_t addr, const uint8_t *img, uint32_t len);
