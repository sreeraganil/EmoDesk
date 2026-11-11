#include "cards.h"

// Idle/GameOver use normal gestures so single tap/triple tap NEVER trap navigation.
// Active gameplay enables raw mode (inputSetRaw(true)) for zero-latency G_PRESS controls.
static int steps(uint32_t& last) {
  uint32_t now = millis();
  if (!last || now - last > 300) { last = now; return 1; }
  int n = (now - last) / 33;
  last += n * 33;
  return n;
}
static bool isPlayTap(uint8_t g) { return g == G_PRESS || g == G_TAP || g == G_DOUBLE; }
static bool leaveActiveGame(uint8_t g, uint8_t& st) {
  if (g == G_LONG) {
    inputSetRaw(false);
    st = 0;
    cardsNext(1);
    return true;
  }
  return false;
}
static uint32_t rnd(uint32_t n) { return n ? esp_random() % n : 0; }

static void startScreen(const char* title, const char* hint, uint32_t best, const char* bestFmt = nullptr) {
  u8g2.setDrawColor(0);
  u8g2.drawBox(4, 3, 120, 58);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(4, 3, 120, 58, 5);
  u8g2.setFont(F_BOLD);
  drawTextFitCenter(64, 19, 112, title);
  drawDottedHLine(16, 22, 96);
  u8g2.setFont(F_SMALL);
  drawTextFitCenter(64, 33, 112, hint);
  char b[28];
  if (bestFmt) safeCopy(b, bestFmt, sizeof(b));
  else snprintf(b, sizeof(b), "Best: %lu", (unsigned long)best);
  drawTextFitCenter(64, 45, 112, b);
  u8g2.setFont(F_TINY);
  textCenter(57, "tap: next   |   3x: home");
}

static void gameOverScreen(const char* title, uint32_t score, uint32_t best) {
  u8g2.setDrawColor(0);
  u8g2.drawBox(10, 9, 108, 46);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(10, 9, 108, 46, 4);
  u8g2.setFont(F_BOLD);
  drawTextFitCenter(64, 24, 100, title);
  char b[32];
  snprintf(b, sizeof(b), "Score %lu  ·  Best %lu", (unsigned long)score, (unsigned long)best);
  u8g2.setFont(F_SMALL);
  drawTextFitCenter(64, 37, 100, b);
  u8g2.setFont(F_TINY);
  textCenter(50, "2x: retry   |   tap: exit");
}

static void scoreTop(uint32_t s) {
  char b[12];
  snprintf(b, sizeof(b), "%lu", (unsigned long)s);
  u8g2.setFont(F_MED);
  int w = u8g2.getStrWidth(b) + 8;
  int x = (128 - w) / 2;
  u8g2.setDrawColor(0);
  u8g2.drawBox(x, 0, w, 11);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(x, 0, w, 11, 2);
  textCenter(9, b);
}

static void lose(const char* game, uint8_t& st, uint32_t& deadAt, uint32_t score, uint32_t& best) {
  st = 2;
  deadAt = millis();
  inputSetRaw(false);
  highScore(game, score, best);
  audioSfx("gameover", EV_GAME);
  emoSet(score > 5 ? E_HAPPY : E_SAD, 2000);
}

