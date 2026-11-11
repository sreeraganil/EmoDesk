#include "qrcode.h"
#include "display.h"
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
int8_t burnX = 0, burnY = 0;
static bool dispOn = true;
static uint32_t lastBurn = 0;
static int batReserve = 0;

static uint8_t lastSentBuf[1024];
static bool hasSentFrame = false;
static uint32_t lastSendMs = 0;

void displayBegin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.setBusClock(OLED_I2C_HZ);
  u8g2.begin();
  Wire.setTimeOut(25);
  Wire.setClock(OLED_I2C_HZ);
  u8g2.enableUTF8Print();
  u8g2.setFontMode(1);
  displayApply();
}

void displayApply() {
  u8g2.setFlipMode((cfg["display"]["flip"] | false) ? 1 : 0);
  u8g2.sendF("c", (cfg["display"]["invert"] | false) ? 0xA7 : 0xA6);
  displaySetContrast(cfg["display"]["bright"] | 200);
  hasSentFrame = false;
}

void displaySetContrast(uint8_t c) {
  static int last = -1;
  if (c == last) return;
  last = c;
  u8g2.setContrast(c);
}

void displaySetBatReserve(int w) { batReserve = max(0, w); }
int  displayBatReserve() { return batReserve; }

void displaySend() {
  if (cfg["display"]["burn"] | true) {
    if (millis() - lastBurn > 120000) {
      lastBurn = millis();
      burnX = (int8_t)(esp_random() % 5) - 2;
      burnY = (int8_t)(esp_random() % 3) - 1;
    }
  } else { burnX = burnY = 0; }
  if (!dispOn) return;
  uint8_t* buf = u8g2.getBufferPtr();
  uint32_t now = millis();
  if (!buf || !hasSentFrame || memcmp(buf, lastSentBuf, 1024) != 0 || (now - lastSendMs >= 2000)) {
    u8g2.sendBuffer();
    if (buf) memcpy(lastSentBuf, buf, 1024);
    hasSentFrame = true;
    lastSendMs = now;
  }
}

void displayPower(bool on) {
  if (on == dispOn) return;
  dispOn = on;
  hasSentFrame = false;
  u8g2.setPowerSave(on ? 0 : 1);
}

bool displayIsOn() { return dispOn; }

// ------------------------------------------------------------------ text
static void fitBuf(char* buf, size_t cap, int maxW, const char* s) {
  if (!buf || !cap) return;
  safeCopy(buf, s ? s : "", cap);
  if (maxW <= 0) { buf[0] = 0; return; }
  if (u8g2.getUTF8Width(buf) <= maxW) return;
  int dw = u8g2.getUTF8Width("..");
  int len = (int)strlen(buf);
  while (len > 0) {
    len--;
    while (len > 0 && ((uint8_t)buf[len] & 0xC0) == 0x80) len--;
    buf[len] = 0;
    if (u8g2.getUTF8Width(buf) + dw <= maxW) break;
  }
  if (len + 2 < (int)cap && u8g2.getUTF8Width("..") <= maxW) strcat(buf, "..");
}

void textCenter(int y, const char* s) {
  if (!s) return;
  int w = u8g2.getUTF8Width(s);
  u8g2.drawUTF8((128 - w) / 2, y, s);
}

void textRight(int xr, int y, const char* s) {
  if (!s) return;
  u8g2.drawUTF8(xr - u8g2.getUTF8Width(s), y, s);
}

void drawTextFit(int x, int y, int maxW, const char* s) {
  if (!s || !*s || maxW <= 4) return;
  char buf[72];
  fitBuf(buf, sizeof(buf), maxW, s);
  u8g2.drawUTF8(x, y, buf);
}

void drawTextFitCenter(int cx, int y, int maxW, const char* s) {
  if (!s || !*s || maxW <= 4) return;
  char buf[72];
  fitBuf(buf, sizeof(buf), maxW, s);
  int w = u8g2.getUTF8Width(buf);
  u8g2.drawUTF8(cx - w / 2, y, buf);
}

