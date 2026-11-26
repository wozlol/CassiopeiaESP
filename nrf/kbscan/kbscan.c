// Keyboard connectivity scanner for the SparkFun nRF52832 breakout. Bare metal, no SoftDevice.
// Drives each keyboard pad low in turn with all others as pull-up inputs and records which others read low.
// rows[i] bit j set means: driving pad i low pulled pad j low. Works with or without diodes in the keyboard.
// Results live in a RAM mailbox at 0x20000000 that the ESP can read over SWD (fallback and flashing).
// Normal use: the nRF debounces the keys, keeps the list of held key pad pairs, raises IO3 when it changes and answers an I2C read from the ESP (address 0x2A, pads 26 SDA and 27 SCL).
// When no key is down for a moment the core sleeps. Group A pads are driven low and group B pads sense low, so any key press wakes it, see tools/kbsleep_sets.py.
#include <stdint.h>
#include "wake_sets.h"

#define P0 0x50000000UL
#define OUTCLR (*(volatile uint32_t *)(P0 + 0x50C))
#define IN (*(volatile uint32_t *)(P0 + 0x510))
#define PIN_CNF(n) (*(volatile uint32_t *)(P0 + 0x700 + 4 * (n)))
#define CNF_IN_PULLUP 0x0000000CUL
#define CNF_OUT 0x00000001UL
#define CNF_OUT_NOBUF 0x00000003UL
#define CNF_IN_PULLUP_SENSE_LOW 0x0003000CUL
#define LATCH (*(volatile uint32_t *)(P0 + 0x520))
#define OUTSET (*(volatile uint32_t *)(P0 + 0x508))
#define GPIOTE_EVENTS_PORT (*(volatile uint32_t *)0x4000617CUL)
#define GPIOTE_INTENSET (*(volatile uint32_t *)0x40006304UL)
#define GPIOTE_INTENCLR (*(volatile uint32_t *)0x40006308UL)
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100UL)
#define NVIC_ICER0 (*(volatile uint32_t *)0xE000E180UL)
#define NVIC_ICPR0 (*(volatile uint32_t *)0xE000E280UL)
#define SCB_SCR (*(volatile uint32_t *)0xE000ED10UL)
#define GPIOTE_IRQ_BIT (1UL << 6)
#define IDLE_SCANS_BEFORE_SLEEP 60
#define IO3_PAD 25
#define FW_TAG 0x57414B31UL   // WAK1
#define I2C_ADDR 0x2A
#define I2C_SCL_PAD 27
#define I2C_SDA_PAD 26
#define TW(off) (*(volatile uint32_t *)(0x40003000UL + (off)))
#define TWIS_IRQ_BIT (1UL << 3)
#define REPORT_BYTES 32
#define TM(off) (*(volatile uint32_t *)(0x40008000UL + (off)))   // TIMER0 counts only while the core is awake, 31.25 kHz
#define REPORT_MAX_PAIRS 10

// Keyboard pads in flex wire order KB1 to KB25, see WIRING.md. Pad 30 is spare, not scanned.
static const uint8_t PADS[] = {31, 29, 28, 24, 23, 22, 20, 19, 18, 17, 16, 2, 3, 4, 5, 8, 11, 12, 13, 14, 15, 9, 10, 21};
#define NPADS (sizeof(PADS) / sizeof(PADS[0]))

// Mailbox layout in 32 bit words: 0 magic, 1 scan counter, 2 pad count, 3 state (1 scanning, 2 asleep), 4..35 pad numbers, 36..67 rows, 68 wake counter.
// ESP wake support: 69 control (ESP writes, bit 0 = ESP is asleep, watch for the wake key), 70 wake key as pad A | pad B << 8, 71 latched flag (nRF sets),
// 72..95 rows copied at the moment the wake key was seen, so the ESP can tell which keys were held with it, 96 firmware tag, 97 debounce counter.
// IO3 (ESP GPIO3) is driven high on pad 25 when the wake key is latched. The ESP clears it after waking.
// I2C link: 110..133 debounced rows, 134..157 candidate rows, 158 candidate count, 159 change counter, 160 change counter last sent, 161..166 report (24 bytes),
// 176..177 receive buffer, 169 set while a read is being answered, 170 times IO3 was raised for a change, 171 reads answered, 172 times a read finished and IO3 was lowered.
// Report (32 bytes): byte 0 = 0xA5, 1 = change counter, 2 = number of pairs n, then n pairs of nRF pad numbers (low, high) up to byte 22,
// 24..27 awake time so far in 32 microsecond ticks (little endian, wraps after about 37 hours), 28..29 number of wakes from sleep.
#define MB ((volatile uint32_t *)0x20000000UL)
#define MAGIC 0x4B425343UL

extern uint32_t _estack;
void Reset_Handler(void);
void Default_Handler(void) { for (;;) {} }

