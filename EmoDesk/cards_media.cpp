#include "cards.h"

static bool needSpeaker() {
  if (audioInstalled()) return true;
  drawCenteredMsg("No speaker yet", "Turn it on in Sound settings");
  return false;
}
static void note(int x, int y) {
  u8g2.drawDisc(x, y, 2);
  u8g2.drawVLine(x + 2, y - 8, 8);
  u8g2.drawLine(x + 2, y - 8, x + 5, y - 6);
}

// ================================================================== music box
static void musicbox_draw(CardCtx& c) {
  if (!needSpeaker()) return;
  int n = melodyCount();
  int sel = c.st[0] % max(1, n);
  char b[12];
  snprintf(b, sizeof(b), "%d/%d", sel + 1, n);
  drawTitle(c, "Music Box", b);
  bool playing = audioBusy() && c.st[1];
  if (playing) {
    emoSet(E_HAPPY, 500);
    int bob = (int)(sinf(millis() / 120.0f) * 3);
    emoRender(64, 27 + bob, 0.46f);
    note(16, 30 - bob);
    note(109, 28 + bob);
    cardHold = true;
  } else {
    c.st[1] = 0;
    u8g2.drawCircle(64, 24, 10);
    u8g2.drawTriangle(61, 19, 61, 29, 70, 24);
    u8g2.setFont(F_TINY);
    textCenter(41, "2x: play/stop   hold: next");
  }
  drawDottedHLine(0, 45, 128);
  drawIconMusic(3, 51);
  u8g2.setFont(F_MED);
  const char* mn = melodyName(sel);
  if (u8g2.getUTF8Width(mn) <= 112) u8g2.drawUTF8(15, 59, mn);
  else marquee(15, 59, 111, mn, c.t0, 28);
}
static bool musicbox_input(CardCtx& c, uint8_t g) {
  int n = max(1, melodyCount());
  if (g == G_DOUBLE) {
    if (audioBusy()) { audioStop(); c.st[1] = 0; }
    else { audioMelody(c.st[0] % n, EV_UI); c.st[1] = 1; }
    return true;
  }
  if (g == G_LONG) {
    c.st[0] = (c.st[0] + 1) % n;
    if (c.st[1]) { audioStop(); audioMelody(c.st[0], EV_UI); }
    return true;
  }
  return false;
}
const CardType CT_musicbox = {"musicbox", 0, 0, 0, nullptr, musicbox_draw, musicbox_input, nullptr, nullptr, nullptr};

// ================================================================== soundboard
struct SbData { uint8_t n; char spec[28][36]; };
static void soundboard_enter(CardCtx& c) {
  SbData* d = (SbData*)c.d;
  if (!d) return;
  d->n = 0;
  File dir = LittleFS.open("/snd");
  if (dir) {
    for (File f = dir.openNextFile(); f && d->n < 12; f = dir.openNextFile()) {
      snprintf(d->spec[d->n++], 36, "file:/snd/%s", f.name());
      f.close();
    }
  }
  for (int i = 0; i < sfxCount() && d->n < 28; i++) snprintf(d->spec[d->n++], 36, "sfx:%s", sfxName(i));
}
static const char* specLabel(const char* s) {
  const char* p = strrchr(s, '/');
  if (p) return p + 1;
  p = strchr(s, ':');
  return p ? p + 1 : s;
}
static void soundboard_draw(CardCtx& c) {
  if (!needSpeaker()) return;
  SbData* d = (SbData*)c.d;
  if (!d) return;
  if (!d->n) soundboard_enter(c);
  drawTitle(c, "Soundboard", "2x play");
  if (!d->n) { drawCenteredMsg("No sounds available"); return; }
  int cur = c.st[0] % d->n;
  int first = max(0, min(cur - 2, (int)d->n - 5));
  int rowW = (d->n > 5) ? 124 : 128;
  for (int k = 0; k < 5 && first + k < d->n; k++) {
    int y = 20 + k * 10;
    bool sel = first + k == cur;
    bool isFile = !strncmp(d->spec[first + k], "file:", 5);
    if (sel) { u8g2.drawBox(0, y - 8, rowW, 10); u8g2.setDrawColor(0); }
    u8g2.setFont(F_SMALL);
    drawTextFit(4, y, rowW - (isFile ? 18 : 8), specLabel(d->spec[first + k]));
    if (isFile) u8g2.drawStr(rowW - 9, y, "*");
    u8g2.setDrawColor(1);
  }
  if (d->n > 5) {
    int h = max(6, 50 * 5 / d->n);
    int sy = 12 + (50 - h) * first / (d->n - 5);
    u8g2.drawBox(126, sy, 2, h);
  }
}
static bool soundboard_input(CardCtx& c, uint8_t g) {
  SbData* d = (SbData*)c.d;
  if (!d || !d->n) return false;
  if (g == G_DOUBLE) { audioStop(); audioPlaySpec(d->spec[c.st[0] % d->n], EV_UI); return true; }
  if (g == G_LONG) { c.st[0] = (c.st[0] + 1) % d->n; return true; }
  return false;
}
const CardType CT_soundboard = {"soundboard", sizeof(SbData), 0, 0, soundboard_enter, soundboard_draw, soundboard_input, nullptr, nullptr, nullptr};

