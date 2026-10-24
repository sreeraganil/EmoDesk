#include "emo.h"
#include "display.h"
#include "audio.h"
#include "feeds.h"
#include "cards.h"

const char* const EMO_NAMES[E_COUNT] = {"neutral", "happy", "sad", "angry", "sleepy", "surprised", "love", "bored",
                                         "dizzy", "confused", "excited", "suspicious", "cry", "sleep", "cool", "focused"};
PetStats pet = {70, 80, 20, 0};

static int tempEmo = -1, holdEmo = -1;
static uint32_t tempUntil = 0, holdUntil = 0;
static bool sleeping = false, nightSleep = false;
static uint32_t lastTouch = 0, lastUpd = 0, nextLook = 0, nextBlink = 0, blinkStart = 0, blinkLen = 150;
static uint32_t nextIdle = 0, lastWeatherReact = 0, lastSave = 0, lastAnim = 0, lastPetFrame = 0, lastNeedNote = 0;
static float lookX = 0, lookY = 0, tLookX = 0, tLookY = 0;
static uint32_t pokeT[12];
static uint8_t pokeI = 0;
static bool purred = false;

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ------------------------------------------------------------------ looks (cached from cfg by emoApply)
// Expression shapes are in "design units" for a 36x42 eye and get scaled to the
// user's eye size. Order: width, height, top cut outer, top cut inner,
// bottom cut outer, bottom cut inner. [0] = left eye, [1] = right eye.
static const int8_t BUILTIN[E_COUNT][2][6] = {
  {{36, 42, 0, 0, 0, 0},     {36, 42, 0, 0, 0, 0}},       // neutral
  {{36, 42, 8, 8, 20, 20},   {36, 42, 8, 8, 20, 20}},     // happy
  {{36, 38, 18, 0, 6, 6},    {36, 38, 18, 0, 6, 6}},      // sad
  {{36, 38, 4, 22, 6, 6},    {36, 38, 4, 22, 6, 6}},      // angry
  {{36, 42, 22, 22, 10, 10}, {36, 42, 22, 22, 10, 10}},   // sleepy
  {{34, 46, -4, -4, -2, -2}, {34, 46, -4, -4, -2, -2}},   // surprised
  {{36, 42, 0, 0, 0, 0},     {36, 42, 0, 0, 0, 0}},       // love (hearts unless customised)
  {{36, 42, 22, 22, 10, 10}, {36, 42, 22, 22, 10, 10}},   // bored
  {{36, 42, 0, 0, 0, 0},     {36, 42, 0, 0, 0, 0}},       // dizzy (spirals unless customised)
  {{38, 44, 0, 0, 0, 0},     {36, 42, 18, 18, 12, 12}},   // confused
  {{40, 44, 0, 0, 12, 12},   {40, 44, 0, 0, 12, 12}},     // excited
  {{38, 44, 0, 0, 0, 0},     {36, 42, 18, 18, 12, 12}},   // suspicious
  {{36, 38, 18, 0, 6, 6},    {36, 38, 18, 0, 6, 6}},      // cry
  {{36, 42, 26, 26, 14, 14}, {36, 42, 26, 26, 14, 14}},   // sleep
  {{36, 42, 18, 18, 0, 0},   {36, 42, 18, 18, 0, 0}},     // cool
  {{36, 34, 6, 12, 8, 8},    {36, 34, 6, 12, 8, 8}},      // focused
};

struct CustomExpr { bool on; int8_t l[6], r[6]; char anim[36]; };
static CustomExpr cust[E_COUNT];
static struct { float w, h, r, gap; uint8_t style; bool needs; } look = {36, 42, 12, 14, ES_ROUND, true};

static void readShape(JsonVariantConst a, int8_t* out, const int8_t* def) {
  if (a.is<JsonArrayConst>() && a.size() >= 6) {
    for (int k = 0; k < 6; k++) out[k] = (int8_t)constrain((int)(a[k] | (int)def[k]), k < 2 ? 4 : -10, 60);
  } else memcpy(out, def, 6);
}