void drawTextRightFit(int xr, int y, int maxW, const char* s) {
  if (!s || !*s || maxW <= 4) return;
  char buf[72];
  fitBuf(buf, sizeof(buf), maxW, s);
  u8g2.drawUTF8(xr - u8g2.getUTF8Width(buf), y, buf);
}

int wrapText(int x, int y, int w, int lineH, const char* s, int maxLines, int skip) {
  char buf[100];
  int line = 0;
  const char* p = s;
  while (p && *p) {
    while (*p == ' ') p++;
    if (!*p) break;
    int len = 0, lastSpace = -1;
    while (p[len] && p[len] != '\n' && len < 98) {
      int next = len + 1;
      while (p[next] && ((uint8_t)p[next] & 0xC0) == 0x80) next++;   // whole UTF-8 char
      memcpy(buf, p, next);
      buf[next] = 0;
      if (u8g2.getUTF8Width(buf) > w) break;
      if (p[len] == ' ') lastSpace = len;
      len = next;
    }
    int take = len;
    if (p[len] && p[len] != '\n' && p[len] != ' ' && lastSpace > 0) take = lastSpace;
    if (take <= 0) take = 1;
    memcpy(buf, p, take);
    buf[take] = 0;
    if (line >= skip && line < skip + maxLines) u8g2.drawUTF8(x, y + (line - skip) * lineH, buf);
    line++;
    p += take;
    if (*p == '\n') p++;
  }
  return line;
}

void marquee(int x, int y, int w, const char* s, uint32_t t0, int pxPerSec) {
  if (!s || !*s || w <= 0) return;
  int tw = u8g2.getUTF8Width(s);
  if (tw <= w) { u8g2.drawUTF8(x, y, s); return; }
  int span = tw + 28;
  uint32_t el = millis() - t0;
  int off = el < 1300 ? 0 : (int)(((el - 1300) * pxPerSec / 1000) % span);
  int x0 = constrain(x, 0, 127);
  int x1 = constrain(x + w, x0 + 1, 128);
  u8g2.setClipWindow(x0, 0, x1, 64);
  u8g2.drawUTF8(x - off, y, s);
  u8g2.drawUTF8(x - off + span, y, s);
  u8g2.setMaxClipWindow();
}

void drawBar(int x, int y, int w, int h, float f) {
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  int r = h >= 6 ? 2 : 1;
  u8g2.drawRFrame(x, y, w, h, r);
  int maxFill = w - 4;
  if (maxFill > 0) {
    int fw = (int)(maxFill * f + 0.5f);
    if (fw > 0) u8g2.drawBox(x + 2, y + 2, min(fw, maxFill), max(1, h - 4));
  }
}

void drawDottedHLine(int x, int y, int w) {
  for (int i = 0; i < w; i += 2) u8g2.drawPixel(x + i, y);
}

void drawDottedVLine(int x, int y, int h) {
  for (int i = 0; i < h; i += 2) u8g2.drawPixel(x, y + i);
}

void drawBadge(int xr, int y, const char* text) {
  if (!text || !*text) return;
  u8g2.setFont(F_TINY);
  int tw = u8g2.getUTF8Width(text);
  int bw = tw + 6;
  int bx = xr - bw;
  u8g2.setDrawColor(0);
  u8g2.drawRBox(bx - 1, y - 7, bw + 2, 10, 2);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(bx, y - 6, bw, 8, 2);
  u8g2.drawUTF8(bx + 3, y, text);
}

void drawPill(int x, int y, int w, int h, const char* text, bool filled) {
  if (!text || !*text) return;
  int r = h >= 8 ? 2 : 1;
  if (filled) {
    u8g2.drawRBox(x, y, w, h, r);
    u8g2.setDrawColor(0);
    int tw = u8g2.getUTF8Width(text);
    u8g2.drawUTF8(x + (w - tw) / 2, y + h - 2, text);
    u8g2.setDrawColor(1);
  } else {
    u8g2.drawRFrame(x, y, w, h, r);
    int tw = u8g2.getUTF8Width(text);
    u8g2.drawUTF8(x + (w - tw) / 2, y + h - 2, text);
  }
}

