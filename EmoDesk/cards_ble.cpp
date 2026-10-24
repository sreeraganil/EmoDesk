#include "cards.h"
#include "services.h"   // bleMode(), bleStatusCopy(), blePresent(), bleMedia()
#include "power.h"      // powerSaver(), batteryPercent()

// ================================================================== BLE card
//
// Two pages on a 128×64 OLED (U8g2, y = text baseline):
//
//   Page 0 — STATUS
//   ┌────────────────────────────────┐  y=0
//   │ Bluetooth          PRESENCE   │  y=7  F_SMALL header + hline at y=9
//   │ ──────────────────────────── │
//   │ [ᗒ icon, 9×14px]             │
//   │ you are here                  │  y=20  status (marquee if long)
//   │                               │
//   │ RAM [██████████░░░] 52K free  │  y=32  bar y=24..31, numbers at y=32
//   │ blk 18K      target: MyWatch  │  y=41  F_TINY detail row
//   │ CPU 160 MHz    min free 44K   │  y=50  F_TINY info row
//   │ tap=play   2×=next  hold=vol+ │  y=62  F_TINY hint (remote mode only)
//   └────────────────────────────────┘
//
//   Page 1 — REMOTE  (auto-shown when mode=="remote")
//   ┌────────────────────────────────┐
//   │ BLE Remote          paired    │  y=7  header
//   │ ─────────────────────────────── │
//   │ tap           play / pause    │  y=22  F_SMALL
//   │ 2× tap        next track      │  y=33
//   │ 3× tap        prev track      │  y=44
//   │ hold          vol +           │  y=55
//   │ long hold     vol −           │  y=64  clipped to 63 bottom
//   └────────────────────────────────┘
//
// RAM cost: dsize = 0 — no per-instance heap allocation.
// The only local variables are stack-allocated char arrays.

#if ENABLE_BLE

// ---------------------------------------------------------------- BLE glyph
//
//  Classic Bluetooth ᗒ shape: vertical bar + upper-right + lower-right chevron.
//  cx/cy = centre of the glyph; r = half-height (total height = 2r).

static void drawBleGlyph(int cx, int cy, int r) {
  u8g2.drawVLine(cx, cy - r, r * 2 + 1);           // vertical bar
  u8g2.drawLine(cx, cy - r,     cx + r, cy - r/2); // upper right: bar-top  → right-mid-top
  u8g2.drawLine(cx + r, cy - r/2, cx, cy);          // upper right: right-mid-top → centre
  u8g2.drawLine(cx, cy,       cx + r, cy + r/2);   // lower right: centre → right-mid-bot
  u8g2.drawLine(cx + r, cy + r/2, cx, cy + r);     // lower right: right-mid-bot → bar-bot
}

// ---------------------------------------------------------------- page 0: STATUS

static void ble_drawStatus(CardCtx& c) {
  // Mode label for the header — stack only, no heap.
  const char* m = bleMode();
  char modeLabel[12];
  if      (!strcmp(m, "presence")) snprintf(modeLabel, sizeof(modeLabel), "PRESENCE");
  else if (!strcmp(m, "remote"))   snprintf(modeLabel, sizeof(modeLabel), "REMOTE");
  else                             snprintf(modeLabel, sizeof(modeLabel), "OFF");

  drawTitle(c, "Bluetooth", modeLabel);
  // hline drawn by drawTitle at y=9; content starts at y=10.

  // ---- BLE glyph (left column, x=0..10) ----
  // Centre at (5, 19), r=8  →  spans y=11..27, x=0..13
  drawBleGlyph(5, 19, 8);

  // Animated pulse circle when device is present
  if (blePresent()) {
    uint32_t phase = (millis() / 500) % 3;          // 0,1,2 → 0,4,8 px extra radius
    u8g2.drawCircle(5, 19, 11 + (int)phase * 4);
  }

  // ---- Status string (right of glyph) ----
  char st[48];
  bleStatusCopy(st, sizeof(st));

  u8g2.setFont(F_SMALL);
  int stW = u8g2.getStrWidth(st);
  if (stW > 108) {
    marquee(14, 20, 113, st, c.t0, 28);
  } else {
    u8g2.drawStr(14, 20, st);
  }

  // ---- RAM bar row (y=24..31 bar, label+number at y=32) ----
  uint32_t freeH  = ESP.getFreeHeap();
  uint32_t totalH = ESP.getHeapSize();
  float used = (totalH > 0) ? 1.0f - (float)freeH / totalH : 0.0f;

  u8g2.setFont(F_TINY);
  u8g2.drawStr(0, 32, "RAM");
  drawBar(18, 25, 68, 7, used);             // bar: x=18..86, y=25..31
  char nbuf[16];
  snprintf(nbuf, sizeof(nbuf), "%uK fr", (unsigned)(freeH / 1024));
  textRight(127, 32, nbuf);

  // ---- Detail row: largest free block + target (y=42) ----
  u8g2.setFont(F_TINY);
  snprintf(nbuf, sizeof(nbuf), "blk %uK", (unsigned)(heapLargest() / 1024));
  u8g2.drawStr(0, 42, nbuf);

  // Target device (read directly from cfg — we're inside LOCK() already)
  const char* tgt = cfg["ble"]["target"] | "";
  if (tgt[0]) {
    char tb[28];
    snprintf(tb, sizeof(tb), "tgt: %.20s", tgt);
    textRight(127, 42, tb);
  }

  // ---- Info row: CPU MHz + min free (y=51) ----
  char ibuf[32];
  uint32_t minFree = ESP.getMinFreeHeap();
  snprintf(ibuf, sizeof(ibuf), "CPU %uMHz  min %uK",
           (unsigned)(ESP.getCpuFreqMHz()),
           (unsigned)(minFree / 1024));
  u8g2.setFont(F_TINY);
  u8g2.drawStr(0, 51, ibuf);

  // ---- Hint row (y=62, only in remote mode) ----
  if (!strcmp(m, "remote")) {
    u8g2.setFont(F_TINY);
    textCenter(62, "tap=play  2x=next  hold=vol+");
  }
}

