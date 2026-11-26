// nRF52832 tool for the ESP32-S3. Commands over USB serial, one per line.
//   info            read only, chip and protection details
//   dump            read only, prints the whole flash and UICR as Intel HEX (all 0xFF lines skipped)
//   verify          read only, compares chip flash with the image built into this sketch
//   flash CONFIRM   halts the nRF, erases only the pages the image covers, writes, verifies, runs
//   run             resets the nRF and lets it run
// Every command ends with a line starting RESULT:
// Pins: see swd.h. Needs USB CDC On Boot so Serial is the USB port.

#include <Arduino.h>
#include "swd.h"
#include "nrf_image.h"

#define NVMC_READY 0x4001E400UL
#define NVMC_CONFIG 0x4001E504UL
#define NVMC_ERASEPAGE 0x4001E508UL
#define DHCSR 0xE000EDF0UL
#define DEMCR 0xE000EDFCUL
#define AIRCR 0xE000ED0CUL

#define FLASH_SIZE 0x80000UL
#define PAGE_SIZE 4096UL
#define PROTECT_FROM 0x7A000UL
#define UICR_BASE 0x10001000UL
#define UICR_DUMP_LEN 0x400UL

static void result(bool ok, const char *msg) {
  Serial.printf("RESULT: %s %s\n", ok ? "OK" : "FAIL", msg);
}

static bool connectChecked() {
  uint32_t id = 0;
  if (!connectDp(&id)) {
    Serial.printf("No answer from nRF, IDCODE 0x%08X, last ack %u\n", id, lastAck);
    return false;
  }
  if (!debugOpen()) {
    Serial.println("nRF is locked (APPROTECT), a full erase would be needed");
    return false;
  }
  return true;
}

static bool waitNvmcReady(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  uint32_t r = 0;
  while (millis() - t0 < timeoutMs) {
    if (memRead(NVMC_READY, &r) && (r & 1)) return true;
  }
  return false;
}

static bool isHalted() {
  uint32_t d = 0;
  return memRead(DHCSR, &d) && (d & (1UL << 17));
}

// Reset the core and stop it before it runs any code, so flash write protection set by old code is cleared.
static bool resetHalt() {
  if (!memWrite(DEMCR, 0x01000001UL)) return false;
  if (!memWrite(DHCSR, 0xA05F0003UL)) return false;
  memWrite(AIRCR, 0x05FA0004UL);
  delay(30);
  uint32_t id;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (connectDp(&id) && isHalted()) return true;
    delay(20);
  }
  return false;
}

static bool resetRun() {
  memWrite(DEMCR, 0);
  memWrite(DHCSR, 0xA05F0000UL);
  memWrite(AIRCR, 0x05FA0004UL);
  return true;
}

static void ihexLine(uint8_t len, uint16_t addr, uint8_t type, const uint8_t *data) {
  uint8_t sum = len + (addr >> 8) + (addr & 0xFF) + type;
  Serial.printf(":%02X%04X%02X", len, addr, type);
  for (int i = 0; i < len; i++) {
    Serial.printf("%02X", data[i]);
    sum += data[i];
  }
  Serial.printf("%02X\n", (uint8_t)(0x100 - sum));
}

static bool dumpRegion(uint32_t base, uint32_t length) {
  static uint32_t buf[256];
  uint32_t lastUpper = 0xFFFFFFFF;
  for (uint32_t off = 0; off < length; off += 1024) {
    if (!blockRead(base + off, buf, 256)) return false;
    const uint8_t *bytes = (const uint8_t *)buf;
    for (int i = 0; i < 1024; i += 16) {
      bool allFF = true;
      for (int j = 0; j < 16; j++) if (bytes[i + j] != 0xFF) { allFF = false; break; }
      if (allFF) continue;
      uint32_t a = base + off + i;
      if ((a >> 16) != lastUpper) {
        lastUpper = a >> 16;
        uint8_t hi[2] = {(uint8_t)(lastUpper >> 8), (uint8_t)lastUpper};
        ihexLine(2, 0, 4, hi);
      }
      ihexLine(16, a & 0xFFFF, 0, bytes + i);
    }
  }
  return true;
}