void emoApply() {
  LOCK();
  JsonObjectConst E = cfg["emo"];
  look.w = constrain((float)(E["w"] | 36), 10.0f, 60.0f);
  look.h = constrain((float)(E["h"] | 42), 6.0f, 60.0f);
  look.r = constrain((float)(E["r"] | 12), 0.0f, 30.0f);
  look.gap = constrain((float)(E["gap"] | 14), 0.0f, 60.0f);
  look.needs = E["needs"] | true;
  const char* st = E["style"] | "round";
  look.style = !strcmp(st, "pixel") ? ES_PIXEL : !strcmp(st, "scanner") ? ES_SCANNER : !strcmp(st, "cat") ? ES_CAT : ES_ROUND;
  for (int i = 0; i < E_COUNT; i++) {
    JsonObjectConst x = E["x"][EMO_NAMES[i]];
    CustomExpr& c = cust[i];
    c.on = x["on"] | false;
    readShape(x["l"], c.l, BUILTIN[i][0]);
    readShape(x["r"], c.r, x["r"].is<JsonArrayConst>() ? BUILTIN[i][1] : c.l);   // no right eye = same as left
    if (!x["l"].is<JsonArrayConst>()) memcpy(c.r, BUILTIN[i][1], 6);
    safeCopy(c.anim, x["anim"] | "", sizeof(c.anim));
  }
  UNLOCK();
}

void emoBegin() {
  JsonObject p = stateDoc["pet"];
  if (!p.isNull()) {
    pet.happy = p["h"] | 70.0f;
    pet.energy = p["e"] | 80.0f;
    pet.hunger = p["f"] | 20.0f;
    pet.interactions = p["i"] | 0;
  }
  lastTouch = millis();
  emoApply();
}

void emoSaveState() {
  LOCK();
  JsonObject p = stateDoc["pet"].to<JsonObject>();
  p["h"] = (int)pet.happy;
  p["e"] = (int)pet.energy;
  p["f"] = (int)pet.hunger;
  p["i"] = pet.interactions;
  UNLOCK();
  markDirty(DIRTY_STATE);
}

void emoResetStats() {
  pet = {70, 80, 20, 0};
  emoSaveState();
}

int emoFromName(const char* n) {
  for (int i = 0; i < E_COUNT; i++)
    if (n && strcasecmp(n, EMO_NAMES[i]) == 0) return i;
  return -1;
}

bool emoActive() {
  if (cardsCount() == 0) return true; // during boot before cards are loaded
  return cardsHaveType("emo");
}

void emoSet(int e, uint32_t ms) {
  if (!emoActive()) return;
  if (e < 0 || e >= E_COUNT) return;
  tempEmo = e;
  tempUntil = millis() + ms;
}

void emoHold(int e, uint32_t ms) {
  if (!emoActive()) return;
  if (e < 0 || e >= E_COUNT) return;
  holdEmo = e;
  holdUntil = millis() + ms;
}
static bool holdActive(uint32_t now) { return holdEmo >= 0 && (int32_t)(holdUntil - now) > 0; }
static bool tempActive(uint32_t now) { return tempEmo >= 0 && (int32_t)(tempUntil - now) > 0; }

bool emoSleeping() { return emoActive() && sleeping; }

void emoSleepNow() {
  if (!emoActive()) return;
  sleeping = true;
  nightSleep = false;
  lastTouch = millis() - 3600000UL;
}

int emoCurrent() {
  if (!emoActive()) return E_NEUTRAL;
  uint32_t now = millis();
  if (tempActive(now)) return tempEmo;
  if (sleeping) return E_SLEEP;
  if (holdActive(now)) return holdEmo;
  uint32_t idle = now - lastTouch;
  int bored = cfg["emo"]["boredMin"] | 5, sleepy = cfg["emo"]["sleepyMin"] | 15;
  if (sleepy > 0 && idle > sleepy * 60000UL) return E_SLEEPY;
  if (bored > 0 && idle > bored * 60000UL) return E_BORED;
  if (pet.hunger > 85 || pet.happy < 20) return E_SAD;
  if (pet.energy < 15) return E_SLEEPY;
  if (pet.happy > 85) return E_HAPPY;
  return E_NEUTRAL;
}

