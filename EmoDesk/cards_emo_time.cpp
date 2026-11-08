#include "cards.h"
#include "services.h"
#include "net.h"

static bool needTime(struct tm& t) {
  if (nowTm(t)) return true;
  drawCenteredMsg("Syncing time...", netOnline() ? "waiting for NTP" : "needs Wi-Fi once");
  return false;
}

static void speakTime(CardCtx&, String& s) {
  struct tm t;
  if (!nowTm(t)) { s = "I do not know the time yet."; return; }
  s = "It is " + sayTime(t.tm_hour, t.tm_min) + ". " + WDAY_LONG[t.tm_wday] + ", " + MONTH_LONG[t.tm_mon] + " " +
      numWords(t.tm_mday) + ".";
}

static int daysInMonth(int y, int m) {   // m 0..11
  static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return d[m];
}

// ================================================================== emo
static void emo_draw(CardCtx& c) {
  if (inputHeldMs() > 600) emoPetting();
  emoRender(64, 30, 1.0f);
}
static bool emo_input(CardCtx& c, uint8_t g) {
  if (g == G_TRIPLE) { emoPoke(2); return true; }
  if (g == G_LONG) { emoPlay(); return true; }   // petting happens while holding
  return false;   // G_TAP -> Next Card, G_DOUBLE -> Prev Card
}
static void emo_speak(CardCtx&, String& s) { s = emoSpeech(); }
const CardType CT_emo = {"emo", 0, 0, 0, nullptr, emo_draw, emo_input, nullptr, emo_speak, nullptr};

// ================================================================== pet
static void pet_draw(CardCtx& c) {
  drawTitle(c, "Pet Care", "virtual pet");
  emoRender(20, 36, 0.45f);
  u8g2.setFont(F_TINY);
  static const char* const lab[3] = {"JOY", "NRG", "FED"};
  float v[3] = {pet.happy, pet.energy, 100 - pet.hunger};
  for (int i = 0; i < 3; i++) {
    int y = 14 + i * 13;
    u8g2.drawStr(44, y + 7, lab[i]);
    drawBar(64, y, 60, 8, v[i] / 100.0f);
  }
  u8g2.setFont(F_TINY);
  textCenter(62, "tap: next   |   hold: care");
}
static bool pet_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) { emoFeed(); emoPlay(); return true; }
  return false;
}
const CardType CT_pet = {"pet", 0, 0, 0, nullptr, pet_draw, pet_input, nullptr, emo_speak, nullptr};

// ================================================================== digital clock
struct DigitAnimSlot {
  char lastShown[16] = "";
  char prevShown[16] = "";
  uint32_t animStartTime = 0;
  bool hasPrior = false;
};
static DigitAnimSlot s_animSlots[4];

static void drawDigits(int slot, const char* str, int x, int y, int hClip, const char* animMode) {
  if (!str || !str[0]) return;
  if (!animMode || !strcmp(animMode, "none") || slot < 0 || slot >= 4) {
    u8g2.drawStr(x, y, str);
    return;
  }

  DigitAnimSlot& as = s_animSlots[slot];
  if (!as.hasPrior) {
    safeCopy(as.lastShown, str, sizeof(as.lastShown));
    safeCopy(as.prevShown, str, sizeof(as.prevShown));
    as.hasPrior = true;
    as.animStartTime = 0;
  }

  if (strcmp(str, as.lastShown) != 0) {
    safeCopy(as.prevShown, as.lastShown, sizeof(as.prevShown));
    safeCopy(as.lastShown, str, sizeof(as.lastShown));
    as.animStartTime = millis();
  }

  uint32_t elapsed = (as.animStartTime > 0) ? (millis() - as.animStartTime) : 999999;
  uint32_t animDur = 350;
  if (cfg["clock"]["dur"].is<uint32_t>()) {
    animDur = cfg["clock"]["dur"].as<uint32_t>();
    if (animDur < 100) animDur = 100;
    if (animDur > 1000) animDur = 1000;
  }

  if (elapsed >= animDur || as.prevShown[0] == '\0') {
    u8g2.drawStr(x, y, str);
    return;
  }

  float p = (float)elapsed / (float)animDur;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  float ease = p * p * (3.0f - 2.0f * p);

  size_t lenCur = strlen(str);
  size_t lenPrev = strlen(as.prevShown);

  if (lenCur != lenPrev) {
    int cY0 = (y - hClip < 0) ? 0 : (y - hClip);
    int cY1 = (y + 3 > 64) ? 64 : (y + 3);
    u8g2.setClipWindow(0, cY0, 128, cY1);
    int dy = (int)((float)hClip * ease);
    if (!strcmp(animMode, "slide_down")) {
      u8g2.drawStr(x, y + dy, as.prevShown);
      u8g2.drawStr(x, y - hClip + dy, str);
    } else {
      u8g2.drawStr(x, y - dy, as.prevShown);
      u8g2.drawStr(x, y + hClip - dy, str);
    }
    u8g2.setMaxClipWindow();
    return;
  }

  int curX = x;
  for (size_t i = 0; i < lenCur; i++) {
    char chCur[2] = { str[i], '\0' };
    char chPrev[2] = { as.prevShown[i], '\0' };
    int cw = u8g2.getStrWidth(chCur);

    if (str[i] == as.prevShown[i]) {
      u8g2.drawStr(curX, y, chCur);
    } else {
      int clipX0 = (curX - 1 < 0) ? 0 : (curX - 1);
      int clipX1 = (curX + cw + 2 > 128) ? 128 : (curX + cw + 2);
      int clipY0 = (y - hClip < 0) ? 0 : (y - hClip);
      int clipY1 = (y + 3 > 64) ? 64 : (y + 3);

      u8g2.setClipWindow(clipX0, clipY0, clipX1, clipY1);

      if (!strcmp(animMode, "slide_down")) {
        int dy = (int)((float)hClip * ease);
        u8g2.drawStr(curX, y + dy, chPrev);
        u8g2.drawStr(curX, y - hClip + dy, chCur);
      } else if (!strcmp(animMode, "flip")) {
        int midY = y - (hClip / 2);
        if (ease < 0.5f) {
          float sub = ease * 2.0f;
          int dy = (int)((float)(hClip / 2) * sub);
          u8g2.drawStr(curX, y - dy, chPrev);
        } else {
          float sub = (ease - 0.5f) * 2.0f;
          int dy = (int)((float)(hClip / 2) * (1.0f - sub));
          u8g2.drawStr(curX, y + dy, chCur);
        }
        u8g2.drawHLine(curX, midY, cw);
      } else if (!strcmp(animMode, "fade")) {
        u8g2.drawStr(curX, y, (ease < 0.5f) ? chPrev : chCur);
      } else if (!strcmp(animMode, "bounce")) {
        float be = 1.0f - cosf(ease * 3.14159f * 2.5f) * expf(-ease * 3.5f);
        int dy = (int)((float)hClip * (1.0f - be));
        u8g2.drawStr(curX, y - dy, chCur);
      } else {
        int dy = (int)((float)hClip * ease);
        u8g2.drawStr(curX, y - dy, chPrev);
        u8g2.drawStr(curX, y + hClip - dy, chCur);
      }

      u8g2.setMaxClipWindow();
    }
    curX += cw;
  }
}

