#include "cards.h"
#include <WiFi.h>

// ================================================================== matrix rain
struct Mx { bool init; float y[21], sp[21]; uint8_t len[21]; char ch[21][8]; };
static void matrix_draw(CardCtx& c) {
  Mx* m = (Mx*)c.d;
  if (!m->init) {
    m->init = true;
    for (int i = 0; i < 21; i++) { m->y[i] = -(float)(esp_random() % 64); m->sp[i] = 0.6f + (esp_random() % 100) / 60.0f; m->len[i] = 3 + esp_random() % 5; }
  }
  u8g2.setFont(F_TINY);
  for (int i = 0; i < 21; i++) {
    m->y[i] += m->sp[i];
    if (m->y[i] - m->len[i] * 7 > 64) { m->y[i] = -(float)(esp_random() % 30); m->sp[i] = 0.6f + (esp_random() % 100) / 60.0f; }
    for (int k = 0; k < m->len[i]; k++) {
      if (esp_random() % 12 == 0 || !m->ch[i][k]) m->ch[i][k] = 33 + esp_random() % 90;
      int y = (int)m->y[i] - k * 7;
      if (y < 0 || y > 70) continue;
      char s[2] = {m->ch[i][k], 0};
      if (k == 0) { u8g2.drawBox(i * 6, y - 6, 5, 7); u8g2.setDrawColor(0); u8g2.drawStr(i * 6 + 1, y, s); u8g2.setDrawColor(1); }
      else if (k < m->len[i] - 1 || (millis() / 100) % 2) u8g2.drawStr(i * 6 + 1, y, s);
    }
  }
}
const CardType CT_matrix = {"matrix", sizeof(Mx), 0, 0, nullptr, matrix_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== starfield
struct Stars { bool init; float x[48], y[48], z[48]; };
static void starfield_draw(CardCtx& c) {
  Stars* s = (Stars*)c.d;
  for (int i = 0; i < 48; i++) {
    if (!s->init || s->z[i] < 1) {
      s->x[i] = ((int)(esp_random() % 200) - 100);
      s->y[i] = ((int)(esp_random() % 120) - 60);
      s->z[i] = s->init ? 64 : 1 + esp_random() % 64;
    }
    s->z[i] -= 1.2f;
    if (s->z[i] < 1) continue;
    int px = 64 + (int)(s->x[i] * 32 / s->z[i]), py = 32 + (int)(s->y[i] * 32 / s->z[i]);
    if (px < 0 || px > 127 || py < 0 || py > 63) { s->z[i] = 0; continue; }
    if (s->z[i] < 16) u8g2.drawBox(px, py, 2, 2);
    else u8g2.drawPixel(px, py);
  }
  s->init = true;
}
const CardType CT_starfield = {"starfield", sizeof(Stars), 0, 0, nullptr, starfield_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== game of life (64x32, 2px cells)
struct Life { bool init; uint16_t gen; uint32_t last, hash; uint8_t g[256]; };
static inline bool lget(const uint8_t* g, int x, int y) { x = (x + 64) % 64; y = (y + 32) % 32; return g[y * 8 + x / 8] >> (x % 8) & 1; }
static void life_seed(Life* l) { for (int i = 0; i < 256; i++) l->g[i] = esp_random() & esp_random(); l->gen = 0; }
static void life_draw(CardCtx& c) {
  Life* l = (Life*)c.d;
  if (!l->init) { l->init = true; life_seed(l); }
  if (millis() - l->last > 120) {
    l->last = millis();
    uint8_t n[256];
    memset(n, 0, sizeof(n));
    uint32_t h = 0;
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 64; x++) {
        int k = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if ((dx || dy) && lget(l->g, x + dx, y + dy)) k++;
        bool alive = lget(l->g, x, y);
        if (k == 3 || (alive && k == 2)) { n[y * 8 + x / 8] |= 1 << (x % 8); h = h * 31 + y * 64 + x; }
      }
    memcpy(l->g, n, 256);
    if (++l->gen > 400 || h == l->hash) life_seed(l);
    l->hash = h;
  }
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++)
      if (lget(l->g, x, y)) u8g2.drawBox(x * 2, y * 2, 2, 2);
}
static bool life_input(CardCtx& c, uint8_t g) { if (g == G_LONG) { life_seed((Life*)c.d); return true; } return false; }
const CardType CT_life = {"life", sizeof(Life), 0, 0, nullptr, life_draw, life_input, nullptr, nullptr, nullptr};