void emoTouched() {
  uint32_t now = millis();
  lastTouch = now;
  if (!emoActive()) return;
  if (sleeping) {
    sleeping = false;
    emoSet(nightSleep ? E_ANGRY : E_SURPRISED, 1500);
    audioChirp(nightSleep ? E_ANGRY : E_SURPRISED, EV_TOUCH);
  }
}

void emoPoke(int n) {
  if (!emoActive()) return;
  emoTouched();
  uint32_t now = millis();
  for (int i = 0; i < n; i++) { pokeT[pokeI] = now; pokeI = (pokeI + 1) % 12; }
  int cnt = 0;
  for (int i = 0; i < 12; i++) if (pokeT[i] && now - pokeT[i] < 4000) cnt++;
  pet.interactions += n;
  tLookX = 0; tLookY = 0;
  if (cnt >= 10) {
    emoSet(E_DIZZY, 3000);
    audioSfx("dizzy", EV_TOUCH);
    pet.happy -= 3;
  } else if (cnt >= 6) {
    emoSet(E_ANGRY, 2500);
    audioChirp(E_ANGRY, EV_TOUCH);
    pet.happy -= 2;
  } else {
    int e = (esp_random() % 3 == 0) ? E_SURPRISED : E_HAPPY;
    emoSet(e, 1300);
    audioChirp(e, EV_TOUCH);
    pet.happy += 3;
  }
  pet.happy = clampf(pet.happy, 0, 100);
}

void emoPetting() {
  if (!emoActive()) return;
  uint32_t now = millis();
  lastTouch = now;
  if (sleeping) emoTouched();
  emoSet(E_LOVE, 1200);
  pet.happy = clampf(pet.happy + 0.05f, 0, 100);
  if (!purred) { audioSfx("purr", EV_TOUCH); purred = true; pet.interactions++; }
  lastPetFrame = now;
}

void emoFeed() {
  if (!emoActive()) return;
  emoTouched();
  pet.hunger = clampf(pet.hunger - 35, 0, 100);
  pet.happy = clampf(pet.happy + 5, 0, 100);
  pet.interactions++;
  emoSet(E_HAPPY, 3000);
  audioSfx("yum", EV_TOUCH);
  emoSaveState();
}

void emoPlay() {
  if (!emoActive()) return;
  emoTouched();
  pet.happy = clampf(pet.happy + 8, 0, 100);
  pet.energy = clampf(pet.energy - 3, 0, 100);
  pet.interactions++;
  emoSet(E_EXCITED, 3000);
  audioChirp(E_EXCITED, EV_TOUCH);
}

String emoSpeech() {
  if (!emoActive()) return "Emo face card is disabled.";
  String name = cfg["name"] | "Emo";
  String s = "I am " + name + ". I feel " + String(EMO_NAMES[emoCurrent()]) + ".";
  if (pet.hunger > 70) s += " I am hungry. Please feed me.";
  if (pet.energy < 25) s += " I am tired.";
  s += " Happiness " + String((int)pet.happy) + " percent.";
  return s;
}