// ---------------------------------------------------------------- page 1: REMOTE

static void ble_drawRemote(CardCtx& c) {
  // Status tells us paired/not-paired without a String heap alloc.
  char st[32];
  bleStatusCopy(st, sizeof(st));
  bool paired = !strcmp(st, "connected");

  drawTitle(c, "BLE Remote", paired ? "paired" : "not paired");

  u8g2.setFont(F_SMALL);
  // Five gesture rows — evenly spaced from y=22 to y=62 (11px step).
  static const char* const GESTURES[] = {"tap", "2\xD7 tap", "3\xD7 tap", "hold", "long hold"};
  static const char* const ACTIONS[]  = {"play / pause", "next track", "prev track", "vol +", "vol \x2D"};
  for (int i = 0; i < 5; i++) {
    int y = 22 + i * 11;
    u8g2.drawStr(0, y, GESTURES[i]);
    textRight(127, y, ACTIONS[i]);
  }

  // If not yet paired, dim-overlay the bottom hint.
  if (!paired) {
    u8g2.setFont(F_TINY);
    textCenter(63, "waiting for pairing…");
  }
}

// ---------------------------------------------------------------- card callbacks

// st[0] = current page  (0 = status, 1 = remote)
// Page is auto-flipped every 8 s when mode == "remote"; presence mode shows status only.

static void ble_draw(CardCtx& c) {
  bool isRemote = !strcmp(bleMode(), "remote");
  if (isRemote) {
    c.st[0] = (millis() - c.t0) / 8000 % 2;
  } else {
    c.st[0] = 0;
  }

  if (c.st[0] == 1) ble_drawRemote(c);
  else              ble_drawStatus(c);
}

static bool ble_input(CardCtx& c, uint8_t g) {
  bool isRemote = !strcmp(bleMode(), "remote");
  switch (g) {
    case G_TAP:    if (isRemote) { bleMedia("play");     return true; } return false;
    case G_DOUBLE: if (isRemote) { bleMedia("next");     return true; } return false;
    case G_TRIPLE: if (isRemote) { bleMedia("prev");     return true; } return false;
    case G_LONG:   if (isRemote) { bleMedia("volup");    return true; } return false;
    case G_VLONG:  if (isRemote) { bleMedia("voldown");  return true; } return false;
    default: return false;
  }
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
  if (isRemote) s += " Remote mode: tap to play, double-tap for next track.";
}

const CardType CT_ble = {
  "ble",     // id — must match CARD_LIST / dashboard entry
  0,         // dsize: zero — no per-instance heap allocation
  0,         // refresh: no fetch() needed; all data is live
  0,         // flags
  nullptr,   // enter
  ble_draw,  // draw
  ble_input, // input
  nullptr,   // fetch
  ble_speak, // speak
  nullptr    // tick
};

#else  // ENABLE_BLE == 0 — friendly stub
static void ble_draw_stub(CardCtx& c) {
  drawTitle(c, "Bluetooth");
  drawCenteredMsg("BLE not enabled", "Set ENABLE_BLE 1");
}
const CardType CT_ble = {"ble", 0, 0, 0, nullptr, ble_draw_stub, nullptr, nullptr, nullptr, nullptr};
#endif