// ================================================================== flappy
struct Flappy { uint8_t st; float y, vy, px[2]; int8_t gy[2]; bool sc[2]; uint32_t score, best, last, deadAt; };
static void flappy_reset(Flappy* g) {
  g->y = 32; g->vy = 0; g->score = 0;
  for (int i = 0; i < 2; i++) { g->px[i] = 128 + i * 70; g->gy[i] = 18 + rnd(28); g->sc[i] = false; }
}
static void flappy_enter(CardCtx& c) {
  inputSetRaw(false);
  Flappy* g = (Flappy*)c.d;
  if (!g) return;
  g->st = 0;
  g->best = getHighScore("flappy");
  flappy_reset(g);
}
static void flappy_draw(CardCtx& c) {
  Flappy* g = (Flappy*)c.d;
  if (!g) return;
  if (g->st == 1) {
    cardHold = true;
    inputSetRaw(true);
    for (int n = steps(g->last); n > 0; n--) {
      g->vy += 0.32f;
      g->y += g->vy;
      float sp = 1.6f + min(1.4f, g->score * 0.04f);
      for (int i = 0; i < 2; i++) {
        g->px[i] -= sp;
        if (g->px[i] < -12) { g->px[i] += 140; g->gy[i] = 18 + rnd(28); g->sc[i] = false; }
        if (!g->sc[i] && g->px[i] + 10 < 20) { g->sc[i] = true; g->score++; audioSfx("coin", EV_GAME); }
        bool inX = 23 > g->px[i] && 17 < g->px[i] + 10;
        if (inX && (g->y - 3 < g->gy[i] - 13 || g->y + 3 > g->gy[i] + 13)) lose("flappy", g->st, g->deadAt, g->score, g->best);
      }
      if (g->y > 62 || g->y < 1) lose("flappy", g->st, g->deadAt, g->score, g->best);
      if (g->st != 1) break;
    }
  } else {
    inputSetRaw(false);
  }
  for (int i = 0; i < 2; i++) {
    int x = (int)g->px[i];
    u8g2.drawBox(x, 0, 10, max(0, g->gy[i] - 13));
    u8g2.drawBox(x, g->gy[i] + 13, 10, max(0, 64 - (g->gy[i] + 13)));
  }
  u8g2.drawDisc(20, (int)g->y, 3);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(21, (int)g->y - 1);
  u8g2.setDrawColor(1);
  if (g->st == 0) startScreen("Flappy Emo", "2x to start · hold to flap", g->best);
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Game Over", g->score, g->best);
}
static bool flappy_input(CardCtx& c, uint8_t gs) {
  Flappy* g = (Flappy*)c.d;
  if (!g) return false;
  if (g->st == 1) {
    if (leaveActiveGame(gs, g->st)) return true;
    if (gs == G_RELEASE) return true;
    if (isPlayTap(gs)) { g->vy = -3.3f; audioSfx("tap", EV_GAME); return true; }
    return false;
  }
  if (gs == G_DOUBLE) {
    if (g->st == 2 && millis() - g->deadAt <= 500) return true;
    flappy_reset(g); g->st = 1; g->vy = -3.2f; g->last = 0;
    inputSetRaw(true);
    return true;
  }
  if (gs == G_LONG) { g->st = 0; flappy_reset(g); return true; }
  return false;
}
const CardType CT_flappy = {"flappy", sizeof(Flappy), 0, CF_NOROT, flappy_enter, flappy_draw, flappy_input, nullptr, nullptr, nullptr};