void emoUpdate() {
  if (!emoActive()) return;
  uint32_t now = millis();
  if (!lastUpd) { lastUpd = now; return; }
  float dt = (now - lastUpd) / 1000.0f;
  lastUpd = now;
  pet.happy = clampf(pet.happy - dt / 180.0f, 0, 100);
  pet.hunger = clampf(pet.hunger + dt / 400.0f, 0, 100);
  pet.energy = clampf(pet.energy + (sleeping ? dt / 30.0f : -dt / 700.0f), 0, 100);
  if (purred && now - lastPetFrame > 2000) purred = false;

  bool sleepEn = cfg["emo"]["sleepEn"] | true;
  int from = parseHM(cfg["emo"]["sleepFrom"] | "23:00"), to = parseHM(cfg["emo"]["sleepTo"] | "07:00");
  bool night = sleepEn && inWindow(nowMin(), from, to);
  uint32_t idle = now - lastTouch;
  int napMin = cfg["emo"]["napMin"] | 30;
  bool held = holdActive(now);   // e.g. a focus session is running: stay awake
  if (!sleeping) {
    if (!held && night && idle > 60000) { sleeping = true; nightSleep = true; }
    else if (!held && napMin > 0 && idle > napMin * 60000UL) { sleeping = true; nightSleep = false; }
  } else if (nightSleep && !night && sleepEn) {
    sleeping = false;
    lastTouch = now;
    emoSet(E_HAPPY, 4000);
    audioChirp(E_HAPPY, EV_EMO);
  }

  // Pet needs: ask for food / company at most every 2 hours, never at night or asleep.
  if (look.needs && !sleeping && !night && now > 60000 && (!lastNeedNote || now - lastNeedNote > 7200000UL)) {
    String name = cfg["name"] | "Emo";
    if (pet.hunger > 85) {
      lastNeedNote = now;
      notify((name + " is hungry").c_str(), "Hold the Pet card or tap Feed in the dashboard.", E_SAD, "sfx:notify", EV_EMO, false, 8);
    } else if (pet.happy < 15) {
      lastNeedNote = now;
      notify((name + " is lonely").c_str(), "Come and play with me!", E_SAD, "sfx:notify", EV_EMO, false, 8);
    }
  }

  int activity = cfg["emo"]["activity"] | 60;
  if ((int32_t)(now - nextIdle) > 0) {
    nextIdle = now + 3000 + esp_random() % (12000 - activity * 80);
    if (!sleeping && !held && !tempActive(now) && (int)(esp_random() % 100) < activity / 2) {
      static const int8_t micro[] = {E_HAPPY, E_SUSPICIOUS, E_CONFUSED, E_SURPRISED, E_EXCITED, E_HAPPY, E_LOVE};
      int e = micro[esp_random() % sizeof(micro)];
      if (pet.happy < 35 && esp_random() % 2) e = E_SAD;
      emoSet(e, 1500 + esp_random() % 1500);
      if ((cfg["emo"]["chirps"] | true) && (int)(esp_random() % 100) < activity / 2) audioChirp(e, EV_EMO);
    }
  }

  if ((cfg["emo"]["weather"] | true) && homeW.ok && !sleeping && !held && now - lastWeatherReact > 900000UL) {
    lastWeatherReact = now;
    int k = wmoClass(homeW.code);
    if (k == 0 && homeW.isDay && homeW.temp >= (cfg["units"] == "imperial" ? 82 : 28)) emoSet(E_COOL, 6000);
    else if (k == 4 || k == 6) emoSet(E_SAD, 3000);
    else if (k == 5) emoSet(E_SURPRISED, 3000);
  }

  if (now - lastSave > 600000UL) { lastSave = now; emoSaveState(); }
}

// ------------------------------------------------------------------ drawing

struct EyeParams { float w, h, tL, tR, bL, bR; };
static EyeParams curL = {36, 42, 0, 0, 0, 0};
static EyeParams curR = {36, 42, 0, 0, 0, 0};
static float pupil = 0.16f;   // cat-eye pupil width (fraction of eye width)

static void getEyeTarget(int e, bool isLeft, EyeParams& p) {
  const int8_t* a = cust[e].on ? (isLeft ? cust[e].l : cust[e].r) : BUILTIN[e][isLeft ? 0 : 1];
  float kw = look.w / 36.0f, kh = look.h / 42.0f;
  p.w = a[0] * kw;
  p.h = a[1] * kh;
  float to = a[2] * kh, ti = a[3] * kh, bo = a[4] * kh, bi = a[5] * kh;
  if (isLeft) { p.tL = to; p.tR = ti; p.bL = bo; p.bR = bi; }
  else        { p.tL = ti; p.tR = to; p.bL = bi; p.bR = bo; }
}