static void cmdInfo() {
  if (!connectChecked()) { result(false, "no nRF or locked"); return; }
  uint32_t part = 0, variant = 0, ram = 0, flash = 0, bl = 0, nfc = 0, sp = 0, rv = 0;
  bool ok = memRead(0x10000100, &part) && memRead(0x10000104, &variant) && memRead(0x1000010C, &ram) &&
            memRead(0x10000110, &flash) && memRead(0x10001014, &bl) && memRead(0x1000120C, &nfc) &&
            memRead(0, &sp) && memRead(4, &rv);
  if (!ok) { result(false, "memory read failed"); return; }
  Serial.printf("nRF%X variant %c%c%c%c, RAM %u KB, flash %u KB\n", part, (char)(variant >> 24), (char)(variant >> 16), (char)(variant >> 8), (char)variant, ram, flash);
  Serial.printf("UICR bootloader 0x%08X, NFC pins 0x%08X, flash word0 0x%08X word1 0x%08X\n", bl, nfc, sp, rv);
  Serial.printf("Image in sketch: %lu bytes at 0x%08lX\n", NRF_IMAGE_LEN, NRF_IMAGE_ADDR);
  result(part == 0x52832, part == 0x52832 ? "nRF52832 found" : "unexpected chip");
}

static void cmdDump() {
  if (!connectChecked()) { result(false, "no nRF or locked"); return; }
  Serial.println("DUMP BEGIN");
  bool ok = dumpRegion(0, FLASH_SIZE) && dumpRegion(UICR_BASE, UICR_DUMP_LEN);
  Serial.println(":00000001FF");
  result(ok, ok ? "dump complete" : "read error during dump");
}

static bool verifyImage(uint32_t *firstBad) {
  static uint32_t buf[256];
  const uint32_t *img = (const uint32_t *)NRF_IMAGE;
  uint32_t words = NRF_IMAGE_LEN / 4;
  for (uint32_t w = 0; w < words; w += 256) {
    uint32_t n = min((uint32_t)256, words - w);
    if (!blockRead(NRF_IMAGE_ADDR + w * 4, buf, n)) { *firstBad = NRF_IMAGE_ADDR + w * 4; return false; }
    for (uint32_t i = 0; i < n; i++) {
      uint32_t expect;
      memcpy(&expect, &NRF_IMAGE[(w + i) * 4], 4);
      if (buf[i] != expect) { *firstBad = NRF_IMAGE_ADDR + (w + i) * 4; return false; }
    }
  }
  (void)img;
  return true;
}

static void cmdVerify() {
  if (!connectChecked()) { result(false, "no nRF or locked"); return; }
  uint32_t bad = 0;
  bool ok = verifyImage(&bad);
  if (ok) result(true, "chip flash matches the image");
  else { Serial.printf("Mismatch or read error at 0x%08X\n", bad); result(false, "chip flash differs from the image"); }
}

static void cmdFlash() {
  if (NRF_IMAGE_ADDR % PAGE_SIZE != 0 || NRF_IMAGE_ADDR + NRF_IMAGE_LEN > PROTECT_FROM) {
    result(false, "image would touch the bootloader area or is not page aligned, refused");
    return;
  }
  if (!connectChecked()) { result(false, "no nRF or locked"); return; }
  uint32_t firstPage = NRF_IMAGE_ADDR / PAGE_SIZE;
  uint32_t lastPage = (NRF_IMAGE_ADDR + NRF_IMAGE_LEN - 1) / PAGE_SIZE;
  Serial.printf("Plan: erase pages %lu to %lu (0x%08lX to 0x%08lX), write %lu bytes\n", firstPage, lastPage, firstPage * PAGE_SIZE, (lastPage + 1) * PAGE_SIZE - 1, NRF_IMAGE_LEN);
  if (!resetHalt()) { result(false, "could not halt the nRF core"); return; }
  Serial.println("Core halted");
  for (uint32_t p = firstPage; p <= lastPage; p++) {
    bool ok = memWrite(NVMC_CONFIG, 2) && memWrite(NVMC_ERASEPAGE, p * PAGE_SIZE) && waitNvmcReady(1000);
    memWrite(NVMC_CONFIG, 0);
    if (!ok) { Serial.printf("Erase failed on page %lu\n", p); result(false, "erase failed"); return; }
  }
  Serial.println("Erased");
  uint32_t words = NRF_IMAGE_LEN / 4;
  static uint32_t buf[256];
  if (!memWrite(NVMC_CONFIG, 1)) { result(false, "could not enable flash write"); return; }
  bool wok = true;
  for (uint32_t w = 0; w < words && wok; w += 256) {
    uint32_t n = min((uint32_t)256, words - w);
    memcpy(buf, &NRF_IMAGE[w * 4], n * 4);
    wok = blockWrite(NRF_IMAGE_ADDR + w * 4, buf, n) && waitNvmcReady(1000);
  }
  memWrite(NVMC_CONFIG, 0);
  if (!wok) { result(false, "write failed"); return; }
  Serial.println("Written");
  uint32_t bad = 0;
  if (!verifyImage(&bad)) { Serial.printf("Verify failed at 0x%08X\n", bad); result(false, "verify failed, nRF left halted"); return; }
  Serial.println("Verified");
  resetRun();
  result(true, "flashed, verified and running");
}