// ================================================================== sleep sounds
static const char* const NOISES[] = {"white", "pink", "brown", "rain", "ocean"};
static int noiseIdx(const char* n) {
  for (int i = 0; i < 5; i++) if (!strcmp(n, NOISES[i])) return i;
  return 3;
}
// st: 0 playing 1 end millis 2 type+1 3 start millis
static void sleepsounds_draw(CardCtx& c) {
  if (!needSpeaker()) return;
  if (!c.st[2]) c.st[2] = noiseIdx(c.p["type"] | "rain") + 1;
  int type = c.st[2] - 1;
  if (c.st[0] && millis() - c.st[3] > 3000 && !audioContinuous()) c.st[0] = 0;
  drawTitle(c, "Sleep Sounds", c.st[0] ? "playing" : "ready");
  char name[16];
  snprintf(name, sizeof(name), "%s%s", NOISES[type], type < 3 ? " noise" : "");
  name[0] = toupper(name[0]);
  u8g2.setFont(F_HELV14);
  textCenter(29, name);
  float t = millis() / 400.0f;
  for (int x = 2; x < 126; x += 2) {
    int amp = c.st[0] ? 4 : 1;
    int y = 42 + (int)(sinf(x / 9.0f + t) * amp + sinf(x / 4.0f - t * 1.7f) * amp / 2);
    u8g2.drawPixel(x, y);
  }
  drawDottedHLine(0, 52, 128);
  u8g2.setFont(F_TINY);
  char b[36];
  if (c.st[0] && c.st[1]) {
    int32_t r = (int32_t)(c.st[1] - millis());
    snprintf(b, sizeof(b), "%ldm left  |  hold: next", (long)max(0L, (long)r / 60000 + 1));
  } else {
    snprintf(b, sizeof(b), "%s (%dm)  |  2x: %s",
             c.st[0] ? "Endless" : "Timer", (int)(c.p["min"] | 30), c.st[0] ? "stop" : "play");
  }
  drawTextFitCenter(64, 62, 124, b);
}
static bool sleepsounds_input(CardCtx& c, uint8_t g) {
  int mins = c.p["min"] | 30;
  if (!c.st[2]) c.st[2] = noiseIdx(c.p["type"] | "rain") + 1;
  if (g == G_DOUBLE || (g == G_LONG && c.st[0])) {
    if (g == G_LONG) c.st[2] = c.st[2] % 5 + 1;
    if (c.st[0] && g == G_DOUBLE) { audioStop(); c.st[0] = 0; return true; }
    audioStop();
    audioNoise(c.st[2] - 1, mins > 0 ? mins * 60000UL : 0);
    c.st[0] = 1;
    c.st[1] = mins > 0 ? millis() + mins * 60000UL : 0;
    c.st[3] = millis();
    return true;
  }
  if (g == G_LONG) { c.st[2] = c.st[2] % 5 + 1; return true; }
  return false;
}
const CardType CT_sleepsounds = {"sleepsounds", 0, 0, 0, nullptr, sleepsounds_draw, sleepsounds_input, nullptr, nullptr, nullptr};