// Pixel geometry of one eye, blink already applied.
struct EyeGeo { int cx, cy, x0, y0, w, h, r, tL, tR, bL, bR; };
static EyeGeo eyeGeo(int cx, int cy, float sc, const EyeParams& p, float open) {
  EyeGeo g;
  g.cx = cx; g.cy = cy;
  g.w = max(3, (int)(p.w * sc));
  g.h = (int)(p.h * sc);
  g.x0 = cx - g.w / 2;
  g.y0 = cy - g.h / 2;
  g.r = min((int)(look.r * sc), min(g.w, g.h) / 2 - 1);
  if (g.r < 0) g.r = 0;
  float blinkT = (p.h - p.tL - p.bL) / 2.0f * (1.0f - open);
  float blinkR = (p.h - p.tR - p.bR) / 2.0f * (1.0f - open);
  g.tL = (int)((p.tL + blinkT) * sc); g.tR = (int)((p.tR + blinkR) * sc);
  g.bL = (int)((p.bL + blinkT) * sc); g.bR = (int)((p.bR + blinkR) * sc);
  return g;
}

static bool eyeInside(const EyeGeo& g, float x, float y) {
  if (x < g.x0 || x >= g.x0 + g.w || y < g.y0 || y >= g.y0 + g.h) return false;
  float r = g.r;
  float qx = x < g.x0 + r ? g.x0 + r : (x > g.x0 + g.w - r ? g.x0 + g.w - r : x);
  float qy = y < g.y0 + r ? g.y0 + r : (y > g.y0 + g.h - r ? g.y0 + g.h - r : y);
  float dx = x - qx, dy = y - qy;
  if (dx * dx + dy * dy > r * r) return false;
  float f = (x - g.x0) / (float)g.w;
  if (y < g.y0 + g.tL + (g.tR - g.tL) * f) return false;
  if (y > g.y0 + g.h - g.bL - (g.bR - g.bL) * f) return false;
  return true;
}

static void cutLids(const EyeGeo& g) {
  u8g2.setDrawColor(0);
  int L = g.x0 - 2, R = g.x0 + g.w + 2;
  if (g.tL > 0 || g.tR > 0) {
    int top = g.y0 - 2, tyL = g.y0 + g.tL, tyR = g.y0 + g.tR;
    u8g2.drawTriangle(L, top, R, top, R, tyR);
    u8g2.drawTriangle(L, top, R, tyR, L, tyL);
  }
  if (g.bL > 0 || g.bR > 0) {
    int bot = g.y0 + g.h + 2, byL = g.y0 + g.h - g.bL, byR = g.y0 + g.h - g.bR;
    u8g2.drawTriangle(L, bot, R, bot, R, byR);
    u8g2.drawTriangle(L, bot, R, byR, L, byL);
  }
  u8g2.setDrawColor(1);
}

static void drawEye(int cx, int cy, float sc, const EyeParams& p, float open) {
  EyeGeo g = eyeGeo(cx, cy, sc, p, open);
  if (look.style == ES_PIXEL) {
    int b = max(2, (int)(4 * sc + 0.5f));
    if (g.h < 4) { for (int x = g.x0; x < g.x0 + g.w; x += b) u8g2.drawBox(x, cy - 1, b - 1, 2); return; }
    for (int y = g.y0; y < g.y0 + g.h; y += b)
      for (int x = g.x0; x < g.x0 + g.w; x += b)
        if (eyeInside(g, x + b / 2.0f, y + b / 2.0f)) u8g2.drawBox(x, y, b - 1, b - 1);
    return;
  }
  if (g.h < 4) { u8g2.drawBox(g.x0, cy - 1, g.w, 3); return; }
  u8g2.setDrawColor(1);
  u8g2.drawRBox(g.x0, g.y0, g.w, g.h, g.r);
  if (look.style == ES_CAT) {
    int pw = max(1, (int)(g.w * pupil / 2));
    int ph = max(2, (int)(g.h * 0.42f));
    int px = cx + (int)(lookX * g.w * 0.15f);
    u8g2.setDrawColor(0);
    u8g2.drawFilledEllipse(px, cy, pw, ph);
    u8g2.setDrawColor(1);
    if (pw >= 3) u8g2.drawDisc(px - pw / 3, cy - ph / 2, 1);
  }
  cutLids(g);
}