// ================================================================== dino runner
struct Dino { uint8_t st; float y, vy, speed, ox[3]; uint8_t ow[3], oh[3]; uint32_t score, best, last, deadAt; float dist; };
static void dino_reset(Dino* g) {
  g->y = 0; g->vy = 0; g->speed = 2.2f; g->score = 0; g->dist = 0;
  float x = 140;
  for (int i = 0; i < 3; i++) { g->ox[i] = x; g->ow[i] = 4 + rnd(6); g->oh[i] = 8 + rnd(8); x += 62 + rnd(58); }
}
static void dino_enter(CardCtx& c) {
  inputSetRaw(false);
  Dino* g = (Dino*)c.d;
  if (!g) return;
  g->st = 0;
  g->best = getHighScore("dino");
  dino_reset(g);
}
static void dino_draw(CardCtx& c) {
  Dino* g = (Dino*)c.d;
  if (!g) return;
  const int GY = 56;
  if (g->st == 1) {
    cardHold = true;
    inputSetRaw(true);
    for (int n = steps(g->last); n > 0 && g->st == 1; n--) {
      g->vy += 0.42f;
      g->y += g->vy;
      if (g->y > 0) { g->y = 0; g->vy = 0; }
      g->speed = min(5.5f, g->speed + 0.0015f);
      g->dist += g->speed;
      g->score = (uint32_t)(g->dist / 12);
      float far = 0;
      for (int i = 0; i < 3; i++) far = max(far, g->ox[i]);
      for (int i = 0; i < 3; i++) {
        g->ox[i] -= g->speed;
        if (g->ox[i] < -10) { g->ox[i] = max(130.0f, far + 52 + rnd(68)); g->ow[i] = 4 + rnd(6); g->oh[i] = 8 + rnd(9); }
        int dy = GY + (int)g->y;
        if (18 > g->ox[i] && 10 < g->ox[i] + g->ow[i] && dy > GY - g->oh[i] + 1) lose("dino", g->st, g->deadAt, g->score, g->best);
      }
    }
  } else {
    inputSetRaw(false);
  }
  u8g2.drawHLine(0, GY, 128);
  for (int k = 0; k < 6; k++) u8g2.drawPixel((int)(128 - fmodf(g->dist * 1.0f + k * 23, 128)), GY + 3 + k % 3);
  int dy = GY + (int)g->y;
  u8g2.drawBox(10, dy - 12, 7, 8);
  u8g2.drawBox(13, dy - 16, 8, 5);
  u8g2.setDrawColor(0); u8g2.drawPixel(18, dy - 15); u8g2.setDrawColor(1);
  bool step = (millis() / 100) % 2 && g->y == 0;
  u8g2.drawVLine(11, dy - 4, step ? 4 : 3);
  u8g2.drawVLine(15, dy - 4, step ? 3 : 4);
  for (int i = 0; i < 3; i++) {
    int x = (int)g->ox[i];
    u8g2.drawBox(x, GY - g->oh[i], g->ow[i], g->oh[i]);
    if (g->oh[i] > 11) { u8g2.drawVLine(x - 2, GY - g->oh[i] + 3, 4); u8g2.drawVLine(x + g->ow[i] + 1, GY - g->oh[i] + 2, 4); }
  }
  if (g->st == 0) startScreen("Emo Runner", "2x to run · tap to jump", g->best);
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Ouch!", g->score, g->best);
}
static bool dino_input(CardCtx& c, uint8_t gs) {
  Dino* g = (Dino*)c.d;
  if (!g) return false;
  if (g->st == 1) {
    if (leaveActiveGame(gs, g->st)) return true;
    if (gs == G_RELEASE) return true;
    if (isPlayTap(gs)) { if (g->y == 0) { g->vy = -5.4f; audioSfx("jump", EV_GAME); } return true; }
    return false;
  }
  if (gs == G_DOUBLE) {
    if (g->st == 2 && millis() - g->deadAt <= 500) return true;
    dino_reset(g); g->st = 1; g->last = 0;
    inputSetRaw(true);
    return true;
  }
  if (gs == G_LONG) { g->st = 0; dino_reset(g); return true; }
  return false;
}
const CardType CT_dino = {"dino", sizeof(Dino), 0, CF_NOROT, dino_enter, dino_draw, dino_input, nullptr, nullptr, nullptr};

