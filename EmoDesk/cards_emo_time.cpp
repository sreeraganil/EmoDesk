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
  "modular",   // 0: Modern Modular (Apple Watch Ultra)
  "activity",  // 1: Activity Rings (Smartwatch Fitness)
  "horizon",   // 2: Horizon Wave (Pixel Watch)
  "pulse",     // 3: Health ECG Pulse (Smartwatch Health)
  "orbit",     // 4: Planetary Orbit (Galaxy Watch)
  "chrono",    // 5: Chronograph Pro (Luxury Smartwatch)
  "stacked",   // 6: Nike Typography (Orbiting Second)
  "cube",      // 7: 3D Wireframe Cube (NWatch 3D)
  "matrix",    // 8: Matrix Rain (Cyberpunk Code)
  "scan",      // 9: Sci-Fi Scan (Radar Sweep)
  "data",      // 10: Cyberpunk Data (Tech HUD)
  "normal",    // 11: Classic Ticker (Zak Kemble)
  "big"        // 12: Big Time (Modern Giant)
};
static const char* const CLOCK_STYLE_NAMES[] = {
  "1. Modern Modular", "2. Activity Rings", "3. Horizon Wave",
  "4. Health ECG Pulse", "5. Planetary Orbit", "6. Chronograph Pro",
  "7. Nike Typography", "8. 3D Wireframe Cube", "9. Matrix Rain",
  "10. Sci-Fi Scan", "11. Cyberpunk Data", "12. Classic Ticker",
  "13. Modern Big Time"
};
static const int NUM_CLOCK_STYLES = 13;

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

  // Initialize style from card params on first run, or respect touch switch / saved cfg
  if (c.st[1] == 0) {
    const char* defStyle = c.p["style"].is<const char*>() ? c.p["style"].as<const char*>() :
                           (cfg["clock"]["style"].is<const char*>() ? cfg["clock"]["style"].as<const char*>() : "modular");
    // Backward compatibility mappings
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
  char hm[8], date[32];
  fmtHM(hm, t.tm_hour, t.tm_min, h24);
  snprintf(date, sizeof(date), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
  const char* ap = t.tm_hour < 12 ? "AM" : "PM";

  if (!strcmp(style, "modular")) {
    // ============================================================= 0. MODULAR — Apple Ultra Infograph
    // Top bar: weather left, signal/AM-PM right
    u8g2.setFont(F_TINY);
    if (homeW.ok) {
      char wBuf[20];
      snprintf(wBuf, sizeof(wBuf), "%d\xc2\xb0 %s", (int)roundf(homeW.temp), wmoText(homeW.code));
      u8g2.drawUTF8(2 + bx, 7 + by, wBuf);
    } else {
      u8g2.drawUTF8(2 + bx, 7 + by, date);
    }
    if (netOnline()) {
      char nb[12]; snprintf(nb, sizeof(nb), "%ddB", netRSSI());
      textRight(126 + bx, 7 + by, nb);
    } else {
      textRight(126 + bx, 7 + by, ap);
    }
    u8g2.drawHLine(0 + bx, 9 + by, 128);

    // Centre: HH large | blinking colon | MM large
    int h2 = t.tm_hour, m2 = t.tm_min;
    if (!h24) { h2 %= 12; if (!h2) h2 = 12; }
    char hStr[4], mStr[4];
    snprintf(hStr, sizeof(hStr), "%02d", h2);
    snprintf(mStr, sizeof(mStr), "%02d", m2);

    u8g2.setFont(F_NUM32);
    int hw = u8g2.getStrWidth(hStr);
    int mw = u8g2.getStrWidth(mStr);
    int gap = 6; // colon width
    int totalW = hw + gap + mw;
    int startX = (128 - totalW) / 2 + bx;

    drawDigits(0, hStr, startX,        42 + by, 32, anim);
    drawDigits(1, mStr, startX + hw + gap, 42 + by, 32, anim);
    // Blinking colon dots
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawBox(startX + hw + 1, 24 + by, 2, 2);
      u8g2.drawBox(startX + hw + 1, 32 + by, 2, 2);
    }

    // Seconds progress bar with bright leading pixel
    u8g2.drawHLine(0 + bx, 46 + by, 128);
    int secPx = (t.tm_sec * 128) / 60;
    if (secPx > 0) u8g2.drawHLine(0 + bx, 47 + by, secPx);
    if (secPx < 128) u8g2.drawBox(secPx + bx, 46 + by, 2, 3);

    // Bottom row: full date + seconds tag
    u8g2.setFont(F_TINY);
    char foot[32];
    snprintf(foot, sizeof(foot), "%s %d %s", WDAY_LONG[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
    u8g2.drawUTF8(2 + bx, 60 + by, foot);
    char sBuf[6]; snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    textRight(126 + bx, 60 + by, sBuf);

  } else if (!strcmp(style, "activity")) {
    // ============================================================= 1. ACTIVITY RINGS
    int cx = 30 + bx, cy = 33 + by;

    // Draw dashed ring tracks
    for (float a = 0; a < 6.2832f; a += 0.22f) {
      u8g2.drawPixel(cx + lroundf(cosf(a) * 26), cy + lroundf(sinf(a) * 26));
      u8g2.drawPixel(cx + lroundf(cosf(a) * 19), cy + lroundf(sinf(a) * 19));
      u8g2.drawPixel(cx + lroundf(cosf(a) * 12), cy + lroundf(sinf(a) * 12));
    }

    // Outer ring: Seconds (double-thick arc)
    float sFrac = (t.tm_sec + (millis() % 1000) * 0.001f) / 60.0f;
    float sEnd  = sFrac * 6.2832f - 1.5708f;
    for (float a = -1.5708f; a < sEnd; a += 0.09f) {
      u8g2.drawPixel(cx + lroundf(cosf(a) * 26), cy + lroundf(sinf(a) * 26));
      u8g2.drawPixel(cx + lroundf(cosf(a) * 25), cy + lroundf(sinf(a) * 25));
    }
    // Leading dot
    u8g2.drawDisc(cx + lroundf(cosf(sEnd) * 25.5f), cy + lroundf(sinf(sEnd) * 25.5f), 2);

    // Middle ring: Minutes
    float mFrac = (t.tm_min + t.tm_sec / 60.0f) / 60.0f;
    float mEnd  = mFrac * 6.2832f - 1.5708f;
    for (float a = -1.5708f; a < mEnd; a += 0.12f) {
      u8g2.drawPixel(cx + lroundf(cosf(a) * 19), cy + lroundf(sinf(a) * 19));
      u8g2.drawPixel(cx + lroundf(cosf(a) * 18), cy + lroundf(sinf(a) * 18));
    }
    u8g2.drawDisc(cx + lroundf(cosf(mEnd) * 18.5f), cy + lroundf(sinf(mEnd) * 18.5f), 2);

    // Inner ring: Hours
    float hFrac = ((t.tm_hour % 12) + t.tm_min / 60.0f) / 12.0f;
    float hEnd  = hFrac * 6.2832f - 1.5708f;
    for (float a = -1.5708f; a < hEnd; a += 0.18f) {
      u8g2.drawPixel(cx + lroundf(cosf(a) * 12), cy + lroundf(sinf(a) * 12));
      u8g2.drawPixel(cx + lroundf(cosf(a) * 11), cy + lroundf(sinf(a) * 11));
    }
    u8g2.drawDisc(cx + lroundf(cosf(hEnd) * 11.5f), cy + lroundf(sinf(hEnd) * 11.5f), 1);

    // Right: Time + date + sec
    u8g2.setFont(F_HELV14);
    int rX = 66 + bx;
    u8g2.drawStr(rX, 32 + by, hm);
    u8g2.setFont(F_TINY);
    u8g2.drawUTF8(rX, 11 + by, date);
    char sl[12]; snprintf(sl, sizeof(sl), ":%.2d", t.tm_sec);
    u8g2.drawStr(rX, 46 + by, sl);
    u8g2.drawHLine(rX - 2, 48 + by, 60);
    bool pulse = (millis() % 900) < 180;
    char bpm[14]; snprintf(bpm, sizeof(bpm), "%s 72bpm", pulse ? "<3" : " -");
    u8g2.drawStr(rX, 62 + by, bpm);

  } else if (!strcmp(style, "horizon")) {
    // ============================================================= 2. HORIZON WAVE
    uint32_t tMs = millis();
    float ph = tMs * 0.0030f;
    int baseY = 44 + by;

    // Solid fill below wave
    for (int x = 0; x < 128; x++) {
      float w = sinf(x * 0.075f + ph) * 4.0f + cosf(x * 0.038f - ph * 0.6f) * 2.5f;
      int wy = baseY + (int)w;
      if (wy < 0) wy = 0; if (wy > 63) wy = 63;
      // bright wave crest pixel
      u8g2.drawPixel(x + bx, wy);
      u8g2.drawPixel(x + bx, wy + 1);
      // dotted fill below
      for (int y = wy + 3; y < 64; y += 2) {
        if ((x + y) % 3 != 0) u8g2.drawPixel(x + bx, y);
      }
    }

    // Date bar at top
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(2 + bx, 8 + by, date);
    if (homeW.ok) {
      char tb[8]; snprintf(tb, sizeof(tb), "%d\xc2\xb0", (int)roundf(homeW.temp));
      textRight(126 + bx, 8 + by, tb);
    } else textRight(126 + bx, 8 + by, ap);

    // Centre time with black clear-box underneath
    int h2 = t.tm_hour;
    if (!h24) { h2 %= 12; if (!h2) h2 = 12; }
    char hStr[4], mStr[4];
    snprintf(hStr, sizeof(hStr), "%02d", h2);
    snprintf(mStr, sizeof(mStr), "%02d", t.tm_min);
    u8g2.setFont(F_NUM24);
    int hw = u8g2.getStrWidth(hStr), mw2 = u8g2.getStrWidth(mStr);
    int cX = (128 - hw - 4 - mw2) / 2 + bx;
    u8g2.setDrawColor(0); u8g2.drawBox(cX - 3, 13 + by, hw + mw2 + 10, 26); u8g2.setDrawColor(1);
    drawDigits(0, hStr, cX, 36 + by, 24, anim);
    // colon dots
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawBox(cX + hw, 20 + by, 2, 2);
      u8g2.drawBox(cX + hw, 27 + by, 2, 2);
    }
    drawDigits(1, mStr, cX + hw + 4, 36 + by, 24, anim);

  } else if (!strcmp(style, "pulse")) {
    // ============================================================= 3. HEALTH ECG PULSE
    // Header
    u8g2.setFont(F_BOLD);
    int h2 = t.tm_hour;
    if (!h24) { h2 %= 12; if (!h2) h2 = 12; }
    char hStr[4], mStr[4];
    snprintf(hStr, sizeof(hStr), "%02d", h2);
    snprintf(mStr, sizeof(mStr), "%02d", t.tm_min);
    u8g2.setFont(F_NUM24);
    int hw = u8g2.getStrWidth(hStr), mw3 = u8g2.getStrWidth(mStr);
    int cX = (128 - hw - 4 - mw3) / 2 + bx;
    drawDigits(0, hStr, cX,         22 + by, 24, anim);
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawBox(cX + hw, 10 + by, 2, 2);
      u8g2.drawBox(cX + hw, 17 + by, 2, 2);
    }
    drawDigits(1, mStr, cX + hw + 4, 22 + by, 24, anim);
    u8g2.setFont(F_TINY);
    textRight(126 + bx, 7 + by, ap);
    u8g2.drawUTF8(2 + bx, 7 + by, date);
    u8g2.drawHLine(0, 25 + by, 128);

    // ECG wave — richer PQRST shape
    uint32_t ms = millis();
    int beatX = (int)(ms / 12) % 128;
    static const int8_t ecgShape[20] = {0, 0, 1, 1, -1, 0, 0, 10, -12, 6, -2, 0, 2, 3, 2, 1, 0, 0, 0, 0};
    for (int x = 0; x < 128; x++) {
      int dist = (x - beatX + 128) % 128;
      int yo = 0;
      if (dist < 20) yo = ecgShape[dist];
      u8g2.drawPixel(x + bx, 42 + by - yo);
    }
    // Bright head dot
    u8g2.drawDisc(beatX + bx, 42 + by, 2);
    // Fading tail
    for (int t2 = 1; t2 <= 4; t2++) {
      int tx = (beatX - t2 * 3 + 128) % 128;
      u8g2.drawPixel(tx + bx, 42 + by);
    }

    // Bottom: BPM + sec bar
    u8g2.drawHLine(0, 51 + by, 128);
    bool pulse2 = (ms % 850) < 170;
    u8g2.setFont(F_TINY);
    char bpm2[14]; snprintf(bpm2, sizeof(bpm2), "%s 72 BPM", pulse2 ? "<3" : " -");
    u8g2.drawStr(2 + bx, 62 + by, bpm2);
    // sec progress right
    int sp = (t.tm_sec * 56) / 60;
    drawBar(70 + bx, 54 + by, 56, 6, t.tm_sec / 60.0f);
    char sBuf[6]; snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    textRight(126 + bx, 62 + by, sBuf);

  } else if (!strcmp(style, "orbit")) {
    // ============================================================= 4. PLANETARY ORBIT
    int cx = 30 + bx, cy = 32 + by;

    // Globe with meridians & parallels
    u8g2.drawCircle(cx, cy, 15);
    // Meridian lines
    u8g2.drawEllipse(cx, cy, 10, 15);  // main ellipse
    u8g2.drawEllipse(cx, cy, 5, 15);   // inner ellipse
    // Equator
    u8g2.drawHLine(cx - 15, cy, 31);
    // Tropic lines (dashed-ish using pixels)
    for (int px = cx - 12; px <= cx + 12; px += 3)
      u8g2.drawPixel(px, cy - 7);
    for (int px = cx - 12; px <= cx + 12; px += 3)
      u8g2.drawPixel(px, cy + 7);

    // Elliptical orbit track (dashed)
    for (float a = 0; a < 6.2832f; a += 0.25f)
      u8g2.drawPixel(cx + lroundf(cosf(a) * 24), cy + lroundf(sinf(a) * 10));

    // Orbiting moon — full 3D-ish orbit
    float oa = millis() * 0.0020f;
    int ox = cx + lroundf(cosf(oa) * 24);
    int oy = cy + lroundf(sinf(oa) * 10);
    u8g2.drawDisc(ox, oy, 2);

    // Right: stacked HH / MM with separator
    int h3 = t.tm_hour;
    if (!h24) { h3 %= 12; if (!h3) h3 = 12; }
    char hStr2[4], mStr2[4];
    snprintf(hStr2, sizeof(hStr2), "%02d", h3);
    snprintf(mStr2, sizeof(mStr2), "%02d", t.tm_min);

    u8g2.setFont(F_NUM24);
    drawDigits(1, hStr2, 72 + bx, 28 + by, 24, anim);
    u8g2.drawHLine(72 + bx, 32 + by, 54); // separator
    drawDigits(2, mStr2, 72 + bx, 56 + by, 24, anim);

    // Day label + seconds
    u8g2.setFont(F_TINY);
    u8g2.drawUTF8(72 + bx, 7 + by, WDAY[t.tm_wday]);
    char sBuf[6]; snprintf(sBuf, sizeof(sBuf), ":%02d", t.tm_sec);
    textRight(126 + bx, 62 + by, sBuf);

  } else if (!strcmp(style, "chrono")) {
    // ============================================================= 5. CHRONOGRAPH PRO
    int cx = 64 + bx, cy = 32 + by, r = 30;

    // Outer ring
    u8g2.drawCircle(cx, cy, r);

    // Tick marks: every 5° (12 marks), major every 15° (4 marks)
    for (int mark = 0; mark < 60; mark++) {
      if (mark % 5 != 0) continue;
      float q = mark * 6.2832f / 60.0f - 1.5708f;
      int inner = (mark % 15 == 0) ? 22 : 25;
      u8g2.drawLine(cx + lroundf(cosf(q) * inner), cy + lroundf(sinf(q) * inner),
                    cx + lroundf(cosf(q) * 29), cy + lroundf(sinf(q) * 29));
      // Extra thickness for major ticks
      if (mark % 15 == 0) {
        u8g2.drawLine(cx + lroundf(cosf(q) * (inner-1)), cy + lroundf(sinf(q) * (inner-1)),
                      cx + lroundf(cosf(q) * 29), cy + lroundf(sinf(q) * 29));
      }
    }

    // Left subdial: seconds sweep (r=8, pos left of center)
    int lx = 44 + bx, ly = 32 + by;
    u8g2.drawCircle(lx, ly, 8);
    float sAng = t.tm_sec * 6.2832f / 60.0f - 1.5708f;
    u8g2.drawLine(lx, ly, lx + lroundf(cosf(sAng) * 6), ly + lroundf(sinf(sAng) * 6));
    u8g2.drawDisc(lx, ly, 1);

    // Right subdial: day-of-week pointer
    int rsx = 84 + bx, rsy = 32 + by;
    u8g2.drawCircle(rsx, rsy, 8);
    float dAng = t.tm_wday * 6.2832f / 7.0f - 1.5708f;
    u8g2.drawLine(rsx, rsy, rsx + lroundf(cosf(dAng) * 6), rsy + lroundf(sinf(dAng) * 6));
    u8g2.drawDisc(rsx, rsy, 1);

    // Main hands (smooth motion)
    float secA  = (t.tm_sec + (millis() % 1000) * 0.001f) * 6.2832f / 60.0f - 1.5708f;
    float minuA = (t.tm_min + t.tm_sec / 60.0f) * 6.2832f / 60.0f - 1.5708f;
    float hourA = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 6.2832f / 12.0f - 1.5708f;

    // Hour: thick (3 lines)
    for (int d = -1; d <= 1; d++)
      u8g2.drawLine(cx + d, cy, cx + lroundf(cosf(hourA) * 14) + d, cy + lroundf(sinf(hourA) * 14));
    // Minute
    u8g2.drawLine(cx, cy, cx + lroundf(cosf(minuA) * 22), cy + lroundf(sinf(minuA) * 22));
    // Second (thin + counterweight)
    u8g2.drawLine(cx, cy, cx + lroundf(cosf(secA) * 26), cy + lroundf(sinf(secA) * 26));
    u8g2.drawLine(cx, cy, cx - lroundf(cosf(secA) * 7), cy - lroundf(sinf(secA) * 7));
    // Centre disc
    u8g2.drawDisc(cx, cy, 2);

    // Digital time inset (12 o'clock area)
    u8g2.setFont(F_TINY);
    int tw4 = u8g2.getStrWidth(hm);
    u8g2.drawStr(cx - tw4 / 2, cy - 15 + by, hm);

    // Date at 6 o'clock
    char dStr[6]; snprintf(dStr, sizeof(dStr), "%d", t.tm_mday);
    u8g2.drawStr(cx - 3, cy + 23, dStr);

  } else if (!strcmp(style, "stacked")) {
    // ============================================================= 6. STACKED — Nike Typeface
    // Thin vertical separator
    u8g2.drawVLine(44 + bx, 0, 64);

    // Left column: day, date, month, temp
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(2 + bx, 10 + by, WDAY[t.tm_wday]);
    char dBuf[4]; snprintf(dBuf, sizeof(dBuf), "%d", t.tm_mday);
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(2 + bx, 28 + by, dBuf);
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(2 + bx, 42 + by, MONTH[t.tm_mon]);
    if (homeW.ok) {
      char tb[8]; snprintf(tb, sizeof(tb), "%d\xc2\xb0", (int)roundf(homeW.temp));
      u8g2.drawUTF8(2 + bx, 59 + by, tb);
    } else {
      u8g2.drawStr(2 + bx, 59 + by, ap);
    }

    // Right: HH / MM stacked in F_NUM32
    int h4 = t.tm_hour;
    if (!h24) { h4 %= 12; if (!h4) h4 = 12; }
    char hStr3[4], mStr3[4];
    snprintf(hStr3, sizeof(hStr3), "%02d", h4);
    snprintf(mStr3, sizeof(mStr3), "%02d", t.tm_min);

    u8g2.setFont(F_NUM32);
    drawDigits(1, hStr3, 50 + bx, 30 + by, 32, anim);
    u8g2.drawHLine(50 + bx, 33 + by, 76);  // separator between HH and MM
    drawDigits(2, mStr3, 50 + bx, 62 + by, 32, anim);

    // Blinking colon between HH and MM at the separator
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawBox(47 + bx, 28 + by, 2, 2);
      u8g2.drawBox(47 + bx, 35 + by, 2, 2);
    }

    // Orbiting second dot (2px disc) along perimeter
    int perim = (t.tm_sec * 380 + (millis() % 1000) * 380 / 1000) / 60;
    int odx, ody;
    if (perim < 127)      { odx = perim; ody = 0; }
    else if (perim < 190) { odx = 127; ody = perim - 127; }
    else if (perim < 317) { odx = 127 - (perim - 190); ody = 63; }
    else                  { odx = 0; ody = 63 - (perim - 317); }
    u8g2.drawDisc(odx + bx, ody + by, 2);

  } else if (!strcmp(style, "cube")) {
    // ============================================================= 7. 3D WIREFRAME CUBE
    float ax5 = millis() * 0.00085f, ay5 = millis() * 0.00110f, az5 = millis() * 0.00065f;
    float cax = cosf(ax5), sax = sinf(ax5), cay = cosf(ay5), say = sinf(ay5), caz = cosf(az5), saz = sinf(az5);
    int sx5[8], sy5[8];
    for (int i = 0; i < 8; i++) {
      float xv = (i & 1) ? 1.0f : -1.0f, yv = (i & 2) ? 1.0f : -1.0f, zv = (i & 4) ? 1.0f : -1.0f;
      float y1 = yv * cax - zv * sax, z1 = yv * sax + zv * cax;
      float x2 = xv * cay + z1 * say, z2 = -xv * say + z1 * cay;
      float x3 = x2 * caz - y1 * saz, y3 = x2 * saz + y1 * caz;
      float scale = 58.0f / (z2 + 4.0f);
      sx5[i] = 32 + bx + lroundf(x3 * scale);
      sy5[i] = 32 + by + lroundf(y3 * scale);
    }
    for (int i = 0; i < 8; i++) {
      for (int bit = 0; bit < 3; bit++) {
        int j = i ^ (1 << bit);
        if (i < j) u8g2.drawLine(sx5[i], sy5[i], sx5[j], sy5[j]);
      }
    }

    // Vertices as small dots for crispness
    for (int i = 0; i < 8; i++) u8g2.drawPixel(sx5[i], sy5[i]);

    // Right panel: clean time display
    int h5 = t.tm_hour;
    if (!h24) { h5 %= 12; if (!h5) h5 = 12; }
    char hStr4[4], mStr4[4];
    snprintf(hStr4, sizeof(hStr4), "%02d", h5);
    snprintf(mStr4, sizeof(mStr4), "%02d", t.tm_min);

    u8g2.drawVLine(70 + bx, 0, 64);
    u8g2.setFont(F_NUM24);
    drawDigits(1, hStr4, 76 + bx, 27 + by, 24, anim);
    u8g2.drawHLine(76 + bx, 30 + by, 50);
    drawDigits(2, mStr4, 76 + bx, 56 + by, 24, anim);

    u8g2.setFont(F_TINY);
    u8g2.drawUTF8(74 + bx, 7 + by, WDAY[t.tm_wday]);
    char sBuf2[6]; snprintf(sBuf2, sizeof(sBuf2), ":%02d", t.tm_sec);
    textRight(126 + bx, 62 + by, sBuf2);

    // Mini arc showing seconds progress (bottom right)
    float sFr5 = t.tm_sec / 60.0f;
    float sE5 = sFr5 * 6.2832f - 1.5708f;
    int arcCx = 113 + bx, arcCy = 44 + by, arcR = 7;
    u8g2.drawCircle(arcCx, arcCy, arcR);
    for (float a = -1.5708f; a < sE5 && a < 4.7124f; a += 0.18f)
      u8g2.drawPixel(arcCx + lroundf(cosf(a) * arcR), arcCy + lroundf(sinf(a) * arcR));

  } else if (!strcmp(style, "matrix")) {
    // ============================================================= 8. MATRIX RAIN
    uint32_t now6 = millis();
    static const int8_t laneX[16]    = {2,10,18,27,35,43,52,60,68,76,85,93,101,110,118,126};
    static const uint8_t laneSpd[16] = {29,53,37,71,43,31,67,47,83,59,41,73,37,61,53,43};
    for (int lane = 0; lane < 16; lane++) {
      uint32_t tick = now6 / laneSpd[lane];
      int head6 = (int)(tick * 3) % 90 - 20;
      for (int n = 0; n < 8; n++) {
        int y6 = head6 - n * 7 + by;
        if (y6 < 0 || y6 >= 64) continue;
        int x6 = laneX[lane] + bx;
        if (n == 0)      u8g2.drawBox(x6, y6, 3, 4);  // bright head
        else if (n == 1) u8g2.drawBox(x6, y6, 2, 2);  // secondary
        else             u8g2.drawPixel(x6, y6);        // tail
      }
    }

    // Time plate with double-border effect
    u8g2.setFont(F_NUM24);
    int tw5 = u8g2.getStrWidth(hm);
    int plateW = tw5 + 24, plateX = (128 - plateW) / 2 + bx;
    u8g2.setDrawColor(0); u8g2.drawRBox(plateX - 3, 13 + by, plateW + 6, 36, 4); u8g2.setDrawColor(1);
    u8g2.drawRFrame(plateX - 3, 13 + by, plateW + 6, 36, 4);
    u8g2.drawRFrame(plateX - 1, 15 + by, plateW + 2, 32, 3);  // inner border
    drawDigits(0, hm, plateX + 3, 40 + by, 24, anim);

    // Seconds and date in plate corners
    u8g2.setFont(F_TINY);
    char secStr6[6]; snprintf(secStr6, sizeof(secStr6), "%02d", t.tm_sec);
    u8g2.drawStr(plateX + tw5 + 5, 27 + by, secStr6);
    textCenter(60 + by, date);

  } else if (!strcmp(style, "scan")) {
    // ============================================================= 9. RADAR SCAN
    uint32_t now7 = millis();
    int rcx = 32 + bx, rcy = 32 + by, rr = 28;

    // Circle + cross-hair
    u8g2.drawCircle(rcx, rcy, rr);
    u8g2.drawCircle(rcx, rcy, rr / 2);
    u8g2.drawPixel(rcx, rcy);
    for (int i = 0; i < rr; i += 4) {
      u8g2.drawPixel(rcx + i, rcy);
      u8g2.drawPixel(rcx - i, rcy);
      u8g2.drawPixel(rcx, rcy + i);
      u8g2.drawPixel(rcx, rcy - i);
    }

    // Rotating sweep line with fading trail
    float sweepA = (now7 % 3000) * 6.2832f / 3000.0f - 1.5708f;
    for (int seg = 0; seg < 5; seg++) {
      float ta = sweepA - seg * 0.25f;
      if (seg > 0) {
        // Draw fading trail pixels
        for (int d = 4; d < rr - 2; d += 2)
          u8g2.drawPixel(rcx + lroundf(cosf(ta) * d), rcy + lroundf(sinf(ta) * d));
      } else {
        // Bright main sweep line
        u8g2.drawLine(rcx, rcy, rcx + lroundf(cosf(ta) * (rr - 1)), rcy + lroundf(sinf(ta) * (rr - 1)));
      }
    }

    // Ping dot on radar edge moving with seconds
    float pingA = t.tm_sec * 6.2832f / 60.0f - 1.5708f;
    u8g2.drawDisc(rcx + lroundf(cosf(pingA) * (rr - 3)), rcy + lroundf(sinf(pingA) * (rr - 3)), 2);

    // Right panel: time + date
    u8g2.drawVLine(68 + bx, 0, 64);
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(72 + bx, 24 + by, hm);
    u8g2.setFont(F_TINY);
    u8g2.drawUTF8(72 + bx, 8 + by, date);
    char sBuf3[6]; snprintf(sBuf3, sizeof(sBuf3), ":%02d", t.tm_sec);
    u8g2.drawStr(72 + bx, 36 + by, sBuf3);
    u8g2.drawHLine(72 + bx, 39 + by, 55);
    u8g2.drawStr(72 + bx, 50 + by, "RADAR");
    u8g2.drawStr(72 + bx, 60 + by, "ACTIVE");

  } else if (!strcmp(style, "data")) {
    // ============================================================= 10. CYBERPUNK DATA HUD
    u8g2.setFont(F_TINY);
    // Header line
    u8g2.drawStr(2 + bx, 7 + by, "ESP32-C3 SUPER MINI");
    if (netOnline()) {
      char rb[16]; snprintf(rb, sizeof(rb), "%ddBm", netRSSI());
      textRight(126 + bx, 7 + by, rb);
    } else textRight(126 + bx, 7 + by, "OFFLINE");
    u8g2.drawHLine(0, 9 + by, 128);

    // Terminal lines
    u8g2.setFont(F_SMALL);
    char tl[28]; snprintf(tl, sizeof(tl), ">> %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    u8g2.drawStr(2 + bx, 20 + by, tl);

    char dl[32]; snprintf(dl, sizeof(dl), ">> %s %02d/%02d/%04d",
      WDAY[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    u8g2.drawStr(2 + bx, 31 + by, dl);

    if (homeW.ok) {
      char wl[36];
      const char* tu = (cfg["units"] == "imperial") ? "F" : "C";
      snprintf(wl, sizeof(wl), ">> %d\xc2\xb0%s  %s", (int)roundf(homeW.temp), tu, wmoText(homeW.code));
      u8g2.drawUTF8(2 + bx, 42 + by, wl);
    } else u8g2.drawStr(2 + bx, 42 + by, ">> WEATHER OFFLINE");

    // Blinking cursor
    bool cur = (millis() % 600) < 300;
    if (cur) {
      u8g2.setFont(F_TINY);
      u8g2.drawStr(2 + bx, 51 + by, "_");
    }
    u8g2.drawHLine(0, 51 + by, 128);

    // Status bar: heap + seconds loading bar
    char ml[20]; snprintf(ml, sizeof(ml), "MEM %uK", (unsigned)(ESP.getFreeHeap() / 1024));
    u8g2.setFont(F_TINY);
    u8g2.drawStr(2 + bx, 62 + by, ml);
    // Seconds bar on right
    drawBar(60 + bx, 55 + by, 66, 6, t.tm_sec / 60.0f);
    char sp2[6]; snprintf(sp2, sizeof(sp2), "%02ds", t.tm_sec);
    // show percent inside bar area conceptually
    u8g2.drawStr(60 + bx + 68, 62 + by, sp2);  // just past bar

  } else if (!strcmp(style, "normal")) {
    // ============================================================= 11. CLEAN CLASSIC
    // Top: bold day + date
    u8g2.setFont(F_BOLD);
    u8g2.drawUTF8(2 + bx, 10 + by, WDAY_LONG[t.tm_wday]);
    u8g2.setFont(F_SMALL);
    char dLine[20]; snprintf(dLine, sizeof(dLine), "%d %s", t.tm_mday, MONTH_LONG[t.tm_mon]);
    textRight(126 + bx, 10 + by, dLine);
    u8g2.drawHLine(0, 12 + by, 128);

    // Centre: huge time
    int h6 = t.tm_hour;
    if (!h24) { h6 %= 12; if (!h6) h6 = 12; }
    char hStr5[4], mStr5[4];
    snprintf(hStr5, sizeof(hStr5), "%02d", h6);
    snprintf(mStr5, sizeof(mStr5), "%02d", t.tm_min);
    u8g2.setFont(F_NUM24);
    int hw6 = u8g2.getStrWidth(hStr5), mw6 = u8g2.getStrWidth(mStr5);
    int cX6 = (128 - hw6 - 4 - mw6) / 2 + bx;
    drawDigits(0, hStr5, cX6,          40 + by, 24, anim);
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawBox(cX6 + hw6, 25 + by, 2, 2);
      u8g2.drawBox(cX6 + hw6, 32 + by, 2, 2);
    }
    drawDigits(1, mStr5, cX6 + hw6 + 4, 40 + by, 24, anim);

    // Seconds tag right-aligned
    u8g2.setFont(F_NUM16);
    char sBuf4[6]; snprintf(sBuf4, sizeof(sBuf4), ":%02d", t.tm_sec);
    textRight(126 + bx, 40 + by, sBuf4);

    // Bottom separator + info
    u8g2.drawHLine(0, 44 + by, 128);

    // 1px second progress line
    int secLine6 = (t.tm_sec * 128) / 60;
    if (secLine6 > 0) u8g2.drawHLine(0, 45 + by, secLine6);

    u8g2.setFont(F_TINY);
    if (netOnline()) {
      char nb[20]; snprintf(nb, sizeof(nb), "WiFi %ddBm", netRSSI());
      u8g2.drawStr(2 + bx, 60 + by, nb);
    } else u8g2.drawStr(2 + bx, 60 + by, "OFFLINE");

    if (homeW.ok) {
      char tb[8]; snprintf(tb, sizeof(tb), "%d\xc2\xb0", (int)roundf(homeW.temp));
      textRight(126 + bx, 60 + by, tb);
    } else {
      char hb[12]; snprintf(hb, sizeof(hb), "HEAP %uK", (unsigned)(ESP.getFreeHeap()/1024));
      textRight(126 + bx, 60 + by, hb);
    }

  } else {
    // ============================================================= 12. BIG TIME
    int h7 = t.tm_hour;
    if (!h24) { h7 %= 12; if (!h7) h7 = 12; }
    char hStr6[4], mStr6[4];
    snprintf(hStr6, sizeof(hStr6), "%02d", h7);
    snprintf(mStr6, sizeof(mStr6), "%02d", t.tm_min);

    // Giant digits — draw HH and MM separately to allow animated transitions
    u8g2.setFont(F_NUM42);
    int hw7 = u8g2.getStrWidth(hStr6), mw7 = u8g2.getStrWidth(mStr6);
    int gap7 = 8;
    int cX7 = (128 - hw7 - gap7 - mw7) / 2 + bx;
    if (cX7 < 0) cX7 = 0;
    drawDigits(0, hStr6, cX7,            48 + by, 42, anim);
    drawDigits(1, mStr6, cX7 + hw7 + gap7, 48 + by, 42, anim);

    // Blinking colon
    if ((millis() / 500) % 2 == 0) {
      u8g2.drawBox(cX7 + hw7 + 1, 20 + by, 3, 3);
      u8g2.drawBox(cX7 + hw7 + 1, 33 + by, 3, 3);
    }

    // AM/PM badge (12h only) — top right corner
    if (!h24) {
      u8g2.setFont(F_TINY);
      u8g2.drawRFrame(108 + bx, 1 + by, 18, 9, 2);
      u8g2.drawStr(111 + bx, 8 + by, ap);
    }

    // Weather / temp — top left
    if (homeW.ok) {
      char tb2[8]; snprintf(tb2, sizeof(tb2), "%d\xc2\xb0", (int)roundf(homeW.temp));
      u8g2.setFont(F_TINY);
      u8g2.drawUTF8(2 + bx, 8 + by, tb2);
    }

    // Thin separator line + date + dotted second progress
    u8g2.drawHLine(0, 51 + by, 128);
    char fullDate[36];
    snprintf(fullDate, sizeof(fullDate), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
    u8g2.setFont(F_MED);
    textCenter(62 + by, fullDate);

    // Dotted seconds progress at very bottom
    int dotSec = (t.tm_sec * 128) / 60;
    for (int dp = 0; dp < dotSec; dp += 3) u8g2.drawPixel(dp + bx, 63 + by);
  }
}

static bool clock_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG || g == G_TRIPLE) {
    if (g == G_LONG) {
      c.st[0] = (c.st[0] + 1) % NUM_CLOCK_STYLES;
    } else {
      c.st[0] = (c.st[0] + NUM_CLOCK_STYLES - 1) % NUM_CLOCK_STYLES;
    }
    c.st[1] = 1;
    const char* newStyle = CLOCK_STYLES[c.st[0]];
    c.p["style"] = newStyle;
    markDirty(DIRTY_CARDS);
    cfg["clock"]["style"] = newStyle;
    markDirty(DIRTY_CFG);
    saveNow();
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