// KITT / Cylon style visor: one sweeping light whose speed follows the mood.
static void drawScanner(int cx, int cy, float sc, int e, float open, uint32_t now) {
  int W = constrain((int)(look.gap * sc + (curL.w + curR.w) * sc), 20, 120);
  float openU = curL.h - (curL.tL + curL.tR) / 2 - (curL.bL + curL.bR) / 2;
  int bh = max(2, (int)(openU * sc * 0.35f * open));
  int x0 = cx - W / 2, y0 = cy - bh / 2;
  u8g2.drawRFrame(x0 - 2, y0 - 2, W + 4, bh + 4, min(3, (bh + 4) / 2));
  if (e == E_SLEEP) { u8g2.drawHLine(x0, cy, W); return; }
  uint32_t period = 1800;
  switch (e) {
    case E_EXCITED: case E_ANGRY: period = 600; break;
    case E_HAPPY: case E_SURPRISED: period = 1100; break;
    case E_FOCUSED: period = 2600; break;
    case E_SLEEPY: case E_BORED: case E_SAD: case E_CRY: period = 4000; break;
  }
  float ph = (now % period) / (float)period * 6.2832f;
  float pos = (sinf(ph) + 1) * 0.5f;
  if (e == E_DIZZY) pos = (sinf(now / 90.0f) * sinf(now / 233.0f) + 1) * 0.5f;
  int seg = max(4, W / 5);
  if (e == E_LOVE) seg = (int)(seg * (1.0f + 0.6f * sinf(now / 150.0f)));
  if (e == E_SURPRISED) seg = W;
  seg = constrain(seg, 2, W);
  int sx = x0 + (int)(pos * (W - seg));
  u8g2.drawBox(sx, y0, seg, bh);
  if (seg >= W) return;
  int dir = cosf(ph) > 0 ? -1 : 1;   // trail fades out behind the light
  int tw = max(2, seg / 2);
  for (int k = 1; k <= 3; k++) {
    int tx = dir < 0 ? sx - k * tw : sx + seg + (k - 1) * tw;
    for (int x = tx; x < tx + tw; x += k + 1)
      if (x >= x0 && x < x0 + W) u8g2.drawVLine(x, y0 + 1, max(1, bh - 2));
  }
}

static void heart(int cx, int cy, int s) {
  int r = max(2, s / 4);
  u8g2.drawDisc(cx - r, cy - r / 2, r);
  u8g2.drawDisc(cx + r, cy - r / 2, r);
  u8g2.drawTriangle(cx - 2 * r, cy - r / 4, cx + 2 * r + 1, cy - r / 4, cx, cy + s / 2);
}

static void spiral(int cx, int cy, int w, uint32_t now) {
  for (int i = 0; i < 34; i++) {
    float a = i * 0.45f + now / 120.0f;
    float r = i * (w / 2.0f) / 34.0f;
    u8g2.drawDisc(cx + cosf(a) * r, cy + sinf(a) * r, 1);
  }
}

