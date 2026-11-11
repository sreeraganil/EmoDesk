#include "cards.h"
#include "services.h"
#include "power.h"

#if ENABLE_BLE

static void drawBleGlyph(int cx, int cy, int r) {
  u8g2.drawVLine(cx, cy - r, r * 2 + 1);
  u8g2.drawLine(cx, cy - r,       cx + r, cy - r / 2);
  u8g2.drawLine(cx + r, cy - r / 2, cx, cy);
  u8g2.drawLine(cx, cy,           cx + r, cy + r / 2);
  u8g2.drawLine(cx + r, cy + r / 2, cx, cy + r);
}

static void ble_drawStatus(CardCtx& c) {
  const char* m = bleMode();
  char modeLabel[12];
  if      (!strcmp(m, "presence")) strlcpy(modeLabel, "PRESENCE", sizeof(modeLabel));
  else if (!strcmp(m, "remote"))   strlcpy(modeLabel, "REMOTE", sizeof(modeLabel));
  else                             strlcpy(modeLabel, "OFF", sizeof(modeLabel));

  drawTitle(c, "Bluetooth", modeLabel);

  // Bounded BLE glyph inside x=2..14, y=13..25
  drawBleGlyph(5, 19, 6);
  if (blePresent()) {
    uint32_t ph = (millis() / 350) % 3;
    if (ph >= 1) u8g2.drawPixel(14, 17);
    if (ph >= 2) { u8g2.drawPixel(16, 15); u8g2.drawPixel(16, 19); u8g2.drawPixel(16, 23); }
  }

  char st[48];
  bleStatusCopy(st, sizeof(st));
  u8g2.setFont(F_SMALL);
  if (u8g2.getStrWidth(st) > 105) marquee(21, 22, 105, st, c.t0, 28);
  else u8g2.drawStr(21, 22, st);

  uint32_t freeH  = ESP.getFreeHeap();
  uint32_t totalH = ESP.getHeapSize();
  float used = (totalH > 0) ? 1.0f - (float)freeH / totalH : 0.0f;

  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 34, "RAM");
  drawBar(22, 28, 60, 6, used);
  char nbuf[20];
  snprintf(nbuf, sizeof(nbuf), "%uK fr", (unsigned)(freeH / 1024));
  drawTextRightFit(126, 34, 40, nbuf);

  snprintf(nbuf, sizeof(nbuf), "Blk %uK", (unsigned)(heapLargest() / 1024));
  drawTextFit(2, 44, 52, nbuf);

  const char* tgt = cfg["ble"]["target"] | "";
  if (tgt[0]) {
    char tb[28];
    snprintf(tb, sizeof(tb), "Target: %s", tgt);
    drawTextRightFit(126, 44, 70, tb);
  }

  snprintf(nbuf, sizeof(nbuf), "CPU %uMHz  |  Min %uK free", (unsigned)ESP.getCpuFreqMHz(), (unsigned)(ESP.getMinFreeHeap() / 1024));
  drawTextFitCenter(64, 53, 124, nbuf);

  if (!strcmp(m, "remote")) {
    drawDottedHLine(0, 55, 128);
    textCenter(63, "2x: play/pause  |  hold: next");
  }
}

static void ble_drawRemote(CardCtx& c) {
  char st[32];
  bleStatusCopy(st, sizeof(st));
  bool paired = !strcmp(st, "connected");

  drawTitle(c, "BLE Remote", paired ? "paired" : "waiting");

  u8g2.setFont(F_SMALL);
  static const char* const GESTURES[] = {"2x tap", "hold (1s)", "tap / 3x"};
  static const char* const ACTIONS[]  = {"play / pause", "next track", "next card / home"};
  for (int i = 0; i < 3; i++) {
    int y = 23 + i * 11;
    u8g2.drawStr(2, y, GESTURES[i]);
    textRight(126, y, ACTIONS[i]);
  }

  drawDottedHLine(0, 52, 128);
  u8g2.setFont(F_TINY);
  textCenter(61, paired ? "Connected via BLE HID" : "Pair in device Bluetooth");
}

static void ble_draw(CardCtx& c) {
  bool isRemote = !strcmp(bleMode(), "remote");
  c.st[0] = isRemote ? ((millis() - c.t0) / 8000 % 2) : 0;
  if (c.st[0] == 1) ble_drawRemote(c);
  else              ble_drawStatus(c);
}

static bool ble_input(CardCtx& c, uint8_t g) {
  bool isRemote = !strcmp(bleMode(), "remote");
  if (!isRemote) return false;
  // Leave G_TAP (next card) and G_TRIPLE (home) free so navigation never locks!
  if (g == G_DOUBLE) { bleMedia("play"); return true; }
  if (g == G_LONG)   { bleMedia("next"); return true; }
  return false;
}

static void ble_speak(CardCtx& c, String& s) {
  char st[64];
  bleStatusCopy(st, sizeof(st));
  bool isRemote = !strcmp(bleMode(), "remote");
  s  = "Bluetooth is ";
  s += st;
  s += ". RAM: ";
  s += numWords(ESP.getFreeHeap() / 1024);
  s += " kilobytes free.";
  if (isRemote) s += " Remote mode: double-tap to play or pause, hold for next track.";
}

const CardType CT_ble = {
  "ble", 0, 0, 0, nullptr, ble_draw, ble_input, nullptr, ble_speak, nullptr
};

#else
static void ble_draw_stub(CardCtx& c) {
  drawTitle(c, "Bluetooth", "off");
  drawCenteredMsg("BLE Disabled", "Enable ENABLE_BLE in config.h");
}
const CardType CT_ble = {"ble", 0, 0, 0, nullptr, ble_draw_stub, nullptr, nullptr, nullptr, nullptr};
#endif