void drawSignalBars(int x, int y, int rssi) {
  int bars = rssi > -55 ? 4 : (rssi > -68 ? 3 : (rssi > -80 ? 2 : (rssi > -92 ? 1 : 0)));
  for (int i = 0; i < 4; i++) {
    int bh = 2 + i * 2;
    int by = y - bh;
    int bx = x + i * 4;
    if (i < bars) u8g2.drawBox(bx, by, 3, bh);
    else u8g2.drawBox(bx, y - 1, 2, 1);
  }
}

void drawHeader(const char* title, const char* right) {
  int rightEdge = 127 - batReserve;
  bool hasRight = (right && *right);
  int rightX = rightEdge;
  bool framed = false;
  int bw = 0, bx = rightEdge;
  char rBuf[24] = "";

  if (hasRight) {
    u8g2.setFont(F_TINY);
    int maxRW = max(20, rightEdge - 48);
    fitBuf(rBuf, sizeof(rBuf), maxRW, right);
    int rw = u8g2.getUTF8Width(rBuf);
    bw = rw + 6;
    bx = rightEdge - bw;
    framed = (bw <= 54 && bx >= 36);
    rightX = framed ? (bx - 2) : (rightEdge - rw - 2);
  }

  int maxTitleW = hasRight ? (rightX - 2) : (rightEdge - 2);
  if (maxTitleW < 18) maxTitleW = 18;

  if (title && *title) {
    u8g2.setFont(F_SMALL);
    if (u8g2.getUTF8Width(title) <= maxTitleW) {
      u8g2.drawUTF8(1, 8, title);
    } else {
      u8g2.setFont(F_TINY);
      drawTextFit(1, 8, maxTitleW, title);
    }
  }

  if (hasRight && rBuf[0]) {
    u8g2.setFont(F_TINY);
    bool inverse = (!strcasecmp(rBuf, "LIVE") || !strcasecmp(rBuf, "FT") || !strcasecmp(rBuf, "PROB") ||
                    !strcasecmp(rBuf, "ALERT") || !strcasecmp(rBuf, "RUN") || !strcasecmp(rBuf, "PLAY") ||
                    !strcasecmp(rBuf, "ON") || !strcasecmp(rBuf, "AP") || !strcasecmp(rBuf, "BUSY") ||
                    !strcasecmp(rBuf, "ONLINE") || !strcasecmp(rBuf, "NEW"));
    if (inverse) {
      u8g2.drawRBox(bx, 1, bw, 8, 2);
      u8g2.setDrawColor(0);
      u8g2.drawUTF8(bx + 3, 7, rBuf);
      u8g2.setDrawColor(1);
    } else if (framed) {
      u8g2.drawRFrame(bx, 1, bw, 8, 2);
      u8g2.drawUTF8(bx + 3, 7, rBuf);
    } else {
      textRight(rightEdge, 8, rBuf);
    }
  }
  u8g2.drawHLine(0, 10, 128);
}

void drawSpark(int x, int y, int w, int h, const float* v, int n) {
  if (n < 2 || w < 4 || h < 4) return;
  float mn = v[0], mx = v[0];
  for (int i = 1; i < n; i++) { mn = min(mn, v[i]); mx = max(mx, v[i]); }
  float rng = mx - mn;
  if (rng <= 0.00001f) rng = 1.0f;
  int px = x, py = y + h - 1 - (int)((v[0] - mn) / rng * (h - 1));
  for (int i = 1; i < n; i++) {
    int nx = x + i * (w - 1) / (n - 1);
    int ny = y + h - 1 - (int)((v[i] - mn) / rng * (h - 1));
    u8g2.drawLine(px, py, nx, ny);
    px = nx; py = ny;
  }
}