// Custom full-screen animation for a feeling ("EMOA" files from the Media tab).
static struct { char path[44]; uint16_t frames, delayMs, cur; int16_t loaded; uint32_t last; bool ok; uint8_t buf[1024]; } ea;
bool drawAnimFrame(const char* name) {
  char p[44];
  if (name[0] == '/') safeCopy(p, name, sizeof(p));
  else snprintf(p, sizeof(p), "/anim/%s", name);
  uint32_t now = millis();
  if (strcmp(p, ea.path) != 0) {
    safeCopy(ea.path, p, sizeof(ea.path));
    ea.ok = false; ea.cur = 0; ea.loaded = -1; ea.last = now;
    File f = LittleFS.open(ea.path, "r");
    if (f) {
      char magic[4];
      ea.ok = f.read((uint8_t*)magic, 4) == 4 && !memcmp(magic, "EMOA", 4) &&
              f.read((uint8_t*)&ea.frames, 2) == 2 && f.read((uint8_t*)&ea.delayMs, 2) == 2 && ea.frames > 0;
      f.close();
      if (ea.delayMs < 30) ea.delayMs = 30;
    }
  }
  if (!ea.ok) return false;
  if (now - ea.last >= ea.delayMs) { ea.last = now; ea.cur = (ea.cur + 1) % ea.frames; }
  if (ea.loaded != ea.cur) {
    File f = LittleFS.open(ea.path, "r");
    bool ok = f && f.seek(8 + (uint32_t)ea.cur * 1024) && f.read(ea.buf, 1024) == 1024;
    if (f) f.close();
    if (!ok) { ea.ok = false; return false; }
    ea.loaded = ea.cur;
  }
  u8g2.drawXBM(0, 0, 128, 64, ea.buf);
  return true;
}

// Small thought bubble when the pet needs something.
static void needBubble(uint32_t now) {
  bool hungry = pet.hunger > 70, lonely = pet.happy < 25;
  if (!(hungry || lonely) || (now / 1000) % 12 >= 4) return;
  u8g2.drawDisc(2, 15, 1);
  u8g2.drawCircle(5, 12, 1);
  u8g2.setDrawColor(0);
  u8g2.drawRBox(6, 0, 15, 11, 3);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(6, 0, 15, 11, 3);
  if (hungry) { u8g2.drawDisc(13, 6, 3); u8g2.drawLine(13, 3, 15, 1); }
  else heart(13, 5, 7);
}

