#include "cards.h"
#include "services.h"
#include "net.h"

static bool needTime(struct tm& t) {
  if (nowTm(t)) return true;
  drawCenteredMsg("Syncing time...", netOnline() ? "Waiting for NTP" : "Needs Wi-Fi once");
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
  emoRender(64, 32, 1.0f);
}
static bool emo_input(CardCtx& c, uint8_t g) {
  if (g == G_TRIPLE) { emoPoke(2); return true; }
  if (g == G_LONG) { emoPlay(); return true; }
  return false;
}
static void emo_speak(CardCtx&, String& s) { s = emoSpeech(); }
const CardType CT_emo = {"emo", 0, 0, 0, nullptr, emo_draw, emo_input, nullptr, emo_speak, nullptr};

// ================================================================== pet
static void pet_draw(CardCtx& c) {
  drawTitle(c, "Pet Care", "hold: care");
  u8g2.drawRFrame(1, 13, 44, 49, 4);
  emoRender(23, 33, 0.42f);
  char pts[16];
  snprintf(pts, sizeof(pts), "%lu pts", (unsigned long)pet.interactions);
  u8g2.setFont(F_TINY);
  drawTextFitCenter(23, 59, 40, pts);

  static const char* const lab[3] = {"JOY", "NRG", "FED"};
  float v[3] = {pet.happy, pet.energy, 100.0f - pet.hunger};
  for (int i = 0; i < 3; i++) {
    int y0 = 13 + i * 17;
    int pct = constrain((int)roundf(v[i]), 0, 100);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(49, y0 + 6, lab[i]);
    char pb[8];
    snprintf(pb, sizeof(pb), "%d%%", pct);
    textRight(126, y0 + 6, pb);
    drawBar(49, y0 + 8, 77, 6, pct / 100.0f);
  }
}
static bool pet_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) { emoFeed(); emoPlay(); return true; }
  return false;
}
const CardType CT_pet = {"pet", 0, 0, 0, nullptr, pet_draw, pet_input, nullptr, emo_speak, nullptr};

// ================================================================== clock LUTs & digit animations
struct DigitAnimSlot {
  char lastShown[16] = "";
  char prevShown[16] = "";
  uint32_t animStartTime = 0;
  bool hasPrior = false;
};
static DigitAnimSlot s_animSlots[4];

// 32-step sine LUT (-28..+28) for smooth waves and 3D cube
static const int8_t FAST_SIN[32] = {
  0,  6, 12, 18, 22, 25, 27, 28,
 28, 27, 25, 22, 18, 12,  6,  0,
 -6,-12,-18,-22,-25,-27,-28,-28,
-28,-27,-25,-22,-18,-12, -6,  0
};
static inline int fastSin(uint8_t step) { return FAST_SIN[step & 31]; }
static inline int fastCos(uint8_t step) { return FAST_SIN[(step + 8) & 31]; }