// ================================================================== stack
struct Stack { uint8_t st, n; float x, dir, speed; int16_t w; int16_t hx[10], hw[10]; uint32_t score, best, last, deadAt; };
static void stack_reset(Stack* g) {
  g->n = 1; g->hx[0] = 34; g->hw[0] = 60; g->w = 60; g->x = 0; g->dir = 1; g->speed = 1.5f; g->score = 0;
}
static void stack_enter(CardCtx& c) {
  inputSetRaw(false);
  Stack* g = (Stack*)c.d;
  if (!g) return;
  g->st = 0;
  g->best = getHighScore("stack");
  stack_reset(g);
}
static void stack_draw(CardCtx& c) {
  Stack* g = (Stack*)c.d;
  if (!g) return;
  if (g->st == 1) {
    cardHold = true;
    inputSetRaw(true);
    for (int n = steps(g->last); n > 0; n--) {
      g->x += g->dir * g->speed;
      if (g->x < 0) { g->x = 0; g->dir = 1; }
      if (g->x + g->w > 128) { g->x = 128 - g->w; g->dir = -1; }
    }
  } else {
    inputSetRaw(false);
  }
  int shown = min((int)g->n, 8);
  for (int i = 0; i < shown; i++) {
    int k = g->n - shown + i;
    u8g2.drawBox(g->hx[k % 10], 58 - i * 6, g->hw[k % 10], 5);
  }
  if (g->st == 1) u8g2.drawBox((int)g->x, 58 - shown * 6, g->w, 5);
  if (g->st == 0) startScreen("Stack", "2x to play · tap to drop", g->best);
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Toppled!", g->score, g->best);
}
static bool stack_input(CardCtx& c, uint8_t gs) {
  Stack* g = (Stack*)c.d;
  if (!g) return false;
  if (g->st == 1) {
    if (leaveActiveGame(gs, g->st)) return true;
    if (gs == G_RELEASE) return true;
    if (!isPlayTap(gs)) return false;
    int lx = g->hx[(g->n - 1) % 10], lw = g->hw[(g->n - 1) % 10];
    int nx = max((int)g->x, lx), ne = min((int)g->x + g->w, lx + lw);
    if (ne <= nx) { lose("stack", g->st, g->deadAt, g->score, g->best); return true; }
    bool perfect = abs((int)g->x - lx) <= 2;
    if (perfect) { nx = lx; ne = lx + lw; }
    g->hx[g->n % 10] = nx;
    g->hw[g->n % 10] = ne - nx;
    g->w = ne - nx;
    g->n++;
    g->score++;
    g->speed = min(4.5f, g->speed + 0.12f);
    g->x = g->dir > 0 ? 0 : 128 - g->w;
    audioSfx(perfect ? "coin" : "pop", EV_GAME);
    return true;
  }
  if (gs == G_DOUBLE) {
    if (g->st == 2 && millis() - g->deadAt <= 500) return true;
    stack_reset(g); g->st = 1; g->last = 0;
    inputSetRaw(true);
    return true;
  }
  if (gs == G_LONG) { g->st = 0; stack_reset(g); return true; }
  return false;
}
const CardType CT_stack = {"stack", sizeof(Stack), 0, CF_NOROT, stack_enter, stack_draw, stack_input, nullptr, nullptr, nullptr};

// ================================================================== lane dodge
struct Dodge { uint8_t st, lane; int8_t dir; float oy[4], speed; uint8_t ol[4]; uint32_t score, best, last, deadAt; };
static const int LANES[3] = {26, 64, 102};
static void dodge_reset(Dodge* g) {
  g->lane = 1; g->dir = 1; g->speed = 1.2f; g->score = 0;
  for (int i = 0; i < 4; i++) { g->oy[i] = -10 - i * 24.0f; g->ol[i] = rnd(3); }
}
static void dodge_enter(CardCtx& c) {
  inputSetRaw(false);
  Dodge* g = (Dodge*)c.d;
  if (!g) return;
  g->st = 0;
  g->best = getHighScore("dodge");
  dodge_reset(g);
}
static void dodge_draw(CardCtx& c) {
  Dodge* g = (Dodge*)c.d;
  if (!g) return;
  if (g->st == 1) {
    cardHold = true;
    inputSetRaw(true);
    for (int n = steps(g->last); n > 0 && g->st == 1; n--) {
      g->speed = min(3.5f, g->speed + 0.0012f);
      for (int i = 0; i < 4; i++) {
        g->oy[i] += g->speed;
        if (g->oy[i] > 66) { g->oy[i] -= 96; g->ol[i] = rnd(3); g->score++; }
        if (g->ol[i] == g->lane && g->oy[i] > 46 && g->oy[i] < 62) lose("dodge", g->st, g->deadAt, g->score, g->best);
      }
    }
  } else {
    inputSetRaw(false);
  }
  for (int k = 0; k < 2; k++) for (int y = (int)(millis() / 30) % 8 - 8; y < 64; y += 8) u8g2.drawVLine(45 + k * 38, y, 4);
  for (int i = 0; i < 4; i++) if (g->oy[i] > -8) u8g2.drawRBox(LANES[g->ol[i]] - 6, (int)g->oy[i], 12, 8, 2);
  int px = LANES[g->lane];
  u8g2.drawRFrame(px - 6, 52, 12, 10, 2);
  u8g2.drawBox(px - 3, 54, 6, 3);
  if (g->st == 0) startScreen("Lane Dodge", "2x to play · tap to steer", g->best);
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Crash!", g->score, g->best);
}
static bool dodge_input(CardCtx& c, uint8_t gs) {
  Dodge* g = (Dodge*)c.d;
  if (!g) return false;
  if (g->st == 1) {
    if (leaveActiveGame(gs, g->st)) return true;
    if (gs == G_RELEASE) return true;
    if (!isPlayTap(gs)) return false;
    if ((g->lane == 2 && g->dir > 0) || (g->lane == 0 && g->dir < 0)) g->dir = -g->dir;
    g->lane += g->dir;
    audioSfx("click", EV_GAME);
    return true;
  }
  if (gs == G_DOUBLE) {
    if (g->st == 2 && millis() - g->deadAt <= 500) return true;
    dodge_reset(g); g->st = 1; g->last = 0;
    inputSetRaw(true);
    return true;
  }
  if (gs == G_LONG) { g->st = 0; dodge_reset(g); return true; }
  return false;
}
const CardType CT_dodge = {"dodge", sizeof(Dodge), 0, CF_NOROT, dodge_enter, dodge_draw, dodge_input, nullptr, nullptr, nullptr};

