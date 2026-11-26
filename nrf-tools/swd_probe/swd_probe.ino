// SWD probe for the SparkFun nRF52832 breakout. READ ONLY, writes nothing to the nRF flash.
// ESP UART TX (GPIO43) -> nRF C (SWDCLK), ESP UART RX (GPIO44) -> nRF D (SWDIO).
// Written from the ARM ADIv5 SWD protocol and the nRF52832 product specification.
// Framing cross-checked against atc1441/ESP32_nRF52_SWD and DAPLink SW_DP.c, no code copied.
// Build with USB CDC On Boot enabled so Serial uses the USB port and leaves GPIO43/44 free.

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

// One SWD transaction. Returns the ACK value (ACK_OK, ACK_WAIT, ACK_FAULT) or 0 or 7 for no target.
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

static uint8_t lastAck = 0;

static bool xferRetry(bool ap, bool read, uint8_t addr, uint32_t *data) {
  for (int i = 0; i < 20; i++) {
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

static bool memRead(uint32_t addr, uint32_t *v) {
  return apWrite(AP_TAR, addr) && apRead(AP_DRW, v);
}

static bool readIdr(uint8_t apsel, uint32_t *v) {
  if (!dpWrite(DP_SELECT, ((uint32_t)apsel << 24) | 0xF0)) return false;
  bool ok = apRead(AP_IDR, v);
  dpWrite(DP_SELECT, 0);
  return ok;
}

static void printAck() {
  Serial.printf("  last ack: %u (1 ok, 2 wait, 4 fault, 0 or 7 nothing answering)\n", lastAck);
}

static void probe() {
  Serial.println("---- SWD probe (read only) ----");
  uint32_t id = 0;
  if (!connectDp(&id)) {
    Serial.println("VERDICT: Bad, no nRF answered.");
    Serial.printf("  DP IDCODE read: 0x%08X, wanted 0x2BA01477\n", id);
    printAck();
    Serial.println("  Check: C to ESP UART TX, D to ESP UART RX, GND, 3.3V on the 3.3V pad, no short.");
    return;
  }
  Serial.printf("DP IDCODE: 0x%08X %s\n", id, id == 0x2BA01477 ? "(Cortex-M4 SW-DP, as expected)" : "(unexpected)");

  uint32_t ahb = 0, ctrl = 0;
  readIdr(0, &ahb);
  readIdr(1, &ctrl);
  Serial.printf("AHB-AP IDR: 0x%08X (nRF52 is 0x24770011)\n", ahb);
  Serial.printf("CTRL-AP IDR: 0x%08X (nRF52 is 0x02880000)\n", ctrl);

  dpWrite(DP_SELECT, 0x01000000);
  uint32_t prot = 0xFFFFFFFF;
  apRead(0x0C, &prot);
  dpWrite(DP_SELECT, 0);
  bool unlocked = prot & 1;
  Serial.printf("APPROTECTSTATUS: %u (%s)\n", (unsigned)(prot & 1), unlocked ? "debug open" : "LOCKED, memory reads will fail");

  if (!unlocked) {
    Serial.println("VERDICT: Bad, nRF found but locked. A full erase is needed and is not done by this probe.");
    return;
  }

  apWrite(AP_CSW, 0xA2000002);
  uint32_t part = 0, variant = 0, ram = 0, flash = 0, pagesz = 0, pages = 0, d0 = 0, d1 = 0;
  bool ok = memRead(0x10000100, &part) && memRead(0x10000104, &variant) && memRead(0x1000010C, &ram) &&
            memRead(0x10000110, &flash) && memRead(0x10000010, &pagesz) && memRead(0x10000014, &pages) &&
            memRead(0x10000060, &d0) && memRead(0x10000064, &d1);
  if (!ok) {
    Serial.println("VERDICT: Bad, nRF found but memory reads failed.");
    printAck();
    return;
  }
  Serial.printf("Part: nRF%X, variant %c%c%c%c\n", part, (char)(variant >> 24), (char)(variant >> 16), (char)(variant >> 8), (char)variant);
  Serial.printf("RAM %u KB, flash %u KB, page %u bytes x %u\n", ram, flash, pagesz, pages);
  Serial.printf("Device ID: %08X%08X\n", d1, d0);

  uint32_t blAddr = 0, pr0 = 0, pr1 = 0, nfc = 0, sp = 0, rv = 0;
  memRead(0x10001014, &blAddr);
  memRead(0x10001200, &pr0);
  memRead(0x10001204, &pr1);
  memRead(0x1000120C, &nfc);
  memRead(0x00000000, &sp);
  memRead(0x00000004, &rv);
  Serial.printf("UICR bootloader address: 0x%08X (all F means none)\n", blAddr);
  Serial.printf("UICR reset pin: 0x%08X, 0x%08X (all F means reset pin disabled)\n", pr0, pr1);
  Serial.printf("UICR NFC pins: 0x%08X (bit0 set means pads 9 and 10 are NFC)\n", nfc);
  Serial.printf("Flash word 0: 0x%08X, word 1: 0x%08X (all F means empty flash)\n", sp, rv);

  if (part == 0x52832) {
    Serial.println("VERDICT: Good, nRF52832 answered and memory reads work.");
  } else {
    Serial.println("VERDICT: Unknown, a chip answered but it is not an nRF52832.");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(SWCLK_PIN, OUTPUT);
  digitalWrite(SWCLK_PIN, HIGH);
  swdioIn();
  delay(1500);
}

void loop() {
  probe();
  delay(4000);
}