static const char* const CLOCK_STYLES[] = {
  "normal",    // 0: Normal Ticker (NWatch)
  "scan",      // 1: Sci-Fi Scan (NWatch)
  "dual",      // 2: Dual Analog/Digital (NWatch)
  "pixel",     // 3: Big Pixel (NWatch)
  "minimal",   // 4: Minimalist (NWatch)
  "data",      // 5: Cyberpunk Data HUD (NWatch)
  "rain",      // 6: Digital Rain (NWatch)
  "matrix",    // 7: Matrix Rain (NWatch)
  "clock",     // 8: Numbered Dial (NWatch)
  "big",       // 9: Big Time (NWatch)
  "cube"       // 10: 3D Wireframe Cube (NWatch)
};
static const char* const CLOCK_STYLE_NAMES[] = {
  "1. Normal Ticker", "2. Sci-Fi Scan", "3. Dual Analog", "4. Big Pixel",
  "5. Minimalist", "6. Cyberpunk Data", "7. Digital Rain", "8. Matrix Rain",
  "9. Numbered Dial", "10. Big Time", "11. 3D Cube"
};
static const int NUM_CLOCK_STYLES = 11;

static void clock_draw(CardCtx& c) {
  struct tm t;
  int bx = burnX, by = burnY;
  if (!nowTm(t)) {
    u8g2.setFont(F_NUM32);
    textCenter(38 + by, "--:--");
    u8g2.setFont(F_MED);
    textCenter(56 + by, netOnline() ? "Getting the time..." : "Waiting for Wi-Fi");
    return;
  }

  // Initialize style from card params on first run, or respect touch switch
  if (c.st[1] == 0) {
    const char* defStyle = c.p["style"] | "normal";
    if (!strcmp(defStyle, "dial")) defStyle = "clock";
    else if (!strcmp(defStyle, "flip")) defStyle = "pixel";
    else if (!strcmp(defStyle, "seconds")) defStyle = "normal";

    for (int i = 0; i < NUM_CLOCK_STYLES; i++) {
      if (!strcmp(defStyle, CLOCK_STYLES[i])) { c.st[0] = i; break; }
    }
    c.st[1] = 1;
  }

  int styleIdx = c.st[0] % NUM_CLOCK_STYLES;
  const char* style = CLOCK_STYLES[styleIdx];
  const char* anim = c.p["anim"].is<const char*>() ? (c.p["anim"] | "slide") : (cfg["clock"]["anim"] | "slide");
  bool h24 = cfgH24();
  char hm[8], date[32];
  fmtHM(hm, t.tm_hour, t.tm_min, h24);
  snprintf(date, sizeof(date), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
  const char* ap = t.tm_hour < 12 ? "AM" : "PM";

  if (!strcmp(style, "normal")) {
    // ------------------------------------------------------------- 0. Normal Ticker (NWatch Zak Kemble)
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(4 + bx, 8 + by, date);

    if (homeW.ok) {
      char tb[10];
      snprintf(tb, sizeof(tb), "%d°", (int)roundf(homeW.temp));
      textRight(124 + bx, 8 + by, tb);
    } else {
      textRight(124 + bx, 8 + by, ap);
    }
    u8g2.drawHLine(0, 11 + by, 128);

    char sBuf[6];
    snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    u8g2.setFont(F_NUM24);
    int tw = u8g2.getStrWidth(hm);
    u8g2.setFont(F_NUM16);
    int sw = u8g2.getStrWidth(sBuf);
    int startX = (128 - (tw + sw + 2)) / 2 + bx;

    u8g2.setFont(F_NUM24);
    drawDigits(0, hm, startX, 39 + by, 24, anim);
    u8g2.setFont(F_NUM16);
    u8g2.drawStr(startX + tw + 2, 39 + by, sBuf);

    u8g2.drawHLine(0, 48 + by, 128);

    u8g2.setFont(F_TINY);
    if (netOnline()) {
      char netBuf[24];
      snprintf(netBuf, sizeof(netBuf), "WiFi %ddBm", netRSSI());
      u8g2.drawStr(4 + bx, 59 + by, netBuf);
    } else {
      u8g2.drawStr(4 + bx, 59 + by, "OFFLINE");
    }

    char statBuf[24];
    snprintf(statBuf, sizeof(statBuf), "HEAP %uK", (unsigned)(ESP.getFreeHeap() / 1024));
    textRight(124 + bx, 59 + by, statBuf);

    int secProg = (t.tm_sec * 128) / 60;
    if (secProg > 0) u8g2.drawHLine(0, 50 + by, secProg);

  } else if (!strcmp(style, "scan")) {
    // ------------------------------------------------------------- 1. Sci-Fi Scan (NWatch)
    uint32_t now = millis();
    int scanY = 14 + (now / 40) % 36;
    for (int y = 14; y < 50; y += 4) {
      if (abs(y - scanY) <= 3) u8g2.drawHLine(0, y + by, 128);
      else {
        for (int x = 0; x < 128; x += 6) u8g2.drawPixel(x, y + by);
      }
    }

    u8g2.setFont(F_NUM24);
    int tw = u8g2.getStrWidth(hm);
    int plateW = tw + 18;
    int plateX = (128 - plateW) / 2 + bx;
    u8g2.setDrawColor(0);
    u8g2.drawRBox(plateX - 2, 17 + by, plateW + 4, 30, 2);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(plateX - 2, 17 + by, plateW + 4, 30, 2);
    drawDigits(0, hm, plateX + 3, 39 + by, 24, anim);

    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(4 + bx, 8 + by, date);

    char sBuf[6];
    snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    u8g2.setFont(F_SMALL);
    textRight(124 + bx, 8 + by, sBuf);

    u8g2.drawHLine(0, 52 + by, 128);
    u8g2.setFont(F_TINY);
    textCenter(61 + by, "RADAR SCAN ACTIVE");

  } else if (!strcmp(style, "dual")) {
    // ------------------------------------------------------------- 2. Dual Analog/Digital (NWatch)
    int cx = 32 + bx, cy = 32 + by, r = 26;
    u8g2.drawCircle(cx, cy, r);
    for (int h = 0; h < 12; h++) {
      float ang = h * 6.2831853f / 12.0f - 1.5708f;
      int d = (h % 3 == 0) ? 22 : 23;
      u8g2.drawPixel(cx + lroundf(cosf(ang) * d), cy + lroundf(sinf(ang) * d));
    }

    float secAng = t.tm_sec * 6.2831853f / 60.0f - 1.5708f;
    float minAng = (t.tm_min + t.tm_sec / 60.0f) * 6.2831853f / 60.0f - 1.5708f;
    float hrAng  = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 6.2831853f / 12.0f - 1.5708f;

    u8g2.drawLine(cx, cy, cx + lroundf(cosf(hrAng) * 12), cy + lroundf(sinf(hrAng) * 12));
    u8g2.drawLine(cx, cy, cx + lroundf(cosf(minAng) * 19), cy + lroundf(sinf(minAng) * 19));
    u8g2.drawLine(cx, cy, cx + lroundf(cosf(secAng) * 22), cy + lroundf(sinf(secAng) * 22));
    u8g2.drawDisc(cx, cy, 2);

    u8g2.setFont(F_SMALL);
    char dShort[24];
    snprintf(dShort, sizeof(dShort), "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
    u8g2.drawUTF8(68 + bx, 14 + by, dShort);

    u8g2.setFont(F_HELV14);
    u8g2.drawStr(68 + bx, 35 + by, hm);

    char sLine[16];
    snprintf(sLine, sizeof(sLine), "SEC :%02d", t.tm_sec);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(68 + bx, 48 + by, sLine);

    u8g2.setFont(F_TINY);
    if (homeW.ok) {
      char wBuf[16];
      snprintf(wBuf, sizeof(wBuf), "%d°C", (int)roundf(homeW.temp));
      u8g2.drawStr(68 + bx, 61 + by, wBuf);
      textRight(124 + bx, 61 + by, ap);
    } else {
      u8g2.drawStr(68 + bx, 61 + by, ap);
    }

  } else if (!strcmp(style, "pixel")) {
    // ------------------------------------------------------------- 3. Big Pixel (NWatch)
    int h = t.tm_hour;
    if (!h24) { h %= 12; if (!h) h = 12; }
    char hStr[6], mStr[6];
    snprintf(hStr, sizeof(hStr), "%02d", h);
    snprintf(mStr, sizeof(mStr), "%02d", t.tm_min);

    u8g2.setFont(F_SMALL);
    char dateHdr[24];
    snprintf(dateHdr, sizeof(dateHdr), "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
    u8g2.drawUTF8(4 + bx, 8 + by, dateHdr);

    char secPill[8];
    snprintf(secPill, sizeof(secPill), ":%02d", t.tm_sec);
    drawPill(98 + bx, 1 + by, 26, 9, secPill, true);

    u8g2.setFont(F_NUM32);
    drawDigits(1, hStr, 12 + bx, 44 + by, 32, anim);
    drawDigits(2, mStr, 72 + bx, 44 + by, 32, anim);

    if (t.tm_sec % 2 == 0) {
      u8g2.drawBox(58 + bx, 26 + by, 4, 4);
      u8g2.drawBox(58 + bx, 37 + by, 4, 4);
    }

    drawBar(10 + bx, 50 + by, 108, 4, t.tm_sec / 60.0f);

    char yFoot[16];
    snprintf(yFoot, sizeof(yFoot), "%d  %s", t.tm_year + 1900, ap);
    u8g2.setFont(F_TINY);
    textCenter(62 + by, yFoot);

  } else if (!strcmp(style, "minimal")) {
    // ------------------------------------------------------------- 4. Minimalist (NWatch)
    u8g2.setFont(F_NUM32);
    int tw = u8g2.getStrWidth(hm);
    u8g2.setFont(F_SMALL);
    char sBuf[6];
    snprintf(sBuf, sizeof(sBuf), "%02d", t.tm_sec);
    int sw = u8g2.getStrWidth(sBuf);

    int totalW = tw + sw + 3;
    int startX = (128 - totalW) / 2 + bx;
    u8g2.setFont(F_NUM32);
    drawDigits(0, hm, startX, 36 + by, 32, anim);

    u8g2.setFont(F_SMALL);
    u8g2.drawStr(startX + tw + 3, 36 + by, sBuf);

    u8g2.drawHLine(24 + bx, 44 + by, 80);

    u8g2.setFont(F_MED);
    textCenter(58 + by, date);

  } else if (!strcmp(style, "data")) {
    // ------------------------------------------------------------- 5. Cyberpunk Data HUD (NWatch)
    u8g2.setFont(F_TINY);
    u8g2.drawStr(2 + bx, 7 + by, "ESP32-C3 SUPER MINI");
    if (netOnline()) {
      char rssiBuf[16];
      snprintf(rssiBuf, sizeof(rssiBuf), "SIG %ddBm", netRSSI());
      textRight(126 + bx, 7 + by, rssiBuf);
    } else {
      textRight(126 + bx, 7 + by, "OFFLINE");
    }
    u8g2.drawHLine(0, 9 + by, 128);

    char timeLine[24];
    snprintf(timeLine, sizeof(timeLine), "TIME  %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(2 + bx, 21 + by, timeLine);

    char dateLine[28];
    snprintf(dateLine, sizeof(dateLine), "DATE  %s %02d/%02d/%04d", WDAY[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    u8g2.drawStr(2 + bx, 33 + by, dateLine);

    if (homeW.ok) {
      char wLine[32];
      const char* tu = (cfg["units"] == "imperial") ? "F" : "C";
      snprintf(wLine, sizeof(wLine), "ENV   %d%s  %s", (int)roundf(homeW.temp), tu, wmoText(homeW.code));
      u8g2.drawStr(2 + bx, 45 + by, wLine);
    } else {
      u8g2.drawStr(2 + bx, 45 + by, "ENV   WAITING FOR SYNC");
    }

    u8g2.drawHLine(0, 49 + by, 128);
    char memLine[28];
    snprintf(memLine, sizeof(memLine), "FREE  %uKB", (unsigned)(ESP.getFreeHeap() / 1024));
    u8g2.setFont(F_TINY);
    u8g2.drawStr(2 + bx, 61 + by, memLine);
    drawBar(70 + bx, 54 + by, 56, 6, t.tm_sec / 60.0f);

  } else if (!strcmp(style, "rain")) {
    // ------------------------------------------------------------- 6. Digital Rain (NWatch)
    uint32_t now = millis();
    for (int col = 2; col < 126; col += 7) {
      int speed = 25 + (col * 17) % 35;
      int yHead = (int)((now / speed) + col * 11) % 80 - 10;
      for (int k = 0; k < 4; k++) {
        int py = yHead - k * 5 + by;
        if (py >= 0 && py < 64) {
          if (k == 0) u8g2.drawBox(col + bx, py, 2, 2);
          else u8g2.drawPixel(col + bx, py);
        }
      }
    }

    char sBuf[6];
    snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    u8g2.setFont(F_NUM24);
    int tw = u8g2.getStrWidth(hm);
    u8g2.setFont(F_SMALL);
    int sw = u8g2.getStrWidth(sBuf);
    int plateW = tw + sw + 16;
    int plateX = (128 - plateW) / 2 + bx;

    u8g2.setDrawColor(0);
    u8g2.drawRBox(plateX - 2, 16 + by, plateW + 4, 32, 3);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(plateX - 2, 16 + by, plateW + 4, 32, 3);

    u8g2.setFont(F_NUM24);
    drawDigits(0, hm, plateX + 5, 39 + by, 24, anim);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(plateX + tw + 7, 39 + by, sBuf);

    u8g2.setFont(F_TINY);
    textCenter(61 + by, date);

  } else if (!strcmp(style, "matrix")) {
    // ------------------------------------------------------------- 7. Matrix Rain (NWatch)
    uint32_t now = millis();
    static const int8_t laneX[10] = {4, 16, 28, 42, 54, 68, 80, 94, 106, 118};
    static const uint8_t laneSpeed[10] = {29, 53, 37, 71, 43, 31, 67, 47, 83, 59};
    for (int lane = 0; lane < 10; lane++) {
      uint32_t tick = now / laneSpeed[lane];
      int head = (int)(tick * 3) % 80 - 16;
      for (int n = 0; n < 6; n++) {
        int y = head - n * 7 + by;
        if (y >= 0 && y < 64) {
          int x = laneX[lane] + bx;
          if (n == 0) u8g2.drawBox(x, y, 2, 3);
          else u8g2.drawPixel(x, y);
        }
      }
    }

    u8g2.setFont(F_NUM24);
    int tw = u8g2.getStrWidth(hm);
    int plateW = tw + 28;
    int plateX = (128 - plateW) / 2 + bx;
    u8g2.setDrawColor(0);
    u8g2.drawRBox(plateX - 2, 14 + by, plateW + 4, 34, 3);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(plateX - 2, 14 + by, plateW + 4, 34, 3);

    drawDigits(0, hm, plateX + 3, 40 + by, 24, anim);
    u8g2.setFont(F_SMALL);
    char secStr[6];
    snprintf(secStr, sizeof(secStr), "%02d", t.tm_sec);
    u8g2.drawStr(plateX + tw + 6, 28 + by, secStr);
    u8g2.setFont(F_TINY);
    u8g2.drawUTF8(plateX + tw + 6, 38 + by, ap);

    u8g2.setFont(F_TINY);
    textCenter(60 + by, date);

  } else if (!strcmp(style, "clock")) {
    // ------------------------------------------------------------- 8. Numbered Dial (NWatch)
    int cx = 64 + bx, cy = 32 + by, r = 30;
    u8g2.drawCircle(cx, cy, r);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(cx - 5, cy - 21, "12");
    u8g2.drawStr(cx + 21, cy + 3, "3");
    u8g2.drawStr(cx - 3, cy + 27, "6");
    u8g2.drawStr(cx - 26, cy + 3, "9");

    for (int mark = 1; mark < 12; mark++) {
      if (mark != 3 && mark != 6 && mark != 9) {
        float q = mark * 6.2831853f / 12.0f - 1.5708f;
        u8g2.drawPixel(cx + lroundf(cosf(q) * 26), cy + lroundf(sinf(q) * 26));
      }
    }

    float sec = t.tm_sec * 6.2831853f / 60.0f - 1.5708f;
    float minu = (t.tm_min + t.tm_sec / 60.0f) * 6.2831853f / 60.0f - 1.5708f;
    float hour = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 6.2831853f / 12.0f - 1.5708f;

    u8g2.drawLine(cx, cy, cx + lroundf(cosf(hour) * 14), cy + lroundf(sinf(hour) * 14));
    u8g2.drawLine(cx, cy, cx + lroundf(cosf(minu) * 21), cy + lroundf(sinf(minu) * 21));
    u8g2.drawLine(cx, cy, cx + lroundf(cosf(sec) * 25), cy + lroundf(sinf(sec) * 25));
    u8g2.drawLine(cx, cy, cx - lroundf(cosf(sec) * 6), cy - lroundf(sinf(sec) * 6));
    u8g2.drawDisc(cx, cy, 2);

    u8g2.setFont(F_TINY);
    u8g2.drawUTF8(2 + bx, 35 + by, WDAY[t.tm_wday]);
    char dStr[6];
    snprintf(dStr, sizeof(dStr), "%d", t.tm_mday);
    textRight(126 + bx, 35 + by, dStr);

  } else if (!strcmp(style, "cube")) {
    // ------------------------------------------------------------- 10. 3D Rotating Cube (NWatch)
    float ax = millis() * 0.00085f, ay = millis() * 0.00105f, az = millis() * 0.00065f;
    float cax = cosf(ax), sax = sinf(ax), cay = cosf(ay), say = sinf(ay), caz = cosf(az), saz = sinf(az);
    int sx[8], sy[8];
    for (int i = 0; i < 8; i++) {
      float x = (i & 1) ? 1.0f : -1.0f, y = (i & 2) ? 1.0f : -1.0f, z = (i & 4) ? 1.0f : -1.0f;
      float y1 = y * cax - z * sax, z1 = y * sax + z * cax;
      float x2 = x * cay + z1 * say, z2 = -x * say + z1 * cay;
      float x3 = x2 * caz - y1 * saz, y3 = x2 * saz + y1 * caz;
      float scale = 54.0f / (z2 + 4.0f);
      sx[i] = 34 + bx + lroundf(x3 * scale);
      sy[i] = 32 + by + lroundf(y3 * scale);
    }
    for (int i = 0; i < 8; i++) {
      for (int bit = 0; bit < 3; bit++) {
        int j = i ^ (1 << bit);
        if (i < j) u8g2.drawLine(sx[i], sy[i], sx[j], sy[j]);
      }
    }

    int h = t.tm_hour;
    if (!h24) { h %= 12; if (!h) h = 12; }
    char hStr[6], mStr[6];
    snprintf(hStr, sizeof(hStr), "%02d", h);
    snprintf(mStr, sizeof(mStr), "%02d", t.tm_min);
    u8g2.setFont(F_NUM24);
    drawDigits(1, hStr, 78 + bx, 27 + by, 24, anim);
    drawDigits(2, mStr, 78 + bx, 56 + by, 24, anim);

    char sBuf[12];
    snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(112 + bx, 56 + by, sBuf);
    u8g2.drawUTF8(2 + bx, 8 + by, WDAY[t.tm_wday]);

  } else {
    // ------------------------------------------------------------- 9. Big Time (NWatch)
    int h = t.tm_hour;
    if (!h24) { h %= 12; if (!h) h = 12; }
    char b[8];
    snprintf(b, sizeof(b), h24 ? "%02d:%02d" : "%d:%02d", h, t.tm_min);

    u8g2.setFont(F_NUM42);
    int w = u8g2.getStrWidth(b);
    int x = (128 - w) / 2 + bx;
    if (x < 0) x = 0;
    drawDigits(0, b, x, 46 + by, 42, anim);

    int sw = (t.tm_sec * 128) / 60;
    if (sw > 0) u8g2.drawHLine(0, 50 + by, sw);

    char fullDate[36];
    u8g2.setFont(F_MED);
    if (!h24) {
      snprintf(fullDate, sizeof(fullDate), "%s %d %s  %s", WDAY_LONG[t.tm_wday], t.tm_mday, MONTH[t.tm_mon], ap);
      if (u8g2.getStrWidth(fullDate) > 126) {
        snprintf(fullDate, sizeof(fullDate), "%s, %d %s  %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon], ap);
      }
    } else {
      snprintf(fullDate, sizeof(fullDate), "%s %d %s", WDAY_LONG[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
      if (u8g2.getStrWidth(fullDate) > 126) {
        snprintf(fullDate, sizeof(fullDate), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
      }
    }
    textCenter(62 + by, fullDate);

    if (homeW.ok) {
      char tb[10];
      snprintf(tb, sizeof(tb), "%d°", (int)roundf(homeW.temp));
      u8g2.setFont(F_TINY);
      u8g2.drawUTF8(1 + bx, 6 + by, tb);
    }
  }
}

static bool clock_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) {
    c.st[0] = (c.st[0] + 1) % NUM_CLOCK_STYLES;
    c.st[1] = 1;
    toastShow(CLOCK_STYLE_NAMES[c.st[0] % NUM_CLOCK_STYLES]);
    audioSfx("pop", EV_UI);
    return true;
  }
  if (g == G_TRIPLE) {
    c.st[0] = (c.st[0] + NUM_CLOCK_STYLES - 1) % NUM_CLOCK_STYLES;
    c.st[1] = 1;
    toastShow(CLOCK_STYLE_NAMES[c.st[0] % NUM_CLOCK_STYLES]);
    audioSfx("pop", EV_UI);
    return true;
  }
  return false;
}

const CardType CT_clock = {"clock", 0, 0, 0, nullptr, clock_draw, clock_input, nullptr, speakTime, nullptr};

// ================================================================== analog clock
static void hand(int cx, int cy, float deg, int len, bool thick) {
  float a = (deg - 90) * PI / 180.0f;
  int x = cx + (int)(cosf(a) * len), y = cy + (int)(sinf(a) * len);
  u8g2.drawLine(cx, cy, x, y);
  if (thick) { u8g2.drawLine(cx + 1, cy, x + 1, y); u8g2.drawLine(cx, cy + 1, x, y + 1); }
}
static void analog_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  int cx = 32 + burnX, cy = 32 + burnY, r = 30;
  u8g2.drawCircle(cx, cy, r);
  for (int i = 0; i < 12; i++) {
    float a = i * 30 * PI / 180.0f;
    int len = i % 3 == 0 ? 5 : 2;
    u8g2.drawLine(cx + sinf(a) * (r - len), cy - cosf(a) * (r - len), cx + sinf(a) * (r - 1), cy - cosf(a) * (r - 1));
  }
  float s = t.tm_sec, m = t.tm_min + s / 60.0f, h = (t.tm_hour % 12) + m / 60.0f;
  hand(cx, cy, h * 30, 15, true);
  hand(cx, cy, m * 6, 23, true);
  hand(cx, cy, s * 6, 27, false);
  u8g2.drawDisc(cx, cy, 2);
  u8g2.setFont(F_HELV14);
  u8g2.drawStr(72, 22, WDAY[t.tm_wday]);
  char b[16];
  snprintf(b, sizeof(b), "%d %s", t.tm_mday, MONTH[t.tm_mon]);
  u8g2.setFont(F_MED);
  u8g2.drawStr(72, 38, b);
  fmtHM(b, t.tm_hour, t.tm_min, cfgH24());
  u8g2.drawStr(72, 54, b);
}
const CardType CT_analog = {"analog", 0, 0, 0, nullptr, analog_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== binary clock
static void binary_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  int digits[6] = {t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10, t.tm_sec / 10, t.tm_sec % 10};
  int bits[6] = {2, 4, 3, 4, 3, 4};
  static const char* const lab = "HHMMSS";
  for (int col = 0; col < 6; col++) {
    int x = 14 + col * 18 + (col / 2) * 6 + burnX;
    for (int b = 0; b < 4; b++) {
      int y = 50 - b * 12 + burnY;
      if (b >= bits[col]) continue;
      if (digits[col] & (1 << b)) u8g2.drawDisc(x, y, 4);
      else u8g2.drawCircle(x, y, 4);
    }
    char l[2] = {lab[col], 0};
    u8g2.setFont(F_TINY);
    u8g2.drawStr(x - 1, 63, l);
  }
}
const CardType CT_binary = {"binary", 0, 0, 0, nullptr, binary_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== word clock
static void wordclock_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  static const char* const H[] = {"twelve", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven"};
  static const char* const M[] = {"", "five past", "ten past", "quarter past", "twenty past", "twenty-five past", "half past",
                                  "twenty-five to", "twenty to", "quarter to", "ten to", "five to"};
  int idx = ((t.tm_min + 2) / 5) % 12;
  int h = t.tm_hour + ((t.tm_min + 2) / 5 >= 12 ? 1 : 0) + (idx > 6 ? 1 : 0);
  char s[64];
  if (idx == 0) snprintf(s, sizeof(s), "It's %s o'clock", H[h % 12]);
  else snprintf(s, sizeof(s), "It's %s %s", M[idx], H[h % 12]);
  u8g2.setFont(F_HELV14);
  int lines = wrapText(0, 0, 120, 17, s, 0);   // count only
  int y0 = 32 - lines * 17 / 2 + 13;
  wrapText(4 + burnX, y0 + burnY, 120, 17, s, 4);
}
const CardType CT_wordclock = {"wordclock", 0, 0, 0, nullptr, wordclock_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== calendar
static void calendar_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "%s %d", MONTH_LONG[t.tm_mon], t.tm_year + 1900);
  u8g2.setFont(F_SMALL);
  textCenter(7, hdr);
  bool mon = cfg["time"]["weekMon"] | true;
  const char* dn = mon ? "MoTuWeThFrSaSu" : "SuMoTuWeThFrSa";
  u8g2.setFont(F_TINY);
  for (int i = 0; i < 7; i++) {
    char b[3] = {dn[i * 2], dn[i * 2 + 1], 0};
    u8g2.drawStr(4 + i * 18, 15, b);
  }
  int firstW = ((t.tm_wday - (t.tm_mday - 1)) % 7 + 7) % 7;
  int off = mon ? (firstW + 6) % 7 : firstW;
  int dim = daysInMonth(t.tm_year + 1900, t.tm_mon);
  u8g2.setFont(F_SMALL);
  for (int d = 1; d <= dim; d++) {
    int pos = off + d - 1, row = pos / 7, col = pos % 7;
    int x = 1 + col * 18, y = 23 + row * 8;
    if (y > 63) break;
    char b[3];
    snprintf(b, sizeof(b), "%d", d);
    if (d == t.tm_mday) { u8g2.drawBox(x, y - 7, 16, 8); u8g2.setDrawColor(0); }
    u8g2.drawStr(x + (d < 10 ? 6 : 3), y, b);
    u8g2.setDrawColor(1);
  }
}
static void calendar_speak(CardCtx&, String& s) {
  struct tm t;
  if (!nowTm(t)) return;
  s = "Today is " + String(WDAY_LONG[t.tm_wday]) + ", " + MONTH_LONG[t.tm_mon] + " " + numWords(t.tm_mday) + ", " +
      numWords(t.tm_year + 1900) + ".";
}
const CardType CT_calendar = {"calendar", 0, 0, 0, nullptr, calendar_draw, nullptr, nullptr, calendar_speak, nullptr};

// ================================================================== world clock
static void world_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  JsonArray a = c.p["cities"];
  if (a.isNull() || !a.size()) { drawCenteredMsg("No cities yet", "Add them in the dashboard"); return; }
  static struct tm cache[4];
  static uint32_t calcAt = 0;
  static int calcIdx = -1;
  int n = min((int)a.size(), 4);
  if (millis() - calcAt > 1000 || calcIdx != c.idx) {
    calcAt = millis();
    calcIdx = c.idx;
    time_t now = time(nullptr);
    for (int i = 0; i < n; i++) localIn(a[i]["tz"] | "UTC0", now, cache[i]);
  }
  int rowH = 64 / n;
  bool h24 = cfgH24();
  for (int i = 0; i < n; i++) {
    int y = i * rowH + rowH / 2 + 5;
    u8g2.setFont(F_MED);
    u8g2.drawUTF8(2, y, a[i]["n"] | "?");
    char hm[8];
    fmtHM(hm, cache[i].tm_hour, cache[i].tm_min, h24);
    u8g2.setFont(F_BOLD);
    textRight(126, y + 1, hm);
    int dd = cache[i].tm_yday - t.tm_yday;
    if (dd) {
      u8g2.setFont(F_TINY);
      u8g2.drawStr(126 - u8g2.getStrWidth("+1") - 44, y - 3, dd > 0 ? "+1" : "-1");
    }
    if (i) u8g2.drawHLine(0, i * rowH, 128);
  }
}
const CardType CT_world = {"world", 0, 0, 0, nullptr, world_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== progress
static float progs[4];
static void calcProgress() {
  struct tm t;
  if (!nowTm(t)) return;
  int y = t.tm_year + 1900;
  float min = t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.0f;
  int diy = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
  progs[0] = min / 1440.0f;
  progs[1] = (((t.tm_wday + 6) % 7) * 1440 + min) / (7 * 1440.0f);
  progs[2] = ((t.tm_mday - 1) * 1440 + min) / (daysInMonth(y, t.tm_mon) * 1440.0f);
  progs[3] = (t.tm_yday * 1440 + min) / (diy * 1440.0f);
}
static void progress_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  calcProgress();
  static const char* const L[] = {"Day", "Week", "Month", "Year"};
  for (int i = 0; i < 4; i++) {
    int y0 = 2 + i * 16;
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(0, y0 + 8, L[i]);
    drawBar(32, y0, 70, 11, progs[i]);
    char b[6];
    snprintf(b, sizeof(b), "%d%%", (int)(progs[i] * 100));
    textRight(127, y0 + 8, b);
  }
}
static void progress_speak(CardCtx&, String& s) {
  calcProgress();
  s = "The day is " + String((int)(progs[0] * 100)) + " percent done. The year is " + String((int)(progs[3] * 100)) +
      " percent done.";
}
const CardType CT_progress = {"progress", 0, 0, 0, nullptr, progress_draw, nullptr, nullptr, progress_speak, nullptr};

// ================================================================== sun & moon
static float moonAge() {
  double days = (time(nullptr) - 947182440.0) / 86400.0;   // since new moon 2000-01-06 18:14 UTC
  double a = fmod(days, 29.530588853);
  return a < 0 ? a + 29.530588853 : a;
}
static const char* moonName(float age) {
  static const char* const N[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous", "Full moon",
                                  "Waning gibbous", "Last quarter", "Waning crescent"};
  return N[(int)((age / 29.530588853f) * 8 + 0.5f) % 8];
}
static void sunmoon_draw(CardCtx& c) {
  float age = moonAge(), p = age / 29.530588853f;
  int cx = 104, cy = 26, r = 14;
  for (int y = -r; y <= r; y++) {
    float w = sqrtf(r * r - y * y);
    float k = cosf(2 * PI * p);
    int x0, x1;
    if (p < 0.5f) { x0 = (int)(w * k); x1 = (int)w; }
    else { x0 = -(int)w; x1 = (int)(-w * k); }
    if (x1 > x0) u8g2.drawHLine(cx + x0, cy + y, x1 - x0 + 1);
  }
  u8g2.drawCircle(cx, cy, r);
  u8g2.setFont(F_TINY);
  const char* nm = moonName(age);
  u8g2.drawStr(127 - u8g2.getStrWidth(nm), 50, nm);
  char b[24];
  snprintf(b, sizeof(b), "%d%% lit", (int)((1 - cosf(2 * PI * p)) * 50));
  u8g2.drawStr(127 - u8g2.getStrWidth(b), 58, b);
  drawWeatherIcon(2, 2, 18, 0, true);
  u8g2.setFont(F_MED);
  if (!homeW.ok) { u8g2.drawStr(2, 36, "Set location"); u8g2.drawStr(2, 48, "for sun times"); return; }
  snprintf(b, sizeof(b), "Rise %s", homeW.sunrise);
  u8g2.drawStr(24, 12, b);
  snprintf(b, sizeof(b), "Set  %s", homeW.sunset);
  u8g2.drawStr(24, 24, b);
  int sr = parseHM(homeW.sunrise), ss = parseHM(homeW.sunset);
  if (sr >= 0 && ss > sr) {
    snprintf(b, sizeof(b), "Day %dh %02dm", (ss - sr) / 60, (ss - sr) % 60);
    u8g2.drawStr(2, 42, b);
  }
  snprintf(b, sizeof(b), "UV max %.0f", homeW.uv);
  u8g2.drawStr(2, 56, b);
}
static void sunmoon_speak(CardCtx&, String& s) {
  s = String("The moon is ") + moonName(moonAge()) + ".";
  if (homeW.ok) s += String(" Sunrise ") + homeW.sunrise + ", sunset " + homeW.sunset + ".";
}
const CardType CT_sunmoon = {"sunmoon", 0, 0, 0, nullptr, sunmoon_draw, nullptr, nullptr, sunmoon_speak, nullptr};

// ================================================================== countdown / days since
static time_t paramDate(JsonObject p, const char* defTime) {
  int y = 0, m = 0, d = 0, hh = 0, mm = 0;
  if (sscanf(p["date"] | "", "%d-%d-%d", &y, &m, &d) != 3) return 0;
  sscanf(p["time"] | defTime, "%d:%d", &hh, &mm);
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = hh; t.tm_min = mm; t.tm_isdst = -1;
  return mktime(&t);
}
static void countdown_draw(CardCtx& c) {
  time_t tgt = paramDate(c.p, "00:00");
  u8g2.setFont(F_MED);
  String title = c.p["title"] | "Countdown";
  textCenter(10, title.c_str());
  if (!tgt || !timeOK()) { drawCenteredMsg(tgt ? "Syncing time..." : "Set a date in the dashboard"); return; }
  long diff = (long)(tgt - time(nullptr));
  char b[24];
  if (diff > 86400) {
    snprintf(b, sizeof(b), "%ld", diff / 86400);
    u8g2.setFont(F_NUM32);
    int w = u8g2.getStrWidth(b);
    u8g2.drawStr((128 - w) / 2 - 12 + burnX, 47, b);
    u8g2.setFont(F_MED);
    u8g2.drawStr((128 + w) / 2 - 8 + burnX, 47, diff / 86400 == 1 ? "day" : "days");
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", (diff % 86400) / 3600, (diff % 3600) / 60, diff % 60);
    textCenter(62, b);
  } else if (diff > 0) {
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", diff / 3600, (diff % 3600) / 60, diff % 60);
    u8g2.setFont(F_NUM24);
    int w = u8g2.getStrWidth(b);
    u8g2.drawStr((128 - w) / 2, 46, b);
  } else {
    u8g2.setFont(F_HELV14);
    textCenter(40, diff > -86400 ? "It's here!" : "Done");
    snprintf(b, sizeof(b), "%ld days ago", -diff / 86400);
    u8g2.setFont(F_SMALL);
    if (diff <= -86400) textCenter(56, b);
  }
}
static void countdown_tick(CardCtx& c) {
  if (millis() - c.st[1] < 1000) return;
  c.st[1] = millis();
  time_t tgt = paramDate(c.p, "00:00");
  if (!tgt || !timeOK() || !(c.p["alert"] | true)) return;
  long diff = (long)(tgt - time(nullptr));
  if (diff <= 0 && diff > -120 && c.st[0] != (uint32_t)tgt) {
    c.st[0] = (uint32_t)tgt;
    String title = c.p["title"] | "Countdown";
    notify(title.c_str(), "The moment is here!", E_EXCITED, "sfx:win", EV_NOTIFY, true, 15);
  }
}
static void countdown_speak(CardCtx& c, String& s) {
  time_t tgt = paramDate(c.p, "00:00");
  if (!tgt) return;
  long d = (long)(tgt - time(nullptr)) / 86400;
  s = numWords(d) + " days until " + String(c.p["title"] | "the countdown") + ".";
}
const CardType CT_countdown = {"countdown", 0, 0, 0, nullptr, countdown_draw, nullptr, nullptr, countdown_speak, countdown_tick};

static void dayssince_draw(CardCtx& c) {
  time_t from = paramDate(c.p, "00:00");
  u8g2.setFont(F_MED);
  String title = c.p["title"] | "Days since";
  textCenter(10, title.c_str());
  if (!from || !timeOK()) { drawCenteredMsg(from ? "Syncing time..." : "Set a date in the dashboard"); return; }
  long d = (long)(time(nullptr) - from) / 86400;
  char b[12];
  snprintf(b, sizeof(b), "%ld", d);
  u8g2.setFont(F_NUM32);
  int w = u8g2.getStrWidth(b);
  u8g2.drawStr((128 - w) / 2 + burnX, 50, b);
  u8g2.setFont(F_SMALL);
  textCenter(62, d == 1 ? "day" : "days");
}
static void dayssince_speak(CardCtx& c, String& s) {
  time_t from = paramDate(c.p, "00:00");
  if (!from) return;
  s = String(c.p["title"] | "Days since") + ": " + numWords((long)(time(nullptr) - from) / 86400) + " days.";
}
const CardType CT_dayssince = {"dayssince", 0, 0, 0, nullptr, dayssince_draw, nullptr, nullptr, dayssince_speak, nullptr};