// ================================================================== reaction test
// st: 0 idle, 1 wait(raw), 2 go(raw), 3 result, 4 too soon
static void reaction_enter(CardCtx& c) { inputSetRaw(false); c.st[0] = 0; }
static void reaction_draw(CardCtx& c) {
  uint32_t best = stateDoc["hs"]["reaction"] | 0;
  char b[32];
  switch (c.st[0]) {
    case 0:
      inputSetRaw(false);
      snprintf(b, sizeof(b), best ? "Best: %lu ms" : "No record yet", (unsigned long)best);
      startScreen("Reaction", "2x tap to start test", 0, b);
      break;
    case 1:
      cardHold = true;
      inputSetRaw(true);
      if (millis() >= c.st[1]) { c.st[0] = 2; c.st[1] = millis(); audioSfx("beep", EV_GAME); }
      u8g2.drawCircle(64, 24, 6 + (millis() / 200) % 6);
      u8g2.setFont(F_HELV14);
      textCenter(46, "Wait for flash...");
      u8g2.setFont(F_TINY);
      textCenter(61, "hold: cancel");
      break;
    case 2:
      cardHold = true;
      inputSetRaw(true);
      u8g2.drawBox(0, 0, 128, 64);
      u8g2.setDrawColor(0);
      u8g2.setFont(F_HELV14);
      textCenter(38, "TAP NOW!");
      u8g2.setDrawColor(1);
      break;
    case 3:
      inputSetRaw(false);
      u8g2.drawRFrame(4, 3, 120, 58, 5);
      u8g2.setFont(F_TINY);
      textCenter(13, "REACTION TIME");
      snprintf(b, sizeof(b), "%lu ms", (unsigned long)c.st[2]);
      u8g2.setFont(F_NUM24);
      drawTextFitCenter(64, 38, 110, b);
      u8g2.setFont(F_SMALL);
      snprintf(b, sizeof(b), "Best: %lu ms", (unsigned long)best);
      drawTextFitCenter(64, 49, 110, b);
      u8g2.setFont(F_TINY);
      textCenter(58, "2x: retry   |   tap: next");
      break;
    default:
      inputSetRaw(false);
      u8g2.drawRFrame(4, 3, 120, 58, 5);
      u8g2.setFont(F_HELV14);
      textCenter(29, "Too Soon!");
      u8g2.setFont(F_SMALL);
      textCenter(42, "Wait for white flash");
      u8g2.setFont(F_TINY);
      textCenter(57, "2x: retry   |   tap: next");
  }
}
static bool reaction_input(CardCtx& c, uint8_t g) {
  if (c.st[0] == 1 || c.st[0] == 2) {
    uint8_t dummy = 0;
    if (leaveActiveGame(g, dummy)) { c.st[0] = 0; return true; }
    if (g == G_RELEASE) return true;
    if (!isPlayTap(g)) return false;
    if (c.st[0] == 1) {
      c.st[0] = 4;
      inputSetRaw(false);
      audioSfx("error", EV_GAME);
      emoSet(E_CONFUSED, 1500);
    } else {
      c.st[2] = millis() - c.st[1];
      c.st[0] = 3;
      inputSetRaw(false);
      uint32_t best = stateDoc["hs"]["reaction"] | 0;
      if (!best || c.st[2] < best) { stateDoc["hs"]["reaction"] = c.st[2]; markDirty(DIRTY_STATE); audioSfx("win", EV_GAME); emoSet(E_EXCITED, 2000); }
      else audioSfx("coin", EV_GAME);
    }
    return true;
  }
  if (g == G_DOUBLE) {
    c.st[0] = 1;
    c.st[1] = millis() + 1500 + rnd(3500);
    inputSetRaw(true);
    return true;
  }
  if (g == G_LONG) { c.st[0] = 0; return true; }
  return false;
}
const CardType CT_reaction = {"reaction", 0, 0, CF_NOROT, reaction_enter, reaction_draw, reaction_input, nullptr, nullptr, nullptr};

