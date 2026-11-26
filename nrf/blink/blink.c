// Bare metal blink for the SparkFun nRF52832 breakout. Blue LED on P0.07, active low.
// No SoftDevice, no bootloader. Lives at flash address 0 and replaces the MBR page.
#include <stdint.h>

#define P0_OUTSET (*(volatile uint32_t *)0x50000508UL)
#define P0_OUTCLR (*(volatile uint32_t *)0x5000050CUL)
#define P0_DIRSET (*(volatile uint32_t *)0x50000518UL)
#define LED (1UL << 7)

extern uint32_t _estack;
void Reset_Handler(void);
void Default_Handler(void) { for (;;) {} }

__attribute__((section(".vectors"), used))
void *const vectors[16] = {
    &_estack, Reset_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, 0, 0, 0, 0, Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler,
};

static void delay(void) {
    for (volatile uint32_t i = 0; i < 1500000; i++) {}
}

void Reset_Handler(void) {
    P0_DIRSET = LED;
    for (;;) {
        P0_OUTCLR = LED;
        delay();
        P0_OUTSET = LED;
        delay();
    }
}