// Exact 60-step sine table (0=12 o'clock, clockwise, range -127..127)
static const int8_t SIN60[60] = {
    0,  13,  26,  39,  52,  64,  75,  85,  94, 103, 110, 116, 121, 124, 126,
  127, 126, 124, 121, 116, 110, 103,  94,  85,  75,  64,  52,  39,  26,  13,
    0, -13, -26, -39, -52, -64, -75, -85, -94,-103,-110,-116,-121,-124,-126,
 -127,-126,-124,-121,-116,-110,-103, -94, -85, -75, -64, -52, -39, -26, -13
};
static inline int sin60(int step) { int i = (step % 60 + 60) % 60; return SIN60[i]; }
static inline int cos60(int step) { int i = ((step + 15) % 60 + 60) % 60; return SIN60[i]; }
static inline int dialX(int cx, int r, int step60) { return cx + (sin60(step60) * r + (sin60(step60) >= 0 ? 63 : -63)) / 127; }
static inline int dialY(int cy, int r, int step60) { return cy - (cos60(step60) * r + (cos60(step60) >= 0 ? 63 : -63)) / 127; }

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
  const uint32_t animDur = 280;
  if (elapsed >= animDur || as.prevShown[0] == '\0') {
    u8g2.drawStr(x, y, str);
    return;
  }

  float p = constrain((float)elapsed / (float)animDur, 0.0f, 1.0f);
  float ease = p * p * (3.0f - 2.0f * p);
  size_t lenCur = strlen(str), lenPrev = strlen(as.prevShown);
  int padDown = (hClip >= 30) ? 7 : 5;

  if (lenCur != lenPrev) {
    int cY0 = constrain(y - hClip, 0, 63);
    int cY1 = constrain(y + padDown, cY0 + 1, 64);
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
      int cx0 = constrain(curX - 1, 0, 126);
      int cx1 = constrain(curX + cw + 2, cx0 + 1, 128);
      int cy0 = constrain(y - hClip, 0, 62);
      int cy1 = constrain(y + padDown, cy0 + 1, 64);
      u8g2.setClipWindow(cx0, cy0, cx1, cy1);

      if (!strcmp(animMode, "slide_down")) {
        int dy = (int)((float)hClip * ease);
        u8g2.drawStr(curX, y + dy, chPrev);
        u8g2.drawStr(curX, y - hClip + dy, chCur);
      } else if (!strcmp(animMode, "flip")) {
        int midY = y - (hClip / 2);
        if (ease < 0.5f) u8g2.drawStr(curX, y - (int)((hClip / 2) * ease * 2.0f), chPrev);
        else u8g2.drawStr(curX, y + (int)((hClip / 2) * (1.0f - (ease - 0.5f) * 2.0f)), chCur);
        u8g2.drawHLine(curX, midY, cw);
      } else if (!strcmp(animMode, "fade")) {
        u8g2.drawStr(curX, y, (ease < 0.5f) ? chPrev : chCur);
      } else if (!strcmp(animMode, "bounce")) {
        float be = 1.0f - cosf(ease * 3.14159f * 2.5f) * expf(-ease * 3.5f);
        u8g2.drawStr(curX, y - (int)((float)hClip * (1.0f - be)), chCur);
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
  "modular", "activity", "horizon", "pulse", "orbit", "chrono",
  "stacked", "cube", "matrix", "scan", "data", "normal", "big"
};
static const char* const CLOCK_STYLE_NAMES[] = {
  "1. Modern Modular", "2. Activity Rings", "3. Horizon Wave",
  "4. Vital Pulse", "5. Planetary Orbit", "6. Chronograph Pro",
  "7. Nike Stacked", "8. 3D Cube", "9. Matrix Rain",
  "10. Radar Scan", "11. Tech HUD", "12. Classic Ticker",
  "13. Big Time"
};
static const int NUM_CLOCK_STYLES = 13;

static void drawColon(int x, int yMid, int s) {
  if ((millis() / 500) % 2 == 0) {
    u8g2.drawBox(x, yMid - 4 - s / 2, s, s);
    u8g2.drawBox(x, yMid + 4 - s / 2, s, s);
  }
}

static void drawDialRing(int cx, int cy, int r, int uptoStep60, bool thick) {
  u8g2.drawCircle(cx, cy, r);
  int lim = constrain(uptoStep60, 0, 60);
  for (int i = 0; i <= lim; i++) {
    int px = dialX(cx, r, i), py = dialY(cy, r, i);
    u8g2.drawPixel(px, py);
    if (thick) {
      int px2 = dialX(cx, r - 1, i), py2 = dialY(cy, r - 1, i);
      u8g2.drawPixel(px2, py2);
    }
  }
  if (lim > 0) u8g2.drawDisc(dialX(cx, r, lim), dialY(cy, r, lim), 1);
}

static void clock_draw(CardCtx& c) {
  struct tm t;
  int bx = constrain((int)burnX, -1, 1);
  int by = constrain((int)burnY, -1, 1);
  if (!nowTm(t)) {
    u8g2.setFont(F_NUM32);
    textCenter(38 + by, "--:--");
    u8g2.setFont(F_SMALL);
    textCenter(56 + by, netOnline() ? "Syncing NTP time..." : "Waiting for Wi-Fi");
    return;
  }

  if (c.st[1] == 0) {
    const char* defStyle = c.p["style"].is<const char*>() ? c.p["style"].as<const char*>() :
                           (cfg["clock"]["style"].is<const char*>() ? cfg["clock"]["style"].as<const char*>() : "modular");
    if (!strcmp(defStyle, "dial") || !strcmp(defStyle, "clock")) defStyle = "chrono";
    else if (!strcmp(defStyle, "dual")) defStyle = "activity";
    else if (!strcmp(defStyle, "pixel")) defStyle = "stacked";
    else if (!strcmp(defStyle, "minimal")) defStyle = "modular";
    else if (!strcmp(defStyle, "rain")) defStyle = "matrix";
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
  int hShow = t.tm_hour;
  if (!h24) { hShow %= 12; if (!hShow) hShow = 12; }
  char hStr[4], mStr[4], sStr[6], hm[10], date[28], wBuf[24];
  snprintf(hStr, sizeof(hStr), "%02d", hShow);
  snprintf(mStr, sizeof(mStr), "%02d", t.tm_min);
  snprintf(sStr, sizeof(sStr), ":%02d", t.tm_sec);
  fmtHM(hm, t.tm_hour, t.tm_min, h24);
  snprintf(date, sizeof(date), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
  const char* ap = h24 ? "24H" : (t.tm_hour < 12 ? "AM" : "PM");
  const char* tu = (cfg["units"] == "imperial") ? "F" : "C";
  if (homeW.ok) snprintf(wBuf, sizeof(wBuf), "%d°%s %s", (int)roundf(homeW.temp), tu, wmoText(homeW.code));
  else wBuf[0] = 0;

  if (!strcmp(style, "modular")) {
    // 0. MODERN MODULAR
    drawHeader(homeW.ok ? wBuf : date, ap);
    u8g2.setFont(F_NUM32);
    int hw = u8g2.getStrWidth(hStr), mw = u8g2.getStrWidth(mStr);
    int cX = (98 - hw - 7 - mw) / 2 + 4 + bx;
    if (cX < 2) cX = 2;
    drawDigits(0, hStr, cX, 45 + by, 32, anim);
    drawColon(cX + hw + 2, 29 + by, 2);
    drawDigits(1, mStr, cX + hw + 7, 45 + by, 32, anim);

    u8g2.setFont(F_MED);
    textRight(125, 45 + by, sStr);
    if (netOnline()) drawSignalBars(107, 25 + by, netRSSI());

    u8g2.drawRFrame(2, 48, 124, 4, 1);
    int sFill = (t.tm_sec * 120) / 59;
    if (sFill > 0) u8g2.drawBox(4, 49, sFill, 2);

    u8g2.setFont(F_SMALL);
    drawTextFit(2 + bx, 62, 78, date);
    char dPct[16];
    snprintf(dPct, sizeof(dPct), "Day %d%%", ((t.tm_hour * 60 + t.tm_min) * 100) / 1440);
    textRight(126, 62, dPct);

  } else if (!strcmp(style, "activity")) {
    // 1. ACTIVITY RINGS (pure integer LUT, zero FPU loops)
    int cx = 29 + bx, cy = 32 + by;
    drawDialRing(cx, cy, 27, t.tm_sec, true);
    drawDialRing(cx, cy, 20, t.tm_min, true);
    drawDialRing(cx, cy, 13, ((t.tm_hour % 12) * 5) + t.tm_min / 12, false);
    u8g2.setFont(F_TINY);
    int aw = u8g2.getStrWidth(ap);
    u8g2.drawStr(cx - aw / 2, cy + 3, ap);

    drawDottedVLine(60, 2, 60);
    u8g2.setFont(F_TINY);
    drawTextFit(64, 8 + by, 62, date);
    u8g2.drawHLine(63, 10 + by, 64);

    u8g2.setFont(F_HELV14);
    u8g2.drawStr(64, 31 + by, hm);

    u8g2.setFont(F_SMALL);
    char secTxt[12];
    snprintf(secTxt, sizeof(secTxt), "SEC %02d", t.tm_sec);
    u8g2.drawStr(64, 43 + by, secTxt);

    drawBar(64, 46 + by, 62, 5, t.tm_sec / 60.0f);

    u8g2.setFont(F_TINY);
    if (homeW.ok) drawTextFit(64, 61 + by, 62, wBuf);
    else if (netOnline()) {
      char nb[18]; snprintf(nb, sizeof(nb), "WiFi %ddBm", netRSSI());
      u8g2.drawStr(64, 61 + by, nb);
    } else {
      u8g2.drawStr(64, 61 + by, "LOCAL RTC");
    }

  } else if (!strcmp(style, "horizon")) {
    // 2. HORIZON WAVE
    drawHeader(date, homeW.ok ? wBuf : ap);

    u8g2.setFont(F_NUM24);
    int hw = u8g2.getStrWidth(hStr), mw = u8g2.getStrWidth(mStr);
    int cX = (128 - hw - 6 - mw) / 2 + bx;
    drawDigits(0, hStr, cX, 38 + by, 24, anim);
    drawColon(cX + hw + 2, 26 + by, 2);
    drawDigits(1, mStr, cX + hw + 6, 38 + by, 24, anim);

    u8g2.setFont(F_TINY);
    u8g2.drawStr(cX + hw + 6 + mw + 3, 38 + by, sStr + 1);

    uint8_t wavePh = (millis() >> 5) & 31;
    int baseY = 50 + by;
    for (int x = 0; x < 128; x += 2) {
      int w = (fastSin((x >> 2) + wavePh) * 3 + fastCos((x >> 3) - wavePh) * 2) >> 4;
      int wy = constrain(baseY + w, 42, 61);
      u8g2.drawHLine(x, wy, 2);
      for (int y = wy + 2; y < 64; y += 2) {
        if (((x + y) & 3) == 0) u8g2.drawPixel(x, y);
      }
    }
    int sx = 2 + (t.tm_sec * 123) / 59;
    int sw = (fastSin((sx >> 2) + wavePh) * 3 + fastCos((sx >> 3) - wavePh) * 2) >> 4;
    u8g2.drawDisc(sx, constrain(baseY + sw - 1, 41, 61), 2);

  } else if (!strcmp(style, "pulse")) {
    // 3. VITAL PULSE
    drawHeader(date, ap);
    u8g2.setFont(F_HELV14);
    int tw = u8g2.getStrWidth(hm);
    u8g2.setFont(F_SMALL);
    int sw = u8g2.getStrWidth(sStr);
    int startX = (128 - tw - sw - 2) / 2 + bx;
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(startX, 27 + by, hm);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(startX + tw + 2, 27 + by, sStr);

    uint32_t ms = millis();
    int beatX = (int)(ms / 18) % 128;
    static const int8_t ecg[14] = {0, 1, 2, 0, -2, 8, -5, 0, 1, 3, 2, 0, 0, 0};
    for (int x = 0; x < 128; x++) {
      int dist = (x - beatX + 128) % 128;
      int yo = (dist < 14) ? ecg[dist] : 0;
      u8g2.drawPixel(x, 41 + by - yo);
    }
    u8g2.drawDisc(beatX, 41 + by, 1);

    drawDottedHLine(0, 50, 128);
    u8g2.setFont(F_TINY);
    if (homeW.ok) drawTextFit(2, 61, 44, wBuf);
    else u8g2.drawStr(2, 61, netOnline() ? "SYNC OK" : "OFFLINE");
    drawBar(48, 54, 52, 5, t.tm_sec / 60.0f);
    textRight(126, 61, sStr);

  } else if (!strcmp(style, "orbit")) {
    // 4. PLANETARY ORBIT
    int cx = 29 + bx, cy = 32 + by;
    u8g2.drawCircle(cx, cy, 11);
    u8g2.drawEllipse(cx, cy, 11, 4);
    u8g2.drawEllipse(cx, cy, 24, 10);
    int st60 = (int)((millis() / 80) % 60);
    int ox = cx + (sin60(st60) * 24) / 127;
    int oy = cy - (cos60(st60) * 10) / 127;
    u8g2.drawDisc(ox, oy, 2);
    int ox2 = cx + (sin60((st60 + 30) % 60) * 16) / 127;
    int oy2 = cy - (cos60((st60 + 30) % 60) * 7) / 127;
    u8g2.drawPixel(ox2, oy2);

    u8g2.drawVLine(60, 2, 60);
    u8g2.setFont(F_TINY);
    drawTextFit(64, 7, 62, date);
    u8g2.drawHLine(62, 9, 64);

    u8g2.setFont(F_NUM24);
    drawDigits(1, hStr, 65 + bx, 34, 24, anim);
    u8g2.setFont(F_TINY);
    textRight(126, 22, ap);

    u8g2.drawHLine(62, 36, 64);
    u8g2.setFont(F_NUM24);
    drawDigits(2, mStr, 65 + bx, 62, 24, anim);
    u8g2.setFont(F_SMALL);
    textRight(126, 61, sStr);

  } else if (!strcmp(style, "chrono")) {
    // 5. CHRONOGRAPH PRO
    int cx = 31 + bx, cy = 32 + by, r = 29;
    u8g2.drawCircle(cx, cy, r);
    for (int i = 0; i < 12; i++) {
      int st = i * 5;
      int inner = (i % 3 == 0) ? r - 6 : r - 3;
      u8g2.drawLine(dialX(cx, inner, st), dialY(cy, inner, st), dialX(cx, r - 1, st), dialY(cy, r - 1, st));
    }
    int subY = cy + 13;
    u8g2.drawCircle(cx, subY, 7);
    u8g2.drawLine(cx, subY, dialX(cx, 5, t.tm_sec), dialY(subY, 5, t.tm_sec));
    u8g2.drawDisc(cx, subY, 1);

    int hStep = ((t.tm_hour % 12) * 5 + t.tm_min / 12) % 60;
    int hx = dialX(cx, 14, hStep), hy = dialY(cy, 14, hStep);
    u8g2.drawLine(cx, cy, hx, hy);
    u8g2.drawLine(cx + 1, cy, hx + 1, hy);
    u8g2.drawLine(cx, cy, dialX(cx, 22, t.tm_min), dialY(cy, 22, t.tm_min));
    u8g2.drawDisc(cx, cy, 2);

    u8g2.drawRFrame(66, 2, 60, 19, 2);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(70, 10, WDAY[t.tm_wday]);
    textRight(122, 10, ap);
    u8g2.setFont(F_SMALL);
    char md[12]; snprintf(md, sizeof(md), "%d %s", t.tm_mday, MONTH[t.tm_mon]);
    u8g2.drawStr(70, 18, md);

    u8g2.drawRFrame(66, 24, 60, 21, 2);
    u8g2.setFont(F_MED);
    u8g2.drawStr(71, 38, hm);
    u8g2.setFont(F_TINY);
    textRight(122, 38, sStr);

    u8g2.drawRFrame(66, 48, 60, 14, 2);
    u8g2.setFont(F_TINY);
    if (homeW.ok) drawTextFit(70, 57, 52, wBuf);
    else {
      drawBar(70, 52, 52, 5, t.tm_sec / 60.0f);
    }

  } else if (!strcmp(style, "stacked")) {
    // 6. NIKE STACKED
    u8g2.drawRFrame(2, 2, 39, 60, 3);
    u8g2.setFont(F_SMALL);
    drawTextFitCenter(21, 12, 34, WDAY[t.tm_wday]);
    char dBuf[4]; snprintf(dBuf, sizeof(dBuf), "%d", t.tm_mday);
    u8g2.setFont(F_HELV14);
    int dw = u8g2.getStrWidth(dBuf);
    u8g2.drawStr(21 - dw / 2, 30, dBuf);
    u8g2.setFont(F_SMALL);
    drawTextFitCenter(21, 42, 34, MONTH[t.tm_mon]);
    u8g2.setFont(F_TINY);
    if (homeW.ok) {
      char tb[10]; snprintf(tb, sizeof(tb), "%d°%s", (int)roundf(homeW.temp), tu);
      drawTextFitCenter(21, 56, 34, tb);
    } else drawTextFitCenter(21, 56, 34, ap);

    u8g2.setFont(F_NUM24);
    drawDigits(1, hStr, 48 + bx, 29, 24, anim);
    u8g2.setFont(F_TINY);
    u8g2.drawRFrame(98, 12, 24, 10, 2);
    drawTextFitCenter(110, 19, 20, ap);

    u8g2.drawHLine(47, 32, 77);
    drawColon(45, 32, 2);

    u8g2.setFont(F_NUM24);
    drawDigits(2, mStr, 48 + bx, 60, 24, anim);
    u8g2.setFont(F_MED);
    textRight(123, 58, sStr);

    int perim = (t.tm_sec * 376 + (millis() % 1000) * 376 / 1000) / 60;
    int odx = 0, ody = 0;
    if (perim < 127)      { odx = perim; ody = 0; }
    else if (perim < 190) { odx = 127; ody = perim - 127; }
    else if (perim < 317) { odx = 127 - (perim - 190); ody = 63; }
    else                  { odx = 0; ody = 63 - (perim - 317); }
    u8g2.drawDisc(constrain(odx, 1, 126), constrain(ody, 1, 62), 1);

  } else if (!strcmp(style, "cube")) {
    // 7. 3D WIREFRAME CUBE (LUT accelerated, zero overlap)
    uint32_t ms = millis();
    uint8_t ax = (ms / 48) & 31, ay = (ms / 36) & 31, az = (ms / 62) & 31;
    int sax = fastSin(ax), cax = fastCos(ax);
    int say = fastSin(ay), cay = fastCos(ay);
    int saz = fastSin(az), caz = fastCos(az);
    int sx5[8], sy5[8];
    for (int i = 0; i < 8; i++) {
      int xv = (i & 1) ? 28 : -28;
      int yv = (i & 2) ? 28 : -28;
      int zv = (i & 4) ? 28 : -28;
      int y1 = (yv * cax - zv * sax) / 28, z1 = (yv * sax + zv * cax) / 28;
      int x2 = (xv * cay + z1 * say) / 28, z2 = (-xv * say + z1 * cay) / 28;
      int x3 = (x2 * caz - y1 * saz) / 28, y3 = (x2 * saz + y1 * caz) / 28;
      int d = 115 + z2;
      sx5[i] = 30 + bx + (x3 * 52) / d;
      sy5[i] = 32 + by + (y3 * 52) / d;
    }
    for (int i = 0; i < 8; i++) {
      for (int bit = 0; bit < 3; bit++) {
        int j = i ^ (1 << bit);
        if (i < j) u8g2.drawLine(sx5[i], sy5[i], sx5[j], sy5[j]);
      }
      u8g2.drawPixel(sx5[i], sy5[i]);
    }

    u8g2.drawVLine(62, 2, 60);
    u8g2.setFont(F_TINY);
    drawTextFit(66, 7, 60, date);
    u8g2.drawHLine(64, 9, 62);

    u8g2.setFont(F_NUM24);
    drawDigits(1, hStr, 67 + bx, 34, 24, anim);
    u8g2.setFont(F_TINY);
    textRight(126, 22, ap);

    u8g2.drawHLine(64, 36, 62);
    u8g2.setFont(F_NUM24);
    drawDigits(2, mStr, 67 + bx, 62, 24, anim);
    u8g2.setFont(F_SMALL);
    textRight(126, 61, sStr);

  } else if (!strcmp(style, "matrix")) {
    // 8. MATRIX RAIN
    uint32_t now6 = millis();
    static const uint8_t laneSpd[16] = {31, 53, 37, 71, 43, 33, 67, 47, 83, 59, 41, 73, 53, 43, 61, 49};
    for (int lane = 0; lane < 16; lane++) {
      uint32_t tick = now6 / laneSpd[lane];
      int head6 = (int)(tick * 3) % 86 - 16;
      int x6 = 2 + lane * 8;
      for (int n = 0; n < 6; n++) {
        int y6 = head6 - n * 7;
        if (y6 < 0 || y6 >= 64) continue;
        if (n == 0) u8g2.drawBox(x6, y6, 2, 3);
        else u8g2.drawPixel(x6, y6);
      }
    }
    int px = 11 + bx, pw = 106;
    u8g2.setDrawColor(0);
    u8g2.drawRBox(px, 9, pw, 36, 3);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(px, 9, pw, 36, 3);
    u8g2.drawRFrame(px + 2, 11, pw - 4, 32, 2);

    u8g2.setFont(F_NUM24);
    int hw5 = u8g2.getStrWidth(hStr), mw5 = u8g2.getStrWidth(mStr);
    int sx5 = px + 8;
    drawDigits(0, hStr, sx5, 39, 24, anim);
    drawColon(sx5 + hw5 + 2, 27, 2);
    drawDigits(1, mStr, sx5 + hw5 + 7, 39, 24, anim);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(sx5 + hw5 + 7 + mw5 + 3, 39, sStr);

    u8g2.setFont(F_TINY);
    int dw = u8g2.getUTF8Width(date) + 22;
    int dx = (128 - dw) / 2;
    u8g2.setDrawColor(0);
    u8g2.drawRBox(dx, 49, dw, 13, 2);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(dx, 49, dw, 13, 2);
    char foot[36]; snprintf(foot, sizeof(foot), "%s · %s", date, ap);
    drawTextFitCenter(64, 58, dw - 6, foot);

  } else if (!strcmp(style, "scan")) {
    // 9. RADAR SCAN (LUT accelerated)
    int cx = 30 + bx, cy = 32 + by, rr = 28;
    u8g2.drawCircle(cx, cy, rr);
    u8g2.drawCircle(cx, cy, 18);
    u8g2.drawCircle(cx, cy, 9);
    for (int i = -rr + 3; i < rr; i += 4) {
      u8g2.drawPixel(cx + i, cy);
      u8g2.drawPixel(cx, cy + i);
    }
    int sw = (int)((millis() / 50) % 60);
    u8g2.drawLine(cx, cy, dialX(cx, rr - 1, sw), dialY(cy, rr - 1, sw));
    u8g2.drawDisc(dialX(cx, rr - 4, t.tm_sec), dialY(cy, rr - 4, t.tm_sec), 2);

    u8g2.drawVLine(62, 2, 60);
    u8g2.drawRBox(65, 2, 61, 10, 2);
    u8g2.setDrawColor(0);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(68, 9, "RADAR");
    textRight(123, 9, ap);
    u8g2.setDrawColor(1);

    u8g2.setFont(F_SMALL);
    drawTextFit(65, 22, 61, date);
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(65, 39, hm);

    drawBar(65, 43, 41, 5, t.tm_sec / 60.0f);
    u8g2.setFont(F_TINY);
    textRight(126, 48, sStr);

    drawDottedHLine(65, 52, 61);
    if (homeW.ok) drawTextFit(65, 61, 61, wBuf);
    else if (netOnline()) {
      char nb[18]; snprintf(nb, sizeof(nb), "RSSI %ddBm", netRSSI());
      u8g2.drawStr(65, 61, nb);
    } else u8g2.drawStr(65, 61, "SCANNING");

  } else if (!strcmp(style, "data")) {
    // 10. TECH HUD
    u8g2.drawRBox(0, 0, 128, 11, 1);
    u8g2.setDrawColor(0);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(3, 8, "EMO // SYS HUD");
    if (netOnline()) {
      char rb[16]; snprintf(rb, sizeof(rb), "%ddBm", netRSSI());
      textRight(125, 8, rb);
    } else textRight(125, 8, "OFFLINE");
    u8g2.setDrawColor(1);

    u8g2.setFont(F_BOLD);
    char tl[20]; snprintf(tl, sizeof(tl), "> %s%s", hm, sStr);
    u8g2.drawStr(2, 24, tl);
    u8g2.setFont(F_TINY);
    textRight(126, 23, ap);

    u8g2.setFont(F_SMALL);
    char dl[28];
    snprintf(dl, sizeof(dl), "> %s %02d/%02d/%04d", WDAY[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    u8g2.drawStr(2, 35, dl);

    u8g2.setFont(F_TINY);
    char env[36];
    if (homeW.ok) snprintf(env, sizeof(env), "> ENV %s", wBuf);
    else snprintf(env, sizeof(env), "> IP  %s", netIP().c_str());
    drawTextFit(2, 46, 124, env);

    drawDottedHLine(0, 50, 128);
    char ml[18]; snprintf(ml, sizeof(ml), "MEM %uK", (unsigned)(ESP.getFreeHeap() / 1024));
    u8g2.drawStr(2, 61, ml);
    drawBar(46, 54, 56, 6, t.tm_sec / 60.0f);
    char sp2[8]; snprintf(sp2, sizeof(sp2), "%02ds", t.tm_sec);
    textRight(126, 61, sp2);

  } else if (!strcmp(style, "normal")) {
    // 11. CLASSIC TICKER
    u8g2.setFont(F_BOLD);
    u8g2.drawUTF8(2 + bx, 10 + by, WDAY[t.tm_wday]);
    u8g2.setFont(F_SMALL);
    char dLine[20]; snprintf(dLine, sizeof(dLine), "%d %s %04d", t.tm_mday, MONTH[t.tm_mon], t.tm_year + 1900);
    textRight(126, 10 + by, dLine);
    u8g2.drawHLine(0, 12 + by, 128);

    u8g2.setFont(F_NUM24);
    int hw6 = u8g2.getStrWidth(hStr), mw6 = u8g2.getStrWidth(mStr);
    int cX6 = (98 - hw6 - 6 - mw6) / 2 + 4 + bx;
    if (cX6 < 2) cX6 = 2;
    drawDigits(0, hStr, cX6, 41 + by, 24, anim);
    drawColon(cX6 + hw6 + 2, 29 + by, 2);
    drawDigits(1, mStr, cX6 + hw6 + 6, 41 + by, 24, anim);

    u8g2.setFont(F_TINY);
    textRight(125, 25 + by, ap);
    u8g2.setFont(F_MED);
    textRight(125, 41 + by, sStr);

    u8g2.drawHLine(0, 46 + by, 128);
    int secPx = (t.tm_sec * 128) / 59;
    if (secPx > 0) u8g2.drawBox(0, 46 + by, secPx, 2);

    u8g2.setFont(F_TINY);
    if (netOnline()) {
      drawSignalBars(2, 61 + by, netRSSI());
      char nb[18]; snprintf(nb, sizeof(nb), "%ddBm", netRSSI());
      u8g2.drawStr(21, 61 + by, nb);
    } else u8g2.drawStr(2, 61 + by, "OFFLINE");

    if (homeW.ok) drawTextRightFit(126, 61 + by, 68, wBuf);
    else {
      char hb[16]; snprintf(hb, sizeof(hb), "HEAP %uK", (unsigned)(ESP.getFreeHeap() / 1024));
      textRight(126, 61 + by, hb);
    }

  } else {
    // 12. MODERN BIG TIME (zero top/bottom overlap)
    u8g2.setFont(F_TINY);
    if (homeW.ok) drawTextFit(2 + bx, 8 + by, 92, wBuf);
    else u8g2.drawStr(2 + bx, 8 + by, netOnline() ? "SYNCED" : "LOCAL");
    u8g2.drawRFrame(103, 0 + by, 24, 9, 2);
    drawTextFitCenter(115, 7 + by, 20, ap);
    u8g2.drawHLine(0, 10 + by, 128);

    u8g2.setFont(F_NUM32);
    int hw7 = u8g2.getStrWidth(hStr), mw7 = u8g2.getStrWidth(mStr);
    int cX7 = (102 - hw7 - 7 - mw7) / 2 + 2 + bx;
    if (cX7 < 2) cX7 = 2;
    drawDigits(0, hStr, cX7, 46 + by, 32, anim);
    drawColon(cX7 + hw7 + 2, 30 + by, 3);
    drawDigits(1, mStr, cX7 + hw7 + 7, 46 + by, 32, anim);

    u8g2.setFont(F_MED);
    textRight(126, 46 + by, sStr);

    u8g2.drawHLine(0, 50 + by, 128);
    int sPx = (t.tm_sec * 128) / 59;
    if (sPx > 0) u8g2.drawBox(0, 50 + by, sPx, 2);

    u8g2.setFont(F_SMALL);
    char fullDate[28];
    snprintf(fullDate, sizeof(fullDate), "%s, %d %s %04d", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon], t.tm_year + 1900);
    textCenter(62 + by, fullDate);
  }
}

static bool clock_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) {
    c.st[0] = (c.st[0] + 1) % NUM_CLOCK_STYLES;
    c.st[1] = 1;
    const char* newStyle = CLOCK_STYLES[c.st[0]];
    c.p["style"] = newStyle;
    markDirty(DIRTY_CARDS);
    cfg["clock"]["style"] = newStyle;
    markDirty(DIRTY_CFG);
    toastShow(CLOCK_STYLE_NAMES[c.st[0] % NUM_CLOCK_STYLES]);
    audioSfx("pop", EV_UI);
    return true;
  }
  return false;
}
const CardType CT_clock = {"clock", 0, 0, 0, nullptr, clock_draw, clock_input, nullptr, speakTime, nullptr};

// ================================================================== analog clock
static void analog_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  int cx = 32 + constrain((int)burnX, -1, 1);
  int cy = 32 + constrain((int)burnY, -1, 1);
  int r = 30;
  u8g2.drawCircle(cx, cy, r);
  for (int i = 0; i < 12; i++) {
    int st = i * 5;
    int inner = (i % 3 == 0) ? r - 5 : r - 2;
    u8g2.drawLine(dialX(cx, inner, st), dialY(cy, inner, st), dialX(cx, r - 1, st), dialY(cy, r - 1, st));
  }
  int hStep = ((t.tm_hour % 12) * 5 + t.tm_min / 12) % 60;
  int hx = dialX(cx, 15, hStep), hy = dialY(cy, 15, hStep);
  u8g2.drawLine(cx, cy, hx, hy);
  u8g2.drawLine(cx + 1, cy, hx + 1, hy);
  u8g2.drawLine(cx, cy, dialX(cx, 22, t.tm_min), dialY(cy, 22, t.tm_min));
  u8g2.drawLine(cx, cy, dialX(cx, 26, t.tm_sec), dialY(cy, 26, t.tm_sec));
  u8g2.drawDisc(cx, cy, 2);

  drawDottedVLine(65, 2, 60);
  u8g2.setFont(F_HELV14);
  u8g2.drawStr(70, 19, WDAY[t.tm_wday]);
  char b[16];
  snprintf(b, sizeof(b), "%d %s", t.tm_mday, MONTH[t.tm_mon]);
  u8g2.setFont(F_MED);
  u8g2.drawStr(70, 33, b);
  fmtHM(b, t.tm_hour, t.tm_min, cfgH24());
  u8g2.drawStr(70, 47, b);
  if (!cfgH24()) {
    u8g2.setFont(F_TINY);
    textRight(126, 47, t.tm_hour < 12 ? "AM" : "PM");
  }
  drawBar(70, 54, 56, 6, t.tm_sec / 60.0f);
}
const CardType CT_analog = {"analog", 0, 0, 0, nullptr, analog_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== binary clock
static void binary_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  char hm[12];
  snprintf(hm, sizeof(hm), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  drawTitle(c, "Binary Clock", hm);

  int digits[6] = {t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10, t.tm_sec / 10, t.tm_sec % 10};
  static const uint8_t bits[6] = {2, 4, 3, 4, 3, 4};
  static const uint8_t xs[6] = {20, 35, 57, 72, 94, 109};
  static const char* const lab = "HHMMSS";

  u8g2.setFont(F_TINY);
  for (int b = 0; b < 4; b++) {
    int y = 51 - b * 11;
    char w[2] = {(char)('0' + (1 << b)), 0};
    u8g2.drawStr(2, y + 2, w);
  }
  for (int col = 0; col < 6; col++) {
    int x = xs[col] + burnX;
    for (int b = 0; b < 4; b++) {
      int y = 51 - b * 11 + burnY;
      if (b >= bits[col]) continue;
      if (digits[col] & (1 << b)) u8g2.drawDisc(x, y, 4);
      else u8g2.drawCircle(x, y, 4);
    }
    char l[2] = {lab[col], 0};
    u8g2.drawStr(x - 1, 63, l);
  }
}
const CardType CT_binary = {"binary", 0, 0, 0, nullptr, binary_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== word clock
static void wordclock_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  char tag[16];
  snprintf(tag, sizeof(tag), "%s %d", WDAY[t.tm_wday], t.tm_mday);
  drawTitle(c, "Word Clock", tag);
  static const char* const H[] = {"twelve", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven"};
  static const char* const M[] = {"", "five past", "ten past", "quarter past", "twenty past", "twenty-five past", "half past",
                                  "twenty-five to", "twenty to", "quarter to", "ten to", "five to"};
  int idx = ((t.tm_min + 2) / 5) % 12;
  int h = t.tm_hour + ((t.tm_min + 2) / 5 >= 12 ? 1 : 0) + (idx > 6 ? 1 : 0);
  char s[64];
  if (idx == 0) snprintf(s, sizeof(s), "It's %s o'clock", H[h % 12]);
  else snprintf(s, sizeof(s), "It's %s %s", M[idx], H[h % 12]);

  u8g2.setFont(F_HELV14);
  int lines = wrapText(0, 0, 120, 15, s, 0);
  int lh = 15;
  if (lines > 3) {
    u8g2.setFont(F_MED);
    lh = 12;
    lines = wrapText(0, 0, 120, lh, s, 0);
  }
  int y0 = 37 - (lines * lh) / 2 + lh - 2;
  wrapText(4 + burnX, max(24, y0) + burnY, 120, lh, s, 4);
}
const CardType CT_wordclock = {"wordclock", 0, 0, 0, nullptr, wordclock_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== calendar
static void calendar_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  char hdr[24], dayTag[12];
  snprintf(hdr, sizeof(hdr), "%s %d", MONTH_LONG[t.tm_mon], t.tm_year + 1900);
  snprintf(dayTag, sizeof(dayTag), "%s %d", WDAY[t.tm_wday], t.tm_mday);
  drawHeader(hdr, dayTag);

  bool mon = cfg["time"]["weekMon"] | true;
  const char* dn = mon ? "MoTuWeThFrSaSu" : "SuMoTuWeThFrSa";
  u8g2.setFont(F_TINY);
  for (int i = 0; i < 7; i++) {
    char b[3] = {dn[i * 2], dn[i * 2 + 1], 0};
    u8g2.drawStr(4 + i * 18, 18, b);
  }
  drawDottedHLine(0, 20, 128);
  int firstW = ((t.tm_wday - (t.tm_mday - 1)) % 7 + 7) % 7;
  int off = mon ? (firstW + 6) % 7 : firstW;
  int dim = daysInMonth(t.tm_year + 1900, t.tm_mon);
  u8g2.setFont(F_TINY);
  for (int d = 1; d <= dim; d++) {
    int pos = off + d - 1, row = pos / 7, col = pos % 7;
    int x = 1 + col * 18, y = 27 + row * 7;
    if (y > 63) break;
    char b[4];
    snprintf(b, sizeof(b), "%d", d);
    if (d == t.tm_mday) {
      u8g2.drawRBox(x + 1, y - 6, 15, 7, 1);
      u8g2.setDrawColor(0);
    }
    int tw = u8g2.getStrWidth(b);
    u8g2.drawStr(x + (17 - tw) / 2, y, b);
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
  bool h24 = cfgH24();
  char localHm[10];
  fmtHM(localHm, t.tm_hour, t.tm_min, h24);
  drawTitle(c, "World Clock", localHm);

  JsonArray a = c.p["cities"];
  if (a.isNull() || !a.size()) { drawCenteredMsg("No cities yet", "Add them in dashboard"); return; }
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
  int rowH = 52 / n;
  for (int i = 0; i < n; i++) {
    int yTop = 12 + i * rowH;
    int yBase = yTop + rowH / 2 + 3;
    if (i > 0) drawDottedHLine(0, yTop, 128);
    u8g2.setFont(F_SMALL);
    drawTextFit(2, yBase, 64, a[i]["n"] | "?");

    int dd = (cache[i].tm_year > t.tm_year) ? 1 :
             (cache[i].tm_year < t.tm_year ? -1 :
             (cache[i].tm_yday > t.tm_yday ? 1 : (cache[i].tm_yday < t.tm_yday ? -1 : 0)));
    if (dd != 0) {
      u8g2.setFont(F_TINY);
      u8g2.drawStr(69, yBase - 1, dd > 0 ? "+1d" : "-1d");
    }
    char hm[12];
    fmtHM(hm, cache[i].tm_hour, cache[i].tm_min, h24);
    if (!h24) {
      int l = strlen(hm);
      snprintf(hm + l, sizeof(hm) - l, " %c", cache[i].tm_hour < 12 ? 'a' : 'p');
    }
    u8g2.setFont(F_MED);
    textRight(126, yBase, hm);
  }
}
const CardType CT_world = {"world", 0, 0, 0, nullptr, world_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== progress
static float progs[4];
static void calcProgress() {
  struct tm t;
  if (!nowTm(t)) return;
  int y = t.tm_year + 1900;
  float mn = t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.0f;
  int diy = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
  progs[0] = mn / 1440.0f;
  progs[1] = (((t.tm_wday + 6) % 7) * 1440 + mn) / (7 * 1440.0f);
  progs[2] = ((t.tm_mday - 1) * 1440 + mn) / (daysInMonth(y, t.tm_mon) * 1440.0f);
  progs[3] = (t.tm_yday * 1440 + mn) / (diy * 1440.0f);
}
static void progress_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  calcProgress();
  char yr[8]; snprintf(yr, sizeof(yr), "%d", t.tm_year + 1900);
  drawTitle(c, "Progress", yr);
  static const char* const L[] = {"Day", "Week", "Month", "Year"};
  for (int i = 0; i < 4; i++) {
    int y0 = 14 + i * 13;
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(1, y0 + 7, L[i]);
    drawBar(35, y0, 64, 8, progs[i]);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", (int)(progs[i] * 100));
    textRight(127, y0 + 7, b);
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
  double days = (time(nullptr) - 947182440.0) / 86400.0;
  double a = fmod(days, 29.530588853);
  return a < 0 ? a + 29.530588853 : a;
}
static const char* moonName(float age) {
  static const char* const N[] = {"New moon", "Wax crescent", "1st quarter", "Wax gibbous", "Full moon",
                                  "Wan gibbous", "Last quarter", "Wan crescent"};
  return N[(int)((age / 29.530588853f) * 8 + 0.5f) % 8];
}
static void fmtSunHM(const char* hhmm, char* out, size_t n) {
  int m = parseHM(hhmm);
  if (m < 0) { safeCopy(out, "--:--", n); return; }
  fmtHM(out, m / 60, m % 60, cfgH24());
}
static void sunmoon_draw(CardCtx& c) {
  float age = moonAge(), p = age / 29.530588853f;
  int lit = (int)((1 - cosf(2 * PI * p)) * 50);
  char litTag[10]; snprintf(litTag, sizeof(litTag), "%d%% lit", lit);
  drawTitle(c, "Sun & Moon", litTag);

  int cx = 103, cy = 29, r = 13;
  for (int y = -r; y <= r; y++) {
    float w = sqrtf((float)(r * r - y * y));
    float k = cosf(2 * PI * p);
    int x0, x1;
    if (p < 0.5f) { x0 = (int)(w * k); x1 = (int)w; }
    else { x0 = -(int)w; x1 = (int)(-w * k); }
    if (x1 > x0) u8g2.drawHLine(cx + x0, cy + y, x1 - x0 + 1);
  }
  u8g2.drawCircle(cx, cy, r);
  u8g2.setFont(F_TINY);
  drawTextFitCenter(103, 52, 46, moonName(age));
  char ageB[16]; snprintf(ageB, sizeof(ageB), "Day %.0f", age);
  drawTextFitCenter(103, 61, 46, ageB);

  drawDottedVLine(76, 13, 50);
  drawWeatherIcon(1, 14, 16, 0, true);
  if (!homeW.ok) {
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(2, 42, "Set location");
    u8g2.drawStr(2, 53, "for sun times");
    return;
  }
  char srB[12], ssB[12], b[24];
  fmtSunHM(homeW.sunrise, srB, sizeof(srB));
  fmtSunHM(homeW.sunset, ssB, sizeof(ssB));
  u8g2.setFont(F_SMALL);
  snprintf(b, sizeof(b), "Up %s", srB);
  u8g2.drawStr(20, 22, b);
  snprintf(b, sizeof(b), "Dn %s", ssB);
  u8g2.drawStr(20, 32, b);

  int sr = parseHM(homeW.sunrise), ss = parseHM(homeW.sunset);
  if (sr >= 0 && ss > sr) {
    snprintf(b, sizeof(b), "Day %dh %02dm", (ss - sr) / 60, (ss - sr) % 60);
    u8g2.drawStr(2, 47, b);
  }
  snprintf(b, sizeof(b), "UV max %.0f", homeW.uv);
  u8g2.drawStr(2, 59, b);
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
  String title = c.p["title"] | "Countdown";
  drawHeader(title.c_str(), c.p["date"] | "");
  time_t tgt = paramDate(c.p, "00:00");
  if (!tgt || !timeOK()) { drawCenteredMsg(tgt ? "Syncing time..." : "Set a date", "In card settings"); return; }
  long diff = (long)(tgt - time(nullptr));
  char b[24];
  if (diff > 86400) {
    long days = diff / 86400;
    snprintf(b, sizeof(b), "%ld", days);
    u8g2.setFont(days >= 1000 ? F_NUM24 : F_NUM32);
    int nw = u8g2.getStrWidth(b);
    const char* unit = (days == 1) ? "DAY" : "DAYS";
    u8g2.setFont(F_SMALL);
    int uw = u8g2.getStrWidth(unit);
    int sx = (128 - nw - 4 - uw) / 2 + burnX;
    u8g2.setFont(days >= 1000 ? F_NUM24 : F_NUM32);
    u8g2.drawStr(sx, 47, b);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(sx + nw + 4, 47, unit);

    drawDottedHLine(0, 51, 128);
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld remaining", (diff % 86400) / 3600, (diff % 3600) / 60, diff % 60);
    textCenter(61, b);
  } else if (diff > 0) {
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", diff / 3600, (diff % 3600) / 60, diff % 60);
    u8g2.setFont(F_NUM24);
    textCenter(44, b);
    u8g2.setFont(F_TINY);
    textCenter(60, "HOURS : MIN : SEC LEFT");
  } else {
    u8g2.setFont(F_HELV14);
    textCenter(38, diff > -86400 ? "It's Here!" : "Completed");
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
  String title = c.p["title"] | "Days since";
  drawHeader(title.c_str(), c.p["date"] | "");
  time_t from = paramDate(c.p, "00:00");
  if (!from || !timeOK()) { drawCenteredMsg(from ? "Syncing time..." : "Set a date", "In card settings"); return; }
  long d = (long)(time(nullptr) - from) / 86400;
  char b[16];
  snprintf(b, sizeof(b), "%ld", d);
  u8g2.setFont(abs(d) >= 1000 ? F_NUM24 : F_NUM32);
  int nw = u8g2.getStrWidth(b);
  const char* unit = (d == 1) ? "DAY" : "DAYS";
  u8g2.setFont(F_SMALL);
  int uw = u8g2.getStrWidth(unit);
  int sx = (128 - nw - 4 - uw) / 2 + burnX;
  u8g2.setFont(abs(d) >= 1000 ? F_NUM24 : F_NUM32);
  u8g2.drawStr(sx, 48, b);
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(sx + nw + 4, 48, unit);
  drawDottedHLine(0, 52, 128);
  u8g2.setFont(F_TINY);
  textCenter(61, "ELAPSED TIME");
}
static void dayssince_speak(CardCtx& c, String& s) {
  time_t from = paramDate(c.p, "00:00");
  if (!from) return;
  s = String(c.p["title"] | "Days since") + ": " + numWords((long)(time(nullptr) - from) / 86400) + " days.";
}
const CardType CT_dayssince = {"dayssince", 0, 0, 0, nullptr, dayssince_draw, nullptr, nullptr, dayssince_speak, nullptr};