// ================================================================== rhythm tap
struct Rhythm { uint8_t st, judge; uint64_t notes, hit; uint32_t start, score, best, combo, judgeAt, deadAt, lastBeat; };
#define RH_BEATS 48
#define RH_MS 500
static void rhythm_enter(CardCtx& c) {
  inputSetRaw(false);
  Rhythm* g = (Rhythm*)c.d;
  if (!g) return;
  g->st = 0;
  g->best = getHighScore("rhythm");
}
static void rhythm_draw(CardCtx& c) {
  Rhythm* g = (Rhythm*)c.d;
  if (!g) return;
  if (g->st == 0) {
    inputSetRaw(false);
    startScreen("Rhythm Tap", "2x to play · tap on beat", g->best);
    return;
  }
  uint32_t now = millis();
  int32_t el = (int32_t)(now - g->start);
  int beat = el / RH_MS;
  if (g->st == 1) {
    cardHold = true;
    inputSetRaw(true);
    if (beat >= 0 && beat != (int)g->lastBeat && beat < RH_BEATS + 4) { g->lastBeat = beat; audioSfx(beat % 4 ? "tick" : "click", EV_GAME); }
    for (int i = 0; i < RH_BEATS; i++) {
      if (!(g->notes >> i & 1) || (g->hit >> i & 1)) continue;
      if (el - i * RH_MS > 160) { g->hit |= (uint64_t)1 << i; g->combo = 0; g->judge = 3; g->judgeAt = now; }
    }
    if (beat > RH_BEATS + 2) lose("rhythm", g->st, g->deadAt, g->score, g->best);
  } else {
    inputSetRaw(false);
  }
  drawDottedHLine(0, 24, 128);
  drawDottedHLine(0, 34, 128);
  drawDottedHLine(0, 44, 128);
  u8g2.drawVLine(18, 15, 38);
  u8g2.drawRFrame(13, 27, 11, 14, 2);
  for (int i = 0; i < RH_BEATS; i++) {
    if (!(g->notes >> i & 1) || (g->hit >> i & 1)) continue;
    int x = 18 + (i * RH_MS - el) / 6;
    if (x > 130) break;
    if (x > -6) u8g2.drawDisc(x, 34, 4);
  }
  scoreTop(g->score);
  if (now - g->judgeAt < 400) {
    static const char* const J[] = {"", "Perfect!", "Good", "Miss"};
    u8g2.setFont(F_SMALL);
    textCenter(61, J[g->judge]);
  } else if (g->combo > 2) {
    char b[16];
    snprintf(b, sizeof(b), "Combo x%lu", (unsigned long)g->combo);
    u8g2.setFont(F_SMALL);
    textCenter(61, b);
  }
  if (g->st == 2) gameOverScreen("Song Over", g->score, g->best);
}
static bool rhythm_input(CardCtx& c, uint8_t gs) {
  Rhythm* g = (Rhythm*)c.d;
  if (!g) return false;
  if (g->st == 1) {
    if (leaveActiveGame(gs, g->st)) return true;
    if (gs == G_RELEASE) return true;
    if (!isPlayTap(gs)) return false;
    int32_t el = (int32_t)(millis() - g->start);
    int best = -1, bd = 9999;
    for (int i = 0; i < RH_BEATS; i++) {
      if (!(g->notes >> i & 1) || (g->hit >> i & 1)) continue;
      int d = abs(el - i * RH_MS);
      if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && bd < 150) {
      g->hit |= (uint64_t)1 << best;
      g->combo++;
      g->judge = bd < 60 ? 1 : 2;
      g->score += (g->judge == 1 ? 3 : 1) * (1 + g->combo / 8);
      audioSfx("tap", EV_GAME);
    } else { g->judge = 3; g->combo = 0; }
    g->judgeAt = millis();
    return true;
  }
  if (gs == G_DOUBLE) {
    if (g->st == 2 && millis() - g->deadAt <= 500) return true;
    g->notes = 0; g->hit = 0; g->score = 0; g->combo = 0; g->lastBeat = 9999;
    for (int i = 4; i < RH_BEATS; i++) if (i % 2 == 0 || rnd(3) == 0) g->notes |= (uint64_t)1 << i;
    g->start = millis() + 1000;
    g->st = 1;
    inputSetRaw(true);
    return true;
  }
  if (gs == G_LONG) { g->st = 0; return true; }
  return false;
}
const CardType CT_rhythm = {"rhythm", sizeof(Rhythm), 0, CF_NOROT, rhythm_enter, rhythm_draw, rhythm_input, nullptr, nullptr, nullptr};

