#include "cards.h"
#include "core.h"
#include "audio.h"

// Compact 9x9 WhatsApp icon strictly inside x..x+8, y..y+8
static void drawWAIcon(int x, int y) {
  u8g2.drawRBox(x, y, 9, 9, 2);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(x + 2, y + 2);
  u8g2.drawPixel(x + 3, y + 2);
  u8g2.drawLine(x + 3, y + 3, x + 5, y + 5);
  u8g2.drawPixel(x + 6, y + 5);
  u8g2.drawPixel(x + 6, y + 6);
  u8g2.drawPixel(x + 5, y + 6);
  u8g2.setDrawColor(1);
}

static void wa_draw(CardCtx& c) {
  if (!lastMsg.text[0]) {
    drawTitle(c, "WhatsApp");
    drawCenteredMsg("No messages yet", "Waiting for bridge push");
    return;
  }

  // Top bar (y=0..10): icon left, sender name right-safe of battery pill
  drawWAIcon(0, 0);
  int rightLim = 127 - displayBatReserve();
  u8g2.setFont(F_SMALL);
  const char* from = lastMsg.from[0] ? lastMsg.from : "WhatsApp";
  drawTextFit(12, 7, rightLim - 14, from);
  u8g2.drawHLine(0, 10, 128);

  // Metadata row (y=12..18): relative age on left, counter on right
  u8g2.setFont(F_TINY);
  if (timeOK() && lastMsg.t) {
    char tbuf[20];
    long ago = max(0L, (long)(time(nullptr) - lastMsg.t));
    if      (ago < 60)    strlcpy(tbuf, "just now", sizeof(tbuf));
    else if (ago < 3600)  snprintf(tbuf, sizeof(tbuf), "%ldm ago", ago / 60);
    else if (ago < 86400) snprintf(tbuf, sizeof(tbuf), "%ldh ago", ago / 3600);
    else                  snprintf(tbuf, sizeof(tbuf), "%ldd ago", ago / 86400);
    u8g2.drawStr(2, 17, tbuf);
  } else {
    u8g2.drawStr(2, 17, "Recent");
  }
  if (lastMsg.count > 1) {
    char cnt[10];
    snprintf(cnt, sizeof(cnt), "#%lu", (unsigned long)lastMsg.count);
    textRight(126, 17, cnt);
  }
  drawDottedHLine(0, 19, 128);

  // Message body (y=21..53): auto-fit font & scroll
  u8g2.setFont(F_MED);
  int linesM = wrapText(0, 0, 120, 11, lastMsg.text, 0);
  if (linesM <= 3) {
    int y0 = 29 + ((3 - linesM) * 11) / 2;
    wrapText(2, y0, 124, 11, lastMsg.text, 3);
  } else {
    u8g2.setFont(F_SMALL);
    int linesS = wrapText(0, 0, 120, 8, lastMsg.text, 0);
    int skip = (linesS > 4 && !c.st[1]) ? min((int)(((millis() - c.t0) / 2400) % max(1, linesS - 2)), linesS - 4) : 0;
    wrapText(2, 27, 120, 8, lastMsg.text, 4, skip);
    if (linesS > 4) {
      int thumbH = max(5, 30 * 4 / linesS);
      int thumbY = 21 + (30 - thumbH) * skip / max(1, linesS - 4);
      u8g2.drawVLine(126, 21, 30);
      u8g2.drawBox(125, thumbY, 3, thumbH);
    }
  }

  // Footer (y=55..63)
  drawDottedHLine(0, 54, 128);
  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 62, "hold: clear");
  textRight(126, 62, "WhatsApp");
}

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
    s += lastMsg.from[0] ? lastMsg.from : "unknown";
    s += ": ";
    s += lastMsg.text;
  } else {
    s = "No WhatsApp messages.";
  }
}

const CardType CT_whatsapp = {
  "whatsapp", 0, 0, 0, nullptr, wa_draw, wa_input, nullptr, wa_speak, nullptr
};