void drawCenteredMsg(const char* l1, const char* l2) {
  if (!l1) return;
  char b1[44], b2[48];
  u8g2.setFont(F_MED);
  fitBuf(b1, sizeof(b1), 110, l1);
  int w1 = u8g2.getUTF8Width(b1);
  int w2 = 0;
  if (l2 && *l2) {
    u8g2.setFont(F_SMALL);
    fitBuf(b2, sizeof(b2), 112, l2);
    w2 = u8g2.getUTF8Width(b2);
  }
  int boxW = constrain(max(w1, w2) + 16, 76, 124);
  int boxH = (l2 && *l2) ? 38 : 26;
  int bx = (128 - boxW) / 2;
  int by = (64 - boxH) / 2 + 2;
  if (by < 13) by = (64 - boxH) / 2;
  u8g2.drawRFrame(bx, by, boxW, boxH, 3);
  u8g2.drawPixel(bx + 3, by + 3);
  u8g2.drawPixel(bx + boxW - 4, by + 3);
  u8g2.drawPixel(bx + 3, by + boxH - 4);
  u8g2.drawPixel(bx + boxW - 4, by + boxH - 4);

  int y1 = (l2 && *l2) ? by + 16 : by + 17;
  u8g2.setFont(F_MED);
  textCenter(y1, b1);
  if (l2 && *l2) {
    u8g2.setFont(F_SMALL);
    textCenter(y1 + 13, b2);
  }
}

// ------------------------------------------------------------------ weather
int wmoClass(int c) {
  if (c == 0) return 0;
  if (c <= 2) return 1;
  if (c == 3) return 2;
  if (c == 45 || c == 48) return 3;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return 4;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return 5;
  if (c >= 95) return 6;
  return 2;
}

const char* wmoText(int c) {
  static const char* const T[] = {"Clear", "Partly cloudy", "Cloudy", "Fog", "Rain", "Snow", "Thunderstorm"};
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 80 && c <= 82) return "Showers";
  return T[wmoClass(c)];
}

static void sun(int cx, int cy, int r) {
  u8g2.drawDisc(cx, cy, max(2, r * 45 / 100));
  u8g2.drawCircle(cx, cy, r * 65 / 100);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0f;
    int x1 = cx + (int)(cosf(a) * (r * 72 / 100));
    int y1 = cy + (int)(sinf(a) * (r * 72 / 100));
    int x2 = cx + (int)(cosf(a) * r);
    int y2 = cy + (int)(sinf(a) * r);
    u8g2.drawLine(x1, y1, x2, y2);
  }
}

static void moon(int cx, int cy, int r) {
  u8g2.drawDisc(cx, cy, r);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(cx + r / 2 + 1, cy - r / 3, r - 1);
  u8g2.setDrawColor(1);
  u8g2.drawPixel(cx - r + 1, cy - r / 2);
  u8g2.drawPixel(cx + r, cy + r / 2);
}

static void cloud(int x, int y, int w, int h) {
  int r1 = max(3, h * 30 / 100);
  int r2 = max(4, h * 40 / 100);
  int r3 = max(2, h * 24 / 100);
  u8g2.drawRBox(x, y + h / 2, w, h / 2 + 1, max(1, h / 4));
  u8g2.drawDisc(x + w * 28 / 100, y + h * 55 / 100, r1);
  u8g2.drawDisc(x + w * 58 / 100, y + h * 44 / 100, r2);
  u8g2.drawDisc(x + w * 82 / 100, y + h * 60 / 100, r3);
}