// ================================================================== magic 8-ball
static const char* const ANSWERS[] = {"Absolutely", "Signs say yes", "Count on it", "Looks bright", "Yes, go for it",
                                      "Probably", "Ask again later", "Hard to tell", "Maybe tomorrow",
                                      "Focus and ask again", "Not likely", "Doubtful", "My sensors say no", "Nope",
                                      "Definitely not", "The stars are busy"};
static void eightball_draw(CardCtx& c) {
  uint32_t el = millis() - c.st[1];
  if (c.st[0] == 1 && el > 1200) {
    c.st[0] = 2;
    audioSay(ANSWERS[c.st[2]], EV_GAME);
    emoSet(c.st[2] < 6 ? E_HAPPY : (c.st[2] < 10 ? E_CONFUSED : E_SAD), 2500);
  }
  int jx = c.st[0] == 1 ? (int)rnd(7) - 3 : 0, jy = c.st[0] == 1 ? (int)rnd(5) - 2 : 0;
  int cy = 28 + jy;
  u8g2.drawDisc(64 + jx, cy, 26);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(64 + jx, cy, 19);
  u8g2.setDrawColor(1);
  if (c.st[0] == 2) {
    u8g2.setFont(F_TINY);
    wrapText(47 + jx, 22, 34, 7, ANSWERS[c.st[2]], 3);
  } else {
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(59 + jx, cy + 6, c.st[0] ? "?" : "8");
  }
  u8g2.setFont(F_TINY);
  textCenter(63, c.st[0] == 2 ? "2x tap: ask again" : "ask a question, then 2x tap");
}
static bool eightball_input(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  c.st[0] = 1;
  c.st[1] = millis();
  c.st[2] = rnd(sizeof(ANSWERS) / sizeof(ANSWERS[0]));
  audioSfx("whoosh", EV_GAME);
  return true;
}
static void eightball_speak(CardCtx& c, String& s) { if (c.st[0] == 2) s = ANSWERS[c.st[2]]; }
const CardType CT_eightball = {"eightball", 0, 0, 0, nullptr, eightball_draw, eightball_input, nullptr, eightball_speak, nullptr};