// ================================================================== fireworks
struct Part { float x, y, vx, vy; uint8_t life; };
struct Fw { Part p[48]; float rx, ry, rvy; bool rocket; uint32_t next; };
static void fireworks_draw(CardCtx& c) {
  Fw* f = (Fw*)c.d;
  uint32_t now = millis();
  if (!f->rocket && now > f->next) { f->rocket = true; f->rx = 20 + esp_random() % 88; f->ry = 64; f->rvy = -2.6f - (esp_random() % 10) / 10.0f; }
  if (f->rocket) {
    f->ry += f->rvy;
    f->rvy += 0.05f;
    u8g2.drawPixel((int)f->rx, (int)f->ry);
    u8g2.drawPixel((int)f->rx, (int)f->ry + 2);
    if (f->rvy > -0.6f) {
      f->rocket = false;
      f->next = now + 400 + esp_random() % 1200;
      int made = 0;
      for (int i = 0; i < 48 && made < 20; i++) {
        if (f->p[i].life) continue;
        float a = made * 2 * PI / 20, s = 1.0f + (esp_random() % 10) / 10.0f;
        f->p[i] = {f->rx, f->ry, cosf(a) * s, sinf(a) * s, (uint8_t)(25 + esp_random() % 15)};
        made++;
      }
    }
  }
  for (int i = 0; i < 48; i++) {
    Part& p = f->p[i];
    if (!p.life) continue;
    p.x += p.vx; p.y += p.vy; p.vy += 0.04f; p.vx *= 0.98f; p.life--;
    if (p.life > 6 || p.life % 2) u8g2.drawPixel((int)p.x, (int)p.y);
  }
}
const CardType CT_fireworks = {"fireworks", sizeof(Fw), 0, 0, nullptr, fireworks_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== bouncing Emo
struct Bounce { bool init; float x, y, vx, vy; };
static void bounce_draw(CardCtx& c) {
  Bounce* b = (Bounce*)c.d;
  if (!b->init) { b->init = true; b->x = 40; b->y = 20; b->vx = 0.9f; b->vy = 0.6f; }
  b->x += b->vx; b->y += b->vy;
  bool hx = false, hy = false;
  if (b->x < 18 || b->x > 110) { b->vx = -b->vx; hx = true; }
  if (b->y < 10 || b->y > 54) { b->vy = -b->vy; hy = true; }
  if (hx && hy) emoSet(E_EXCITED, 1500);
  else if (hx || hy) emoSet(E_SURPRISED, 300);
  emoRender((int)b->x, (int)b->y, 0.4f);
}
const CardType CT_bounce = {"bounce", sizeof(Bounce), 0, 0, nullptr, bounce_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== network info
static void netinfo_draw(CardCtx& c) {
  drawTitle(c, "Network", netOnline() ? "online" : "offline");
  char b[48];
  if (netOnline()) {
    u8g2.setFont(F_SMALL);
    String ssid = WiFi.SSID();
    if (ssid.length() > 14) ssid = ssid.substring(0, 14);
    snprintf(b, sizeof(b), "%s", ssid.c_str());
    u8g2.drawUTF8(4, 21, b);
    drawSignalBars(110, 21, netRSSI());

    int ipW = u8g2.getUTF8Width(netIP().c_str()) + 14;
    int ipX = (128 - ipW) / 2;
    u8g2.drawRFrame(ipX, 25, ipW, 14, 3);
    u8g2.setFont(F_MED);
    textCenter(36, netIP().c_str());

    String host = String("http://") + (cfg["host"] | DEFAULT_HOST) + ".local";
    u8g2.setFont(F_TINY);
    textCenter(49, host.c_str());

    uint32_t up = millis() / 1000;
    snprintf(b, sizeof(b), "up %lud %02luh %02lum  |  %ddBm",
             (unsigned long)(up / 86400),
             (unsigned long)(up / 3600 % 24),
             (unsigned long)(up / 60 % 60),
             netRSSI());
    textCenter(60, b);
  } else {
    drawCenteredMsg(netPortal() ? "Hotspot Active" : "No Wi-Fi",
                    netPortal() ? "Join: EmoDesk-Setup" : "Check credentials");
  }
}
static void netinfo_speak(CardCtx& c, String& s) {
  s = "My address is " + netIP() + ". Or open " + String(cfg["host"] | DEFAULT_HOST) + " dot local.";
  s.replace(".", " dot ");
}
const CardType CT_netinfo = {"netinfo", 0, 0, 0, nullptr, netinfo_draw, nullptr, nullptr, netinfo_speak, nullptr};

// ================================================================== device health
static void health_draw(CardCtx& c) {
  drawTitle(c, "Device Health", "v" FW_VERSION);
  char b[40];
  uint32_t freeH = ESP.getFreeHeap();
  uint32_t totH = ESP.getHeapSize();
  float usedH = totH ? 1.0f - (float)freeH / totH : 0.0f;
  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 21, "RAM");
  drawBar(24, 15, 60, 7, usedH);
  snprintf(b, sizeof(b), "%uK free", (unsigned)(freeH / 1024));
  textRight(126, 21, b);

  size_t totFS = LittleFS.totalBytes(), usedFS = LittleFS.usedBytes();
  float usedFSPct = totFS ? (float)usedFS / totFS : 0.0f;
  u8g2.drawStr(2, 32, "DISK");
  drawBar(24, 26, 60, 7, usedFSPct);
  snprintf(b, sizeof(b), "%u/%uK", (unsigned)(usedFS / 1024), (unsigned)(totFS / 1024));
  textRight(126, 32, b);

  drawDottedHLine(2, 37, 124);

  snprintf(b, sizeof(b), "CPU %uMHz  |  Chip %.0f C", (unsigned)ESP.getCpuFreqMHz(), temperatureRead());
  textCenter(47, b);

  if (cric.limit > 0) {
    snprintf(b, sizeof(b), "Cricket API: %d/%d hits", cric.hits, cric.limit);
  } else {
    snprintf(b, sizeof(b), "Max block %uK  |  Min %uK", (unsigned)(heapLargest() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024));
  }
  textCenter(59, b);
}
const CardType CT_health = {"health", 0, 0, 0, nullptr, health_draw, nullptr, nullptr, nullptr, nullptr};