__attribute__((section(".vectors"), used))
void *const vectors[16] = {
    &_estack, Reset_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, 0, 0, 0, 0, Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler,
};

static void settle(void) {
    for (volatile uint32_t i = 0; i < 300; i++) {}
}

static uint32_t pad_mask(void) {
    uint32_t m = 0;
    for (uint32_t k = 0; k < NPADS; k++) m |= 1UL << PADS[k];
    return m;
}

static void twis_init(void) {
    PIN_CNF(I2C_SCL_PAD) = 0x00000600UL;   // input, open drain drive, as the I2C slave needs
    PIN_CNF(I2C_SDA_PAD) = 0x00000600UL;
    TW(0x508) = I2C_SCL_PAD;
    TW(0x50C) = I2C_SDA_PAD;
    TW(0x588) = I2C_ADDR;
    TW(0x594) = 1;      // answer address 0 only
    TW(0x5C0) = 0;      // over read character
    TW(0x104) = 0; TW(0x124) = 0; TW(0x164) = 0; TW(0x168) = 0;
    TW(0x500) = 9;      // enable
}

static void compose_report(void) {
    volatile uint8_t *r = (volatile uint8_t *)&MB[161];
    for (uint32_t i = 0; i < REPORT_BYTES; i++) r[i] = 0;
    r[0] = 0xA5;
    r[1] = (uint8_t)MB[159];
    uint32_t n = 0;
    for (uint32_t i = 0; i < NPADS; i++) {
        for (uint32_t j = i + 1; j < NPADS && n < REPORT_MAX_PAIRS; j++) {
            if (!(((MB[110 + i] >> j) | (MB[110 + j] >> i)) & 1)) continue;
            uint8_t a = PADS[i], b = PADS[j];
            if (a > b) { uint8_t t = a; a = b; b = t; }
            r[3 + 2 * n] = a;
            r[4 + 2 * n] = b;
            n++;
        }
    }
    r[2] = (uint8_t)n;
    TM(0x040) = 1;   // capture the awake timer
    uint32_t awake = TM(0x540), wakes = MB[68];
    MB[173] = awake;
    for (uint32_t i = 0; i < 4; i++) r[24 + i] = (uint8_t)(awake >> (8 * i));
    r[28] = (uint8_t)wakes;
    r[29] = (uint8_t)(wakes >> 8);
}

// The ESP asks for the held keys. The slave holds the clock low until we hand it the report, so nothing can change under it.
static void twis_service(void) {
    if (TW(0x168)) {   // read
        TW(0x168) = 0;
        compose_report();
        TW(0x544) = 0x20000000UL + 161 * 4;
        TW(0x548) = REPORT_BYTES;
        TW(0x034) = 1;   // prepare tx
        MB[160] = MB[159];
        MB[169] = 1;
        MB[171]++;
    }
    if (TW(0x164)) {   // write, not used, take and ignore it so the bus never hangs
        TW(0x164) = 0;
        TW(0x534) = 0x20000000UL + 176 * 4;
        TW(0x538) = 8;
        TW(0x030) = 1;   // prepare rx
    }
    if (TW(0x104)) {   // stopped
        TW(0x104) = 0;
        if (MB[169]) {
            MB[169] = 0;
            uint32_t n = ((volatile uint8_t *)&MB[161])[2];
            // Only drop IO3 if the whole list was read and nothing changed since it was made.
            if (TW(0x54C) >= 3 + 2 * n && MB[160] == MB[159] && !(MB[69] & 1)) OUTCLR = 1UL << IO3_PAD;
            MB[172]++;
        }
    }
    if (TW(0x124)) { TW(0x124) = 0; TW(0x4D0) = 0xF; }
}

// A new scan has to match the last one three times in a row before it counts as the key state.
static void debounce(void) {
    for (uint32_t k = 0; k < NPADS; k++) {
        if (MB[134 + k] != MB[36 + k]) {
            for (uint32_t m = 0; m < NPADS; m++) MB[134 + m] = MB[36 + m];
            MB[158] = 1;
            return;
        }
    }
    if (MB[158] < 100) MB[158]++;
    if (MB[158] != 3) return;
    uint32_t diff = 0;
    for (uint32_t k = 0; k < NPADS; k++) if (MB[110 + k] != MB[134 + k]) diff = 1;
    if (!diff) return;
    for (uint32_t k = 0; k < NPADS; k++) MB[110 + k] = MB[134 + k];
    MB[159]++;
    if (!(MB[69] & 1)) { OUTSET = 1UL << IO3_PAD; MB[170]++; }
}