void emoRender(int cx, int cy, float sc) {
  uint32_t now = millis();
  int e = emoCurrent();

  if (now - lastAnim > 15) {
    lastAnim = now;
    if ((int32_t)(now - nextLook) > 0) {
      nextLook = now + 1200 + esp_random() % 4000;
      if (esp_random() % 3 == 0) { tLookX = 0; tLookY = 0; }
      else { tLookX = ((int)(esp_random() % 200) - 100) / 100.0f; tLookY = ((int)(esp_random() % 120) - 60) / 100.0f; }
    }
    float tx = tLookX, ty = tLookY;
    if (e == E_SAD || e == E_CRY || e == E_BORED) ty = 0.7f;
    if (e == E_FOCUSED) { tx = 0; ty = 0.3f; }
    if (e == E_SUSPICIOUS) { tx = sinf(now / 600.0f); ty = 0; }
    if (e == E_DIZZY || e == E_LOVE || e == E_SLEEP) { tx = 0; ty = 0; }
    lookX += (tx - lookX) * 0.3f;
    lookY += (ty - lookY) * 0.3f;

    if ((int32_t)(now - nextBlink) > 0) {
      blinkStart = now;
      blinkLen = (e == E_SLEEPY) ? 600 : 150;
      nextBlink = now + 2200 + esp_random() % 4000;
    }

    EyeParams tL, tR;
    getEyeTarget(e, true, tL);
    getEyeTarget(e, false, tR);
    const float sp = 0.3f;
    curL.w += (tL.w - curL.w) * sp; curL.h += (tL.h - curL.h) * sp;
    curL.tL += (tL.tL - curL.tL) * sp; curL.tR += (tL.tR - curL.tR) * sp;
    curL.bL += (tL.bL - curL.bL) * sp; curL.bR += (tL.bR - curL.bR) * sp;
    curR.w += (tR.w - curR.w) * sp; curR.h += (tR.h - curR.h) * sp;
    curR.tL += (tR.tL - curR.tL) * sp; curR.tR += (tR.tR - curR.tR) * sp;
    curR.bL += (tR.bL - curR.bL) * sp; curR.bR += (tR.bR - curR.bR) * sp;

    float pt = 0.16f;
    if (e == E_LOVE || e == E_EXCITED || e == E_HAPPY || e == E_SURPRISED) pt = 0.42f;
    else if (e == E_ANGRY || e == E_SUSPICIOUS || e == E_FOCUSED) pt = 0.07f;
    else if (e == E_SLEEPY || e == E_BORED) pt = 0.24f;
    pupil += (pt - pupil) * 0.12f;
  }

  // A custom animation replaces the eyes on the full-size face.
  if (sc >= 0.9f && cust[e].anim[0] && drawAnimFrame(cust[e].anim)) return;

  float open = 1;
  uint32_t bt = now - blinkStart;
  if (bt < blinkLen) open = fabsf((float)bt - blinkLen / 2.0f) / (blinkLen / 2.0f);
  if (open < 0.08f) open = 0.08f;

  float G = look.gap * sc;
  int lx = (int)(lookX * 8 * sc), ly = (int)(lookY * 6 * sc);
  int bob = (int)(sinf(now / 700.0f) * sc);
  int L = cx - (int)(G / 2 + curL.w * sc / 2) + lx, Rx = cx + (int)(G / 2 + curR.w * sc / 2) + lx, Y = cy + ly + bob;
  bool builtinShape = !cust[e].on;

  if (look.style == ES_SCANNER) {
    drawScanner(cx + lx, Y, sc, e, open, now);
  } else if (e == E_LOVE && builtinShape) {
    int s = (int)(curL.w * sc * (1.0f + 0.08f * sinf(now / 150.0f)));
    heart(L, Y, s);
    heart(Rx, Y, s);
  } else if (e == E_DIZZY && builtinShape) {
    spiral(L, Y, (int)(curL.w * sc), now);
    spiral(Rx, Y, (int)(curR.w * sc), now);
  } else {
    drawEye(L, Y, sc, curL, open);
    drawEye(Rx, Y, sc, curR, open);
    if (e == E_CRY) {
      for (int k = 0; k < 2; k++) {
        int tx = k ? Rx + (int)(curR.w * sc) / 2 - 4 : L - (int)(curL.w * sc) / 2 + 4;
        int ty = Y + (int)(curR.h * sc) / 2 + (int)(((now / 25) + k * 9) % (int)(20 * sc + 1));
        u8g2.drawDisc(tx, ty, max(1, (int)(2 * sc)));
      }
    }
    if (e == E_COOL && sc > 0.7f) {
      if (Y + (curL.h * sc) / 2 + 6 < 63) u8g2.drawLine(cx - 6, Y + (int)(curL.h * sc / 2) + 6, cx + 7, Y + (int)(curL.h * sc / 2) + 4);
    }
  }
  if (e == E_EXCITED && sc > 0.7f) {
    for (int i = 0; i < 3; i++) {
      if (((now / 220) + i) % 3 == 0) continue;
      int sx = (i == 0) ? 8 : (i == 1 ? 120 : 64), sy = (i == 2) ? 4 : 12 + i * 8;
      u8g2.drawHLine(sx - 3, sy, 7);
      u8g2.drawVLine(sx, sy - 3, 7);
    }
  }
  if ((e == E_SLEEP || e == E_SLEEPY) && sc > 0.7f && (e == E_SLEEP || sleeping)) {
    u8g2.setFont(F_SMALL);
    for (int k = 0; k < 3; k++) {
      int ph = (int)((now / 40 + k * 40) % 120);
      u8g2.drawStr(Rx + (int)(curR.w * sc) / 2 - 10 + k * 6 + ph / 12, Y - (int)(curR.h * sc / 4) - ph / 5, "z");
    }
  }
  if (sc >= 0.9f && !sleeping && !tempActive(now)) needBubble(now);

  if (audioSpeaking() && sc > 0.7f) {
    int mh = 2 + (int)((now / 70) % 5);
    int my = cy + (int)(curL.h * sc / 2) + 4;
    if (my + mh < 64) u8g2.drawRBox(cx - 8, my, 16, mh, mh > 3 ? 1 : 0);
  }
}