// ================================================================== metronome
static const int BPMS[] = {60, 72, 80, 90, 100, 110, 120, 132, 140, 160, 180};
static void metronome_draw(CardCtx& c) {
  if (!needSpeaker()) return;
  if (!c.st[1]) c.st[1] = constrain((int)(c.p["bpm"] | 100), 30, 250);
  if (c.st[0] && !audioContinuous() && millis() - c.st[3] > 2000) c.st[0] = 0;
  drawTitle(c, "Metronome", c.st[0] ? "on" : "ready");
  char b[8];
  snprintf(b, sizeof(b), "%lu", (unsigned long)c.st[1]);
  u8g2.setFont(F_NUM32);
  drawTextFitCenter(34, 46, 66, b);
  u8g2.setFont(F_SMALL);
  drawTextFitCenter(34, 54, 66, "BPM");
  u8g2.setFont(F_TINY);
  drawTextFitCenter(34, 63, 66, "2x:run  hold:bpm");

  drawDottedVLine(70, 14, 48);

  uint32_t period = max(1UL, 60000UL / c.st[1]);
  float ph = c.st[0] ? (float)((millis() - audioMetroBeatMs()) % period) / period : 0.5f;
  int beats = c.st[0] ? (int)((millis() / period) % 2) : 0;
  float ang = cosf(PI * ph) * 0.45f * (beats ? 1 : -1);
  int px = 99, py = 59;
  int tx = px + (int)(sinf(ang) * 40), ty = py - (int)(cosf(ang) * 40);
  u8g2.drawLine(px, py, tx, ty);
  u8g2.drawDisc(tx, ty, 3);
  u8g2.drawTriangle(px - 11, py + 3, px + 11, py + 3, px, py - 5);
  if (c.st[0]) cardHold = true;
}
static bool metronome_input(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE) {
    if (c.st[0]) { audioStop(); c.st[0] = 0; }
    else { audioMetronome(c.st[1]); c.st[0] = 1; c.st[3] = millis(); }
    return true;
  }
  if (g == G_LONG) {
    int i = 0;
    while (i < 11 && BPMS[i] <= (int)c.st[1]) i++;
    c.st[1] = BPMS[i % 11];
    if (c.st[0]) { audioStop(); audioMetronome(c.st[1]); c.st[3] = millis(); }
    return true;
  }
  return false;
}
const CardType CT_metronome = {"metronome", 0, 0, 0, nullptr, metronome_draw, metronome_input, nullptr, nullptr, nullptr};

// ================================================================== visualizer
static void visualizer_draw(CardCtx& c) {
  if (!needSpeaker()) return;
  bool live = audioBusy() || audioContinuous() || millis() - vizStamp < 300;
  if (!live) {
    drawTitle(c, "Visualizer", "idle");
    drawDottedHLine(2, 34, 124);
    u8g2.setFont(F_SMALL);
    textCenter(52, "Play sound to visualize");
    return;
  }
  if (!strcmp(c.p["mode"] | "bars", "wave")) {
    int py = 32;
    for (int x = 0; x < 128; x++) {
      int y = 32 - vizBuf[x] / 1100;
      y = constrain(y, 0, 63);
      if (x) u8g2.drawLine(x - 1, py, x, y);
      py = y;
    }
    return;
  }
  static float peak[16];
  for (int b = 0; b < 16; b++) {
    int32_t sum = 0;
    for (int k = 0; k < 8; k++) sum += abs(vizBuf[b * 8 + k]);
    float h = min(62.0f, sum / 8 / 300.0f);
    peak[b] = max(peak[b] * 0.85f, h);
    u8g2.drawBox(b * 8 + 1, 63 - (int)h, 6, (int)h);
    u8g2.drawHLine(b * 8 + 1, 62 - (int)peak[b], 6);
  }
}
const CardType CT_visualizer = {"visualizer", 0, 0, 0, nullptr, visualizer_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== image slideshow
// images are 1024-byte XBM bitmaps (128x64) created by the dashboard
struct ImgData { uint8_t n, cur; int16_t loaded; uint32_t changed; char names[12][32]; uint8_t buf[1024]; };
static void images_enter(CardCtx& c) {
  ImgData* d = (ImgData*)c.d;
  if (!d) return;
  d->n = 0;
  d->loaded = -1;
  String want = c.p["files"] | "";
  File dir = LittleFS.open("/img");
  if (!dir) return;
  for (File f = dir.openNextFile(); f && d->n < 12; f = dir.openNextFile()) {
    String nm = f.name();
    f.close();
    if (want.length() && want.indexOf(nm) < 0) continue;
    snprintf(d->names[d->n++], 32, "/img/%s", nm.c_str());
  }
  d->changed = millis();
}
static bool loadBitmap(const char* path, uint8_t* buf, uint32_t offset) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  f.seek(offset);
  bool ok = f.read(buf, 1024) == 1024;
  f.close();
  return ok;
}
static void images_draw(CardCtx& c) {
  ImgData* d = (ImgData*)c.d;
  if (!d) return;
  if (!d->n) { drawCenteredMsg("No images yet", "Draw or upload in Media tab"); return; }
  int sec = max(2, (int)(c.p["sec"] | 8));
  if (millis() - d->changed > sec * 1000UL) { d->cur = (d->cur + 1) % d->n; d->changed = millis(); }
  if (d->loaded != d->cur) { d->loaded = loadBitmap(d->names[d->cur], d->buf, 0) ? d->cur : -2; }
  if (d->loaded < 0) { drawCenteredMsg("Image unreadable", d->names[d->cur]); return; }
  u8g2.drawXBM(0, 0, 128, 64, d->buf);
}
static bool images_input(CardCtx& c, uint8_t g) {
  ImgData* d = (ImgData*)c.d;
  if (g != G_DOUBLE || !d || !d->n) return false;
  d->cur = (d->cur + 1) % d->n;
  d->changed = millis();
  return true;
}
const CardType CT_images = {"images", sizeof(ImgData), 0, 0, images_enter, images_draw, images_input, nullptr, nullptr, nullptr};