// ================================================================== dice / coin
static void die(int x, int y, int s, int v) {
  u8g2.drawRFrame(x, y, s, s, 5);
  int q = s / 4, cx = x + s / 2, cy = y + s / 2, r = max(2, s / 12);
  auto pip = [&](int dx, int dy) { u8g2.drawDisc(cx + dx * q, cy + dy * q, r); };
  if (v % 2) pip(0, 0);
  if (v >= 2) { pip(-1, -1); pip(1, 1); }
  if (v >= 4) { pip(1, -1); pip(-1, 1); }
  if (v == 6) { pip(-1, 0); pip(1, 0); }
}
static void dice_draw(CardCtx& c) {
  const char* mode = c.p["mode"] | "dice";
  bool rolling = c.st[0] && millis() - c.st[1] < 900;
  if (c.st[0] && !rolling) { c.st[0] = 0; audioSfx("pop", EV_GAME); }
  if (!c.st[2]) { c.st[2] = 1 + rnd(6); c.st[3] = 1 + rnd(6); }
  int a = rolling ? 1 + rnd(6) : c.st[2], b = rolling ? 1 + rnd(6) : c.st[3];
  char foot[28];
  if (!strcmp(mode, "coin")) {
    float w = rolling ? fabsf(cosf(millis() / 60.0f)) : 1;
    int rx = max(1, (int)(22 * w));
    u8g2.drawFilledEllipse(64, 27, rx, 22);
    u8g2.setDrawColor(0);
    if (rx > 6) u8g2.drawEllipse(64, 27, max(1, rx - 4), 18);
    if (!rolling) { u8g2.setFont(F_HELV14); textCenter(33, a % 2 ? "H" : "T"); }
    u8g2.setDrawColor(1);
    snprintf(foot, sizeof(foot), rolling ? "Flipping..." : "%s  |  2x: flip", a % 2 ? "Heads" : "Tails");
  } else if (!strcmp(mode, "2dice")) {
    die(18, 9, 38, a);
    die(72, 9, 38, b);
    snprintf(foot, sizeof(foot), rolling ? "Rolling..." : "Total %d  |  2x: roll", a + b);
  } else {
    die(43, 5, 42, a);
    snprintf(foot, sizeof(foot), rolling ? "Rolling..." : "Rolled %d  |  2x: roll", a);
  }
  u8g2.setFont(F_SMALL);
  textCenter(61, foot);
}
static bool dice_input(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  c.st[0] = 1;
  c.st[1] = millis();
  c.st[2] = 1 + rnd(6);
  c.st[3] = 1 + rnd(6);
  audioSfx("whoosh", EV_GAME);
  return true;
}
static void dice_speak(CardCtx& c, String& s) {
  const char* mode = c.p["mode"] | "dice";
  if (!strcmp(mode, "coin")) s = c.st[2] % 2 ? "Heads." : "Tails.";
  else if (!strcmp(mode, "2dice")) s = "You rolled " + numWords(c.st[2] + c.st[3]) + ".";
  else s = "You rolled " + numWords(c.st[2]) + ".";
}
const CardType CT_dice = {"dice", 0, 0, 0, nullptr, dice_draw, dice_input, nullptr, dice_speak, nullptr};

// ================================================================== decision maker
static void decision_draw(CardCtx& c) {
  JsonArray o = c.p["opts"];
  int n = o.isNull() ? 0 : o.size();
  drawTitle(c, "Decide", c.st[0] ? "spinning" : "2x spin");
  if (n < 2) { drawCenteredMsg("Add 2+ options", "in the card settings"); return; }
  uint32_t el = millis() - c.st[1];
  int show = c.st[2] % n;
  if (c.st[0]) {
    if (el < 2200) {
      uint32_t interval = 50 + el / 12;
      if (millis() - c.st[3] > interval) { c.st[3] = millis(); c.st[4]++; audioSfx("tick", EV_GAME); }
      show = c.st[4] % n;
    } else {
      c.st[0] = 0;
      c.st[4] = c.st[2];
      audioSfx("win", EV_GAME);
      emoSet(E_EXCITED, 2000);
      audioSay(o[show].as<const char*>() ? o[show].as<const char*>() : "", EV_GAME);
    }
  }
  const char* txt = o[show] | "?";
  u8g2.drawRFrame(4, 16, 120, 32, 5);
  u8g2.setFont(F_HELV14);
  if (u8g2.getUTF8Width(txt) <= 110) {
    textCenter(38, txt);
  } else {
    u8g2.setFont(F_BOLD);
    if (u8g2.getUTF8Width(txt) <= 110) textCenter(36, txt);
    else marquee(9, 36, 110, txt, c.t0, 26);
  }
  u8g2.setFont(F_TINY);
  textCenter(61, c.st[0] ? "choosing..." : "2x tap to spin again");
}
static bool decision_input(CardCtx& c, uint8_t g) {
  JsonArray o = c.p["opts"];
  int n = o.isNull() ? 0 : o.size();
  if (g != G_DOUBLE || n < 2) return false;
  c.st[0] = 1;
  c.st[1] = millis();
  c.st[2] = rnd(n);
  return true;
}
const CardType CT_decision = {"decision", 0, 0, 0, nullptr, decision_draw, decision_input, nullptr, nullptr, nullptr};