static void cmdRun() {
  if (!connectChecked()) { result(false, "no nRF or locked"); return; }
  resetRun();
  result(true, "nRF reset and running");
}

// Reads the scanner mailbox in nRF RAM. Does not halt or disturb the nRF.
static void cmdSnap() {
  static uint32_t mb[68];
  uint32_t id = 0;
  if (!connectDp(&id)) { result(false, "no nRF answered"); return; }
  if (!blockRead(0x20000000UL, mb, 68)) { result(false, "RAM read failed"); return; }
  if (mb[0] != 0x4B425343UL) { result(false, "nRF is not running the scanner image"); return; }
  Serial.printf("SNAP %u %u", mb[1], mb[2]);
  for (int i = 4; i < 68; i++) Serial.printf(" %08X", mb[i]);
  Serial.println();
  result(true, "snapshot");
}

// One time: make pads 9, 10 (NFC) and 21 (reset) normal pins without touching the crystal pads 0 and 1.
// NFCPINS bit 0 cleared means GPIO. PSELRESET both words erased (all F) means no reset pin.
// Both need an UICR erase first because PSELRESET currently has bits to set. The full dump in backup/ includes the UICR.
static void cmdFreePads() {
  const uint32_t NFCPINS = 0x1000120CUL;
  const uint32_t PSELRESET0 = 0x10001200UL;
  const uint32_t PSELRESET1 = 0x10001204UL;
  const uint32_t NVMC_ERASEUICR = 0x4001E514UL;
  if (!connectChecked()) { result(false, "no nRF or locked"); return; }
  uint32_t nfc = 0, r0 = 0, r1 = 0;
  if (!memRead(NFCPINS, &nfc) || !memRead(PSELRESET0, &r0) || !memRead(PSELRESET1, &r1)) { result(false, "UICR read failed"); return; }
  Serial.printf("Now: NFCPINS 0x%08X, PSELRESET 0x%08X 0x%08X\n", nfc, r0, r1);
  if (nfc == 0xFFFFFFFEUL && r0 == 0xFFFFFFFFUL && r1 == 0xFFFFFFFFUL) { result(true, "already set, pads 9, 10 and 21 are normal pins"); return; }
  if (!resetHalt()) { result(false, "could not halt the nRF core"); return; }
  bool ok = true;
  if (r0 != 0xFFFFFFFFUL || r1 != 0xFFFFFFFFUL) {
    ok = memWrite(NVMC_CONFIG, 2) && memWrite(NVMC_ERASEUICR, 1) && waitNvmcReady(1000);
    memWrite(NVMC_CONFIG, 0);
    if (!ok) { result(false, "UICR erase failed"); return; }
    Serial.println("UICR erased");
  }
  ok = memWrite(NVMC_CONFIG, 1) && memWrite(NFCPINS, 0xFFFFFFFEUL) && waitNvmcReady(1000);
  memWrite(NVMC_CONFIG, 0);
  uint32_t n2 = 0, a2 = 0, b2 = 0;
  ok = ok && memRead(NFCPINS, &n2) && memRead(PSELRESET0, &a2) && memRead(PSELRESET1, &b2) &&
       n2 == 0xFFFFFFFEUL && a2 == 0xFFFFFFFFUL && b2 == 0xFFFFFFFFUL;
  Serial.printf("After: NFCPINS 0x%08X, PSELRESET 0x%08X 0x%08X\n", n2, a2, b2);
  resetRun();
  result(ok, ok ? "pads 9, 10 and 21 are now normal pins" : "write or readback failed");
}

static void handle(String line) {
  line.trim();
  if (line == "snap") cmdSnap();
  else if (line == "freepads CONFIRM") cmdFreePads();
  else if (line == "info") cmdInfo();
  else if (line == "dump") cmdDump();
  else if (line == "verify") cmdVerify();
  else if (line == "flash CONFIRM") cmdFlash();
  else if (line == "flash") result(false, "type flash CONFIRM to erase and write");
  else if (line == "run") cmdRun();
  else if (line.length()) result(false, "commands: info, dump, verify, flash CONFIRM, run");
}

void setup() {
  Serial.begin(115200);
  swdPinsBegin();
}

void loop() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handle(line);
      line = "";
    } else if (line.length() < 40) {
      line += c;
    }
  }
}
