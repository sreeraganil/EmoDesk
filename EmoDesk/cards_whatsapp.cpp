#include "cards.h"
#include "core.h"    // lastMsg, MsgData, rcPush, RC_MSG
#include "audio.h"   // audioSay

// ================================================================= WhatsApp card
//
// Shows the last WhatsApp message pushed to EmoDesk by the bridge script.
// Messages arrive via the existing POST /api/message endpoint with
//   { "from": "<contact>", "text": "<body>", "source": "whatsapp" }
// which writes into lastMsg — the same store used by the generic message card.
//
// This card adds WhatsApp-specific UI:
//   • WhatsApp logo (hand-drawn ✆ in a rounded square)
//   • Green "online" tint via a filled square behind the header icon
//   • "from" name at top-right, relative timestamp below header
//   • Scrolling body text (auto-scrolls when > 4 lines)
//   • Source badge: "WhatsApp" bottom-right (F_TINY)
//   • Tap: clear message   2x tap: read aloud   Hold: scroll toggle
//
// Layout (128×64 OLED, y = baseline):
//  y=7   ┌[✆]─────────────────[Contact name]─┐  header
//  y=9   ────────────────────────────────────   hline
//  y=12  2m ago                               (F_TINY, relative time)
//  y=22+  body text (F_MED, 4 lines max, auto-scroll)
//  y=63  WhatsApp                             (F_TINY right-aligned)

// ---------------------------------------------------------------- WA phone icon
// Draws a minimal handset glyph at (x,y) occupying ~10×10 px.
static void drawWAIcon(int x, int y) {
  // Rounded square background (filled = "online" badge feel)
  u8g2.drawRBox(x, y, 11, 11, 2);
  u8g2.setDrawColor(0);              // draw in background colour (XOR/erase)
  // Handset: an arc top-left and arc bottom-right joined by a bar
  u8g2.drawPixel(x+2, y+3);
  u8g2.drawPixel(x+3, y+2);
  u8g2.drawPixel(x+3, y+3);
  u8g2.drawPixel(x+4, y+3);
  u8g2.drawLine (x+4, y+3, x+7, y+6);
  u8g2.drawPixel(x+7, y+6);
  u8g2.drawPixel(x+7, y+7);
  u8g2.drawPixel(x+8, y+7);
  u8g2.drawPixel(x+6, y+8);
  u8g2.drawPixel(x+7, y+8);
  u8g2.setDrawColor(1);             // restore
}

// ---------------------------------------------------------------- draw

static void wa_draw(CardCtx& c) {
  if (!lastMsg.text[0]) {
    drawCenteredMsg("No WhatsApp msg", "Bridge not running?");
    return;
  }

  // ---- Header row (y=7) ----
  // Icon on left, sender name on right (truncated to 18 chars)
  drawWAIcon(0, 0);

  u8g2.setFont(F_SMALL);
  char from[20];
  snprintf(from, sizeof(from), "%.18s", lastMsg.from[0] ? lastMsg.from : "WhatsApp");
  textRight(127, 7, from);

  // Separator line
  u8g2.drawHLine(0, 9, 128);

  // ---- Relative timestamp (y=17, F_TINY) ----
  u8g2.setFont(F_TINY);
  if (timeOK() && lastMsg.t) {
    char tbuf[20];
    long ago = (long)(time(nullptr) - lastMsg.t);
    if      (ago < 60)          snprintf(tbuf, sizeof(tbuf), "just now");
    else if (ago < 3600)        snprintf(tbuf, sizeof(tbuf), "%ldm ago", ago / 60);
    else if (ago < 86400)       snprintf(tbuf, sizeof(tbuf), "%ldh ago", ago / 3600);
    else                        snprintf(tbuf, sizeof(tbuf), "%ldd ago", ago / 86400);
    u8g2.drawStr(13, 17, tbuf);
  }

  // ---- Message count badge ----
  if (lastMsg.count > 1) {
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "#%lu", (unsigned long)lastMsg.count);
    textRight(127, 17, cnt);
  }

  // ---- Body text (y=22 onward, F_MED, up to 4 lines, auto-scroll) ----
  u8g2.setFont(F_MED);
  int lines = wrapText(0, 0, 124, 11, lastMsg.text, 0);
  // Auto-scroll: advance by one line every 2.5 s; freeze at card entry when c.st[1]==1
  int skip = 0;
  if (lines > 4 && !c.st[1]) {
    skip = min((int)((millis() - c.t0) / 2500 % (lines - 2)), lines - 4);
  }
  wrapText(2, 22, 124, 11, lastMsg.text, 4, skip);

  // ---- Source badge (bottom-right, F_TINY) ----
  u8g2.setFont(F_TINY);
  textRight(127, 63, "WhatsApp");
}

// ---------------------------------------------------------------- input

static bool wa_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) {
    if (lastMsg.text[0]) {
      audioSay(lastMsg.text, EV_UI);
      lastMsg.text[0] = 0;
      lastMsg.from[0] = 0;
      lastMsg.count   = 0;
    }
    return true;
  }
  return false;
}

static void wa_speak(CardCtx& c, String& s) {
  if (lastMsg.text[0]) {
    s = "WhatsApp from ";
    s += lastMsg.from;
    s += ": ";
    s += lastMsg.text;
  } else {
    s = "No WhatsApp messages.";
  }
}

const CardType CT_whatsapp = {
  "whatsapp",  // id
  0,           // dsize — zero heap per instance (shares lastMsg global)
  0,           // refresh — push-based, no fetch
  0,           // flags
  nullptr,     // enter
  wa_draw,     // draw
  wa_input,    // input
  nullptr,     // fetch
  wa_speak,    // speak
  nullptr      // tick
};
