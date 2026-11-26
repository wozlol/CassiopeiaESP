// SWD transport and nRF52 memory access. Written from the ARM ADIv5 SWD protocol and the nRF52832 product specification.
// Framing cross-checked against atc1441/ESP32_nRF52_SWD and DAPLink SW_DP.c, no code copied.
// ESP UART TX (GPIO43) -> nRF C (SWDCLK), ESP UART RX (GPIO44) -> nRF D (SWDIO).
#pragma once
#include <Arduino.h>

#define SWCLK_PIN 43
#define SWDIO_PIN 44
#define HALF_US 2

#define ACK_OK 1
#define ACK_WAIT 2
#define ACK_FAULT 4
#define ACK_PARITY 8

#define DP_ABORT 0x00
#define DP_IDCODE 0x00
#define DP_CTRLSTAT 0x04
#define DP_SELECT 0x08
#define DP_RDBUFF 0x0C
#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0C
#define AP_IDR 0xFC

#define CSW_SINGLE 0xA2000002
#define CSW_AUTOINC 0xA2000012

static uint8_t lastAck = 0;

static void swdioOut() { pinMode(SWDIO_PIN, OUTPUT); }
static void swdioIn() { pinMode(SWDIO_PIN, INPUT_PULLUP); }

static void clockCycle() {
  digitalWrite(SWCLK_PIN, LOW);
  delayMicroseconds(HALF_US);
  digitalWrite(SWCLK_PIN, HIGH);
  delayMicroseconds(HALF_US);
}

static void bitOut(bool b) {
  digitalWrite(SWDIO_PIN, b);
  clockCycle();
}

static bool bitIn() {
  digitalWrite(SWCLK_PIN, LOW);
  delayMicroseconds(HALF_US);
  bool b = digitalRead(SWDIO_PIN);
  digitalWrite(SWCLK_PIN, HIGH);
  delayMicroseconds(HALF_US);
  return b;
}

static bool parity32(uint32_t v) {
  v ^= v >> 16;
  v ^= v >> 8;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return v & 1;
}

static uint8_t swdTransfer(bool ap, bool read, uint8_t addr, uint32_t *data) {
  bool a2 = (addr >> 2) & 1;
  bool a3 = (addr >> 3) & 1;
  bool par = ap ^ read ^ a2 ^ a3;
  uint8_t req = 1 | (ap << 1) | (read << 2) | (a2 << 3) | (a3 << 4) | (par << 5) | (1 << 7);
  swdioOut();
  for (int i = 0; i < 8; i++) bitOut((req >> i) & 1);
  swdioIn();
  clockCycle();
  uint8_t ack = 0;
  for (int i = 0; i < 3; i++) ack |= (uint8_t)bitIn() << i;
  if (ack == ACK_OK) {
    if (read) {
      uint32_t v = 0;
      for (int i = 0; i < 32; i++) v |= (uint32_t)bitIn() << i;
      bool p = bitIn();
      clockCycle();
      swdioOut();
      *data = v;
      if (p != parity32(v)) ack = ACK_PARITY;
    } else {
      clockCycle();
      swdioOut();
      uint32_t v = *data;
      for (int i = 0; i < 32; i++) bitOut((v >> i) & 1);
      bitOut(parity32(v));
    }
  } else {
    clockCycle();
    swdioOut();
  }
  digitalWrite(SWDIO_PIN, LOW);
  clockCycle();
  clockCycle();
  return ack;
}

static bool xferRetry(bool ap, bool read, uint8_t addr, uint32_t *data) {
  for (int i = 0; i < 50; i++) {
    lastAck = swdTransfer(ap, read, addr, data);
    if (lastAck == ACK_OK) return true;
    if (lastAck != ACK_WAIT) return false;
  }
  return false;
}

static bool dpWrite(uint8_t addr, uint32_t v) { return xferRetry(false, false, addr, &v); }
static bool dpRead(uint8_t addr, uint32_t *v) { return xferRetry(false, true, addr, v); }
static bool apWrite(uint8_t addr, uint32_t v) { return xferRetry(true, false, addr, &v); }