// ================================================================== animation ("EMOA" + frames:u16 + delay:u16 + frames*1024)
struct AnimData { bool ok; uint16_t frames, delayMs, cur; int16_t loaded; uint32_t last; char path[40]; uint8_t buf[1024]; };
static void anim_enter(CardCtx& c) {
  AnimData* d = (AnimData*)c.d;
  if (!d) return;
  String f = c.p["file"] | "";
  d->ok = false;
  d->loaded = -1;
  if (!f.length()) return;
  if (!f.startsWith("/")) f = "/anim/" + f;
  safeCopy(d->path, f.c_str(), sizeof(d->path));
  File fh = LittleFS.open(d->path, "r");
  if (!fh) return;
  char magic[4];
  fh.read((uint8_t*)magic, 4);
  fh.read((uint8_t*)&d->frames, 2);
  fh.read((uint8_t*)&d->delayMs, 2);
  fh.close();
  d->ok = !memcmp(magic, "EMOA", 4) && d->frames > 0;
  int fps = c.p["fps"] | 0;
  if (fps > 0) d->delayMs = 1000 / fps;
  if (d->delayMs < 30) d->delayMs = 30;
  d->cur = 0;
}
static void anim_draw(CardCtx& c) {
  AnimData* d = (AnimData*)c.d;
  if (!d) return;
  if (!d->ok) { drawCenteredMsg("No animation", "Upload a GIF in Media tab"); return; }
  if (millis() - d->last >= d->delayMs) { d->last = millis(); d->cur = (d->cur + 1) % d->frames; }
  if (d->loaded != d->cur) d->loaded = loadBitmap(d->path, d->buf, 8 + (uint32_t)d->cur * 1024) ? d->cur : -2;
  if (d->loaded >= 0) u8g2.drawXBM(0, 0, 128, 64, d->buf);
}
const CardType CT_anim = {"anim", sizeof(AnimData), 0, 0, anim_enter, anim_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== text scroller
static void scroller_draw(CardCtx& c) {
  const char* t = c.p["text"] | "Hello from Emo!";
  int size = c.p["size"].is<const char*>() ? atoi(c.p["size"].as<const char*>()) : (int)(c.p["size"] | 2);
  bool inv = c.p["invert"] | false;
  int speed = max(5, (int)(c.p["speed"] | 40));
  if (inv) { u8g2.drawBox(0, 0, 128, 64); u8g2.setDrawColor(0); }
  const uint8_t* f = size <= 1 ? F_MED : (size == 2 ? F_HELV14 : u8g2_font_logisoso32_tr);
  u8g2.setFont(f);
  int base = size <= 1 ? 36 : (size == 2 ? 39 : 48);
  int tw = u8g2.getUTF8Width(t);
  int span = tw + 128;
  int off = (int)(((millis() - c.t0) * speed / 1000) % span);
  u8g2.drawUTF8(128 - off, base, t);
  u8g2.setDrawColor(1);
}
static void scroller_speak(CardCtx& c, String& s) { s = c.p["text"] | ""; }
const CardType CT_scroller = {"scroller", 0, 0, 0, nullptr, scroller_draw, nullptr, nullptr, scroller_speak, nullptr};