void drawWeatherIcon(int x, int y, int s, int code, bool day) {
  int k = wmoClass(code);
  switch (k) {
    case 0:
      if (day) sun(x + s / 2, y + s / 2, s / 2 - 1);
      else moon(x + s / 2, y + s / 2, s / 2 - 3);
      break;
    case 1:
      if (day) sun(x + s / 3, y + s / 3, s / 3);
      else moon(x + s / 3, y + s / 3, s / 4);
      u8g2.setDrawColor(0);
      cloud(x + s / 4 - 1, y + s / 3 - 1, s * 3 / 4 + 2, s / 2 + 2);
      u8g2.setDrawColor(1);
      cloud(x + s / 4, y + s / 3, s * 3 / 4, s / 2);
      break;
    case 2:
      cloud(x, y + s / 6, s, s * 2 / 3);
      break;
    case 3:
      for (int i = 0; i < 4; i++) {
        int off = (i % 2) * (s / 8);
        u8g2.drawHLine(x + off, y + s / 4 + i * (s / 5), s - s / 6);
      }
      break;
    case 4:
      cloud(x, y, s, s * 55 / 100);
      for (int i = 0; i < 3; i++) {
        int rx = x + s * 25 / 100 + i * s * 25 / 100;
        int ry = y + s * 70 / 100;
        u8g2.drawLine(rx, ry, rx - 2, ry + 4);
      }
      break;
    case 5:
      cloud(x, y, s, s * 55 / 100);
      for (int i = 0; i < 3; i++) {
        int sx = x + s * 25 / 100 + i * s * 25 / 100;
        int sy = y + s * 75 / 100 + (i % 2) * 2;
        u8g2.drawPixel(sx, sy);
        u8g2.drawPixel(sx - 1, sy);
        u8g2.drawPixel(sx + 1, sy);
        u8g2.drawPixel(sx, sy - 1);
        u8g2.drawPixel(sx, sy + 1);
      }
      break;
    case 6: {
      cloud(x, y, s, s * 52 / 100);
      int cx = x + s / 2;
      int ty = y + s * 58 / 100;
      u8g2.drawLine(cx + 3, ty, cx - 1, ty + 5);
      u8g2.drawLine(cx - 1, ty + 5, cx + 2, ty + 5);
      u8g2.drawLine(cx + 2, ty + 5, cx - 2, ty + 11);
      break;
    }
  }
}

void drawQRCode(int x, int y, const char* text, uint8_t scale) {
  if (!text || !*text) return;
  QRCode qr;
  uint8_t data[qrcode_getBufferSize(4)];
  int ok = -1;
  for (int v = 2; v <= 4 && ok != 0; v++) ok = qrcode_initText(&qr, data, v, ECC_LOW, text);
  if (ok != 0) return;
  int px = qr.size * scale;
  u8g2.setDrawColor(1);
  u8g2.drawBox(x - 2, y - 2, px + 4, px + 4);
  u8g2.setDrawColor(0);
  for (int yy = 0; yy < qr.size; yy++) {
    for (int xx = 0; xx < qr.size; xx++) {
      if (qrcode_getModule(&qr, xx, yy)) {
        u8g2.drawBox(x + xx * scale, y + yy * scale, scale, scale);
      }
    }
  }
  u8g2.setDrawColor(1);
}

void drawIconBall(int cx, int cy, int r, uint8_t fg) {
  u8g2.setDrawColor(fg);
  u8g2.drawDisc(cx, cy, r);
  u8g2.setDrawColor(!fg);
  u8g2.drawDisc(cx, cy, r - 1);
  u8g2.setDrawColor(fg);
  int pr = max(1, r / 3);
  u8g2.drawDisc(cx, cy, pr);
  for (int i = 0; i < 5; i++) {
    float a = -PI / 2.0f + i * 2.0f * PI / 5.0f;
    u8g2.drawLine(cx + cosf(a) * pr, cy + sinf(a) * pr, cx + cosf(a) * (r - 1), cy + sinf(a) * (r - 1));
  }
  u8g2.setDrawColor(1);
}