// Sleeps until a key joins a group A pad to a group B pad. Returns at once if a key is already down.
static void sleep_until_key(void) {
    uint32_t mask = pad_mask();
    for (uint32_t k = 0; k < NPADS; k++) {
        uint32_t bit = 1UL << PADS[k];
        PIN_CNF(PADS[k]) = (bit & WAKE_SENSE_MASK) ? CNF_IN_PULLUP_SENSE_LOW : CNF_OUT_NOBUF;
    }
    OUTCLR = mask;
    settle();
    settle();
    if ((IN & WAKE_SENSE_MASK) == WAKE_SENSE_MASK) {
        LATCH = 0xFFFFFFFFUL;
        GPIOTE_EVENTS_PORT = 0;
        (void)GPIOTE_EVENTS_PORT;
        NVIC_ICPR0 = GPIOTE_IRQ_BIT;
        NVIC_ISER0 = GPIOTE_IRQ_BIT;
        GPIOTE_INTENSET = 1UL << 31;
        NVIC_ICPR0 = TWIS_IRQ_BIT;
        NVIC_ISER0 = TWIS_IRQ_BIT;
        TW(0x304) = (1UL << 26) | (1UL << 25);   // an I2C read or write from the ESP also wakes us
        SCB_SCR |= 0x10;   // SEVONPEND, a pending interrupt wakes WFE without running a handler
        MB[3] = 2;
        TM(0x004) = 1;   // stop the awake timer, no counting while asleep
        __asm volatile("cpsid i");
        while (!GPIOTE_EVENTS_PORT && !TW(0x168) && !TW(0x164)) __asm volatile("wfe");
        TM(0x000) = 1;   // start counting awake time again
        MB[3] = 1;
        MB[68]++;
    }
    TW(0x308) = (1UL << 26) | (1UL << 25);
    NVIC_ICER0 = TWIS_IRQ_BIT;
    NVIC_ICPR0 = TWIS_IRQ_BIT;
    GPIOTE_INTENCLR = 1UL << 31;
    GPIOTE_EVENTS_PORT = 0;
    NVIC_ICER0 = GPIOTE_IRQ_BIT;
    NVIC_ICPR0 = GPIOTE_IRQ_BIT;
    LATCH = 0xFFFFFFFFUL;
    for (uint32_t k = 0; k < NPADS; k++) PIN_CNF(PADS[k]) = CNF_IN_PULLUP;
    OUTCLR = mask;
}

static void wake_check(void) {
    if (!(MB[69] & 1) || MB[71]) { MB[97] = 0; return; }
    uint32_t pa = MB[70] & 0xFF, pb = (MB[70] >> 8) & 0xFF;
    int ia = -1, ib = -1;
    for (uint32_t k = 0; k < NPADS; k++) {
        if (PADS[k] == pa) ia = (int)k;
        if (PADS[k] == pb) ib = (int)k;
    }
    if (ia < 0 || ib < 0 || !(((MB[36 + ia] >> ib) | (MB[36 + ib] >> ia)) & 1)) { MB[97] = 0; return; }
    if (++MB[97] < 3) return;
    for (uint32_t k = 0; k < NPADS; k++) MB[72 + k] = MB[36 + k];
    MB[71] = 1;
    OUTSET = 1UL << IO3_PAD;
}

void Reset_Handler(void) {
    uint32_t mask = 0;
    uint32_t idle = 0;
    for (uint32_t k = 0; k < NPADS; k++) {
        PIN_CNF(PADS[k]) = CNF_IN_PULLUP;
        mask |= 1UL << PADS[k];
        MB[4 + k] = PADS[k];
    }
    OUTCLR = mask;
    MB[1] = 0;
    MB[2] = NPADS;
    MB[3] = 1;
    MB[68] = 0;
    for (uint32_t i = 69; i < 180; i++) MB[i] = 0;
    MB[96] = FW_TAG;
    PIN_CNF(IO3_PAD) = CNF_OUT_NOBUF;
    OUTCLR = 1UL << IO3_PAD;
    TM(0x504) = 0;      // timer mode
    TM(0x508) = 3;      // 32 bit
    TM(0x510) = 9;      // 16 MHz / 2^9 = 31.25 kHz
    TM(0x00C) = 1;      // clear
    TM(0x000) = 1;      // start
    twis_init();
    MB[0] = MAGIC;
    for (;;) {
        uint32_t any = 0;
        for (uint32_t k = 0; k < NPADS; k++) {
            PIN_CNF(PADS[k]) = CNF_OUT;
            settle();
            uint32_t in = IN;
            PIN_CNF(PADS[k]) = CNF_IN_PULLUP;
            uint32_t row = 0;
            for (uint32_t j = 0; j < NPADS; j++) {
                if (j != k && !((in >> PADS[j]) & 1)) row |= 1UL << j;
            }
            MB[36 + k] = row;
            any |= row;
        }
        MB[1]++;
        wake_check();
        debounce();
        twis_service();
        if (any) idle = 0;
        else if (++idle >= IDLE_SCANS_BEFORE_SLEEP) {
            sleep_until_key();
            idle = 0;
        }
    }
}