// AP reads are posted, so the value comes back on the next read, fetched here with RDBUFF.
static bool apRead(uint8_t addr, uint32_t *v) {
  uint32_t dummy;
  if (!xferRetry(true, true, addr, &dummy)) return false;
  return xferRetry(false, true, DP_RDBUFF, v);
}

static void swdPinsBegin() {
  pinMode(SWCLK_PIN, OUTPUT);
  digitalWrite(SWCLK_PIN, HIGH);
  swdioIn();
}

static void lineReset() {
  swdioOut();
  digitalWrite(SWDIO_PIN, HIGH);
  for (int i = 0; i < 56; i++) clockCycle();
  for (int i = 0; i < 16; i++) bitOut((0xE79E >> i) & 1);
  for (int i = 0; i < 56; i++) { digitalWrite(SWDIO_PIN, HIGH); clockCycle(); }
  for (int i = 0; i < 8; i++) bitOut(0);
}

static bool connectDp(uint32_t *idcode) {
  lineReset();
  if (!dpRead(DP_IDCODE, idcode)) return false;
  dpWrite(DP_ABORT, 0x1E);
  dpWrite(DP_SELECT, 0);
  dpWrite(DP_CTRLSTAT, 0x50000000);
  for (int i = 0; i < 50; i++) {
    uint32_t st;
    if (dpRead(DP_CTRLSTAT, &st) && (st & 0xA0000000) == 0xA0000000) return true;
    delay(2);
  }
  return false;
}

// True when the sticky error flags are clear. Clears them if set.
static bool dpErrorsClear() {
  uint32_t st = 0;
  if (!dpRead(DP_CTRLSTAT, &st)) return false;
  if (st & 0xB2) {
    dpWrite(DP_ABORT, 0x1E);
    return false;
  }
  return true;
}

static bool readIdr(uint8_t apsel, uint32_t *v) {
  if (!dpWrite(DP_SELECT, ((uint32_t)apsel << 24) | 0xF0)) return false;
  bool ok = apRead(AP_IDR, v);
  dpWrite(DP_SELECT, 0);
  return ok;
}

// CTRL-AP APPROTECTSTATUS bit 0, 1 means debug access is open.
static bool debugOpen() {
  uint32_t prot = 0;
  dpWrite(DP_SELECT, 0x01000000);
  bool ok = apRead(0x0C, &prot);
  dpWrite(DP_SELECT, 0);
  return ok && (prot & 1);
}

static bool memRead(uint32_t addr, uint32_t *v) {
  return apWrite(AP_CSW, CSW_SINGLE) && apWrite(AP_TAR, addr) && apRead(AP_DRW, v);
}

static bool memWrite(uint32_t addr, uint32_t v) {
  return apWrite(AP_CSW, CSW_SINGLE) && apWrite(AP_TAR, addr) && apWrite(AP_DRW, v);
}

// Reads up to 256 words starting at a 1 KB aligned address with TAR auto increment.
static bool blockRead(uint32_t addr, uint32_t *buf, int words) {
  uint32_t dummy;
  if (!apWrite(AP_CSW, CSW_AUTOINC) || !apWrite(AP_TAR, addr)) return false;
  if (!xferRetry(true, true, AP_DRW, &dummy)) return false;
  for (int i = 0; i < words - 1; i++) {
    if (!xferRetry(true, true, AP_DRW, &buf[i])) return false;
  }
  if (!xferRetry(false, true, DP_RDBUFF, &buf[words - 1])) return false;
  return dpErrorsClear();
}

// Writes up to 256 words starting at a 1 KB aligned address with TAR auto increment.
static bool blockWrite(uint32_t addr, const uint32_t *buf, int words) {
  if (!apWrite(AP_CSW, CSW_AUTOINC) || !apWrite(AP_TAR, addr)) return false;
  for (int i = 0; i < words; i++) {
    if (!apWrite(AP_DRW, buf[i])) return false;
  }
  return dpErrorsClear();
}