void drawIconBell(int x, int y) {
  u8g2.drawDisc(x + 4, y + 3, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawBox(x + 1, y + 3, 7, 3);
  u8g2.drawBox(x, y + 6, 9, 1);
  u8g2.drawBox(x + 3, y + 7, 3, 1);
}

void drawIconMail(int x, int y) {
  u8g2.drawFrame(x, y, 11, 8);
  u8g2.drawLine(x, y, x + 5, y + 4);
  u8g2.drawLine(x + 10, y, x + 5, y + 4);
}

void drawIconBat(int x, int y, int pct) {
  u8g2.drawFrame(x, y, 11, 6);
  u8g2.drawBox(x + 11, y + 2, 1, 2);
  int w = constrain(pct, 0, 100) * 9 / 100;
  if (w > 0) u8g2.drawBox(x + 1, y + 1, w, 4);
}

void drawIconDrop(int x, int y) {
  u8g2.drawPixel(x + 2, y);
  u8g2.drawHLine(x + 1, y + 1, 3);
  u8g2.drawHLine(x + 1, y + 2, 3);
  u8g2.drawBox(x, y + 3, 5, 3);
  u8g2.drawHLine(x + 1, y + 6, 3);
}

void drawIconWind(int x, int y) {
  u8g2.drawHLine(x, y + 1, 6);
  u8g2.drawPixel(x + 6, y);
  u8g2.drawHLine(x + 2, y + 4, 5);
  u8g2.drawPixel(x + 7, y + 3);
}

void drawIconTemp(int x, int y) {
  u8g2.drawBox(x + 1, y, 3, 5);
  u8g2.drawDisc(x + 2, y + 6, 2);
}

void drawIconArrow(int x, int y, bool up) {
  if (up) {
    u8g2.drawPixel(x + 2, y);
    u8g2.drawHLine(x + 1, y + 1, 3);
    u8g2.drawHLine(x, y + 2, 5);
    u8g2.drawVLine(x + 2, y + 3, 3);
  } else {
    u8g2.drawVLine(x + 2, y, 3);
    u8g2.drawHLine(x, y + 3, 5);
    u8g2.drawHLine(x + 1, y + 4, 3);
    u8g2.drawPixel(x + 2, y + 5);
  }
}

void drawIconCalendar(int x, int y) {
  u8g2.drawFrame(x, y + 1, 8, 7);
  u8g2.drawPixel(x + 2, y);
  u8g2.drawPixel(x + 5, y);
  u8g2.drawHLine(x, y + 3, 8);
  u8g2.drawPixel(x + 2, y + 5);
  u8g2.drawPixel(x + 5, y + 5);
}

void drawIconClock(int x, int y) {
  u8g2.drawCircle(x + 3, y + 3, 3);
  u8g2.drawVLine(x + 3, y + 1, 3);
  u8g2.drawHLine(x + 3, y + 3, 3);
}

void drawIconHeart(int x, int y) {
  u8g2.drawHLine(x + 1, y, 2); u8g2.drawHLine(x + 4, y, 2);
  u8g2.drawHLine(x, y + 1, 7);
  u8g2.drawHLine(x, y + 2, 7);
  u8g2.drawHLine(x + 1, y + 3, 5);
  u8g2.drawHLine(x + 2, y + 4, 3);
  u8g2.drawPixel(x + 3, y + 5);
}

void drawIconStar(int x, int y) {
  u8g2.drawPixel(x + 2, y);
  u8g2.drawHLine(x, y + 1, 5);
  u8g2.drawHLine(x + 1, y + 2, 3);
  u8g2.drawPixel(x + 1, y + 3); u8g2.drawPixel(x + 3, y + 3);
}

void drawIconMusic(int x, int y) {
  u8g2.drawDisc(x + 1, y + 5, 1);
  u8g2.drawVLine(x + 2, y + 1, 5);
  u8g2.drawLine(x + 2, y + 1, x + 5, y);
  u8g2.drawVLine(x + 5, y, 4);
  u8g2.drawDisc(x + 4, y + 4, 1);
}

void drawPageDots(int xr, int y, int n, int cur) {
  if (n < 2) return;
  if (n > 8) n = 8;
  int x = xr - (n - 1) * 5;
  for (int i = 0; i < n; i++) {
    if (i == cur) u8g2.drawBox(x + i * 5 - 1, y - 1, 3, 3);
    else u8g2.drawPixel(x + i * 5, y);
  }
}
