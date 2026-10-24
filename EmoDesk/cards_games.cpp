#include "cards.h"

// Games use raw input: each touch is delivered instantly as G_PRESS.
// Hold the pad ~1.5 s (G_LONG) to leave the game.
static int steps(uint32_t& last) {
  uint32_t now = millis();
  if (!last || now - last > 300) { last = now; return 1; }
  int n = (now - last) / 33;
  last += n * 33;
  return n;
}
static bool isAction(uint8_t g) { return g == G_PRESS || g == G_TAP || g == G_DOUBLE; }
static bool gameExit(uint8_t g) { if (g == G_LONG) { cardsNext(1); return true; } return false; }
static uint32_t rnd(uint32_t n) { return n ? esp_random() % n : 0; }

static void gameOverScreen(const char* title, uint32_t score, uint32_t best) {
  u8g2.setDrawColor(0);
  u8g2.drawBox(14, 14, 100, 38);
  u8g2.setDrawColor(1);
  u8g2.drawFrame(14, 14, 100, 38);
  u8g2.setFont(F_BOLD);
  textCenter(28, title);
  char b[32];
  snprintf(b, sizeof(b), "Score %lu  Best %lu", (unsigned long)score, (unsigned long)best);
  u8g2.setFont(F_SMALL);
  textCenter(42, b);
}
static void startScreen(const char* title, const char* hint, uint32_t best) {
  u8g2.setFont(F_BOLD);
  textCenter(24, title);
  u8g2.setFont(F_SMALL);
  textCenter(38, hint);
  char b[24];
  snprintf(b, sizeof(b), "Best %lu", (unsigned long)best);
  textCenter(50, b);
  u8g2.setFont(F_TINY);
  textCenter(62, "hold to leave");
}
static void scoreTop(uint32_t s) {
  char b[12];
  snprintf(b, sizeof(b), "%lu", (unsigned long)s);
  u8g2.setFont(F_MED);
  textCenter(9, b);
}
static void lose(const char* game, uint8_t& st, uint32_t& deadAt, uint32_t score, uint32_t& best) {
  st = 2;
  deadAt = millis();
  highScore(game, score, best);
  audioSfx("gameover", EV_GAME);
  emoSet(score > 5 ? E_HAPPY : E_SAD, 2000);
}

// ================================================================== flappy
struct Flappy { uint8_t st; float y, vy, px[2]; int8_t gy[2]; bool sc[2]; uint32_t score, best, last, deadAt; };
static void flappy_reset(Flappy* g) {
  g->y = 32; g->vy = 0; g->score = 0;
  for (int i = 0; i < 2; i++) { g->px[i] = 128 + i * 70; g->gy[i] = 16 + rnd(32); g->sc[i] = false; }
}
static void flappy_enter(CardCtx& c) { Flappy* g = (Flappy*)c.d; g->st = 0; g->best = getHighScore("flappy"); flappy_reset(g); }
static void flappy_draw(CardCtx& c) {
  Flappy* g = (Flappy*)c.d;
  cardHold = true;
  if (g->st == 1) {
    for (int n = steps(g->last); n > 0; n--) {
      g->vy += 0.32f;
      g->y += g->vy;
      float sp = 1.6f + min(1.4f, g->score * 0.04f);
      for (int i = 0; i < 2; i++) {
        g->px[i] -= sp;
        if (g->px[i] < -12) { g->px[i] += 140; g->gy[i] = 16 + rnd(32); g->sc[i] = false; }
        if (!g->sc[i] && g->px[i] + 10 < 20) { g->sc[i] = true; g->score++; audioSfx("coin", EV_GAME); }
        bool inX = 23 > g->px[i] && 17 < g->px[i] + 10;
        if (inX && (g->y - 3 < g->gy[i] - 13 || g->y + 3 > g->gy[i] + 13)) lose("flappy", g->st, g->deadAt, g->score, g->best);
      }
      if (g->y > 63 || g->y < 0) lose("flappy", g->st, g->deadAt, g->score, g->best);
      if (g->st != 1) break;
    }
  }
  for (int i = 0; i < 2; i++) {
    int x = (int)g->px[i];
    u8g2.drawBox(x, 0, 10, g->gy[i] - 13);
    u8g2.drawBox(x, g->gy[i] + 13, 10, 64 - (g->gy[i] + 13));
  }
  u8g2.drawDisc(20, (int)g->y, 3);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(21, (int)g->y - 1);
  u8g2.setDrawColor(1);
  if (g->st == 0) { u8g2.setDrawColor(0); u8g2.drawBox(20, 14, 88, 40); u8g2.setDrawColor(1); startScreen("Flappy Emo", "Tap to flap", g->best); }
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Game over", g->score, g->best);
}
static bool flappy_input(CardCtx& c, uint8_t gs) {
  if (gameExit(gs)) return true;
  if (!isAction(gs)) return gs == G_RELEASE;
  Flappy* g = (Flappy*)c.d;
  if (g->st == 0) { flappy_reset(g); g->st = 1; g->vy = -3.2f; g->last = 0; }
  else if (g->st == 1) { g->vy = -3.3f; audioSfx("tap", EV_GAME); }
  else if (millis() - g->deadAt > 700) g->st = 0;
  return true;
}
const CardType CT_flappy = {"flappy", sizeof(Flappy), 0, CF_RAW | CF_NOROT, flappy_enter, flappy_draw, flappy_input, nullptr, nullptr, nullptr};

// ================================================================== dino runner
struct Dino { uint8_t st; float y, vy, speed, ox[3]; uint8_t ow[3], oh[3]; uint32_t score, best, last, deadAt; float dist; };
static void dino_reset(Dino* g) {
  g->y = 0; g->vy = 0; g->speed = 2.2f; g->score = 0; g->dist = 0;
  float x = 140;
  for (int i = 0; i < 3; i++) { g->ox[i] = x; g->ow[i] = 4 + rnd(6); g->oh[i] = 8 + rnd(8); x += 60 + rnd(60); }
}
static void dino_enter(CardCtx& c) { Dino* g = (Dino*)c.d; g->st = 0; g->best = getHighScore("dino"); dino_reset(g); }
static void dino_draw(CardCtx& c) {
  Dino* g = (Dino*)c.d;
  cardHold = true;
  const int GY = 56;
  if (g->st == 1) {
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
        if (g->ox[i] < -10) { g->ox[i] = max(130.0f, far + 50 + rnd(70)); g->ow[i] = 4 + rnd(6); g->oh[i] = 8 + rnd(9); }
        int dy = GY + (int)g->y;
        if (18 > g->ox[i] && 10 < g->ox[i] + g->ow[i] && dy > GY - g->oh[i] + 1) lose("dino", g->st, g->deadAt, g->score, g->best);
      }
    }
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
  if (g->st == 0) startScreen("Emo Runner", "Tap to jump", g->best);
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Ouch!", g->score, g->best);
}
static bool dino_input(CardCtx& c, uint8_t gs) {
  if (gameExit(gs)) return true;
  if (!isAction(gs)) return gs == G_RELEASE;
  Dino* g = (Dino*)c.d;
  if (g->st == 0) { dino_reset(g); g->st = 1; g->last = 0; }
  else if (g->st == 1) { if (g->y == 0) { g->vy = -5.4f; audioSfx("jump", EV_GAME); } }
  else if (millis() - g->deadAt > 700) g->st = 0;
  return true;
}
const CardType CT_dino = {"dino", sizeof(Dino), 0, CF_RAW | CF_NOROT, dino_enter, dino_draw, dino_input, nullptr, nullptr, nullptr};

// ================================================================== stack
struct Stack { uint8_t st, n; float x, dir, speed; int16_t w; int16_t hx[10], hw[10]; uint32_t score, best, last, deadAt; };
static void stack_reset(Stack* g) {
  g->n = 1; g->hx[0] = 34; g->hw[0] = 60; g->w = 60; g->x = 0; g->dir = 1; g->speed = 1.5f; g->score = 0;
}
static void stack_enter(CardCtx& c) { Stack* g = (Stack*)c.d; g->st = 0; g->best = getHighScore("stack"); stack_reset(g); }
static void stack_draw(CardCtx& c) {
  Stack* g = (Stack*)c.d;
  cardHold = true;
  if (g->st == 1) {
    for (int n = steps(g->last); n > 0; n--) {
      g->x += g->dir * g->speed;
      if (g->x < 0) { g->x = 0; g->dir = 1; }
      if (g->x + g->w > 128) { g->x = 128 - g->w; g->dir = -1; }
    }
  }
  int shown = min((int)g->n, 8);
  for (int i = 0; i < shown; i++) {
    int k = g->n - shown + i;
    u8g2.drawBox(g->hx[k % 10], 58 - i * 6, g->hw[k % 10], 5);
  }
  if (g->st == 1) u8g2.drawBox((int)g->x, 58 - shown * 6, g->w, 5);
  if (g->st == 0) { u8g2.setDrawColor(0); u8g2.drawBox(16, 12, 96, 40); u8g2.setDrawColor(1); startScreen("Stack", "Tap to drop", g->best); }
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Toppled!", g->score, g->best);
}
static bool stack_input(CardCtx& c, uint8_t gs) {
  if (gameExit(gs)) return true;
  if (!isAction(gs)) return gs == G_RELEASE;
  Stack* g = (Stack*)c.d;
  if (g->st == 0) { stack_reset(g); g->st = 1; g->last = 0; return true; }
  if (g->st == 2) { if (millis() - g->deadAt > 700) g->st = 0; return true; }
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
const CardType CT_stack = {"stack", sizeof(Stack), 0, CF_RAW | CF_NOROT, stack_enter, stack_draw, stack_input, nullptr, nullptr, nullptr};

// ================================================================== lane dodge
struct Dodge { uint8_t st, lane; int8_t dir; float oy[4], speed; uint8_t ol[4]; uint32_t score, best, last, deadAt; };
static const int LANES[3] = {26, 64, 102};
static void dodge_reset(Dodge* g) {
  g->lane = 1; g->dir = 1; g->speed = 1.2f; g->score = 0;
  for (int i = 0; i < 4; i++) { g->oy[i] = -10 - i * 24.0f; g->ol[i] = rnd(3); }
}
static void dodge_enter(CardCtx& c) { Dodge* g = (Dodge*)c.d; g->st = 0; g->best = getHighScore("dodge"); dodge_reset(g); }
static void dodge_draw(CardCtx& c) {
  Dodge* g = (Dodge*)c.d;
  cardHold = true;
  if (g->st == 1) {
    for (int n = steps(g->last); n > 0 && g->st == 1; n--) {
      g->speed = min(3.5f, g->speed + 0.0012f);
      for (int i = 0; i < 4; i++) {
        g->oy[i] += g->speed;
        if (g->oy[i] > 66) { g->oy[i] -= 96; g->ol[i] = rnd(3); g->score++; }
        if (g->ol[i] == g->lane && g->oy[i] > 46 && g->oy[i] < 62) lose("dodge", g->st, g->deadAt, g->score, g->best);
      }
    }
  }
  for (int k = 0; k < 2; k++) for (int y = (int)(millis() / 30) % 8 - 8; y < 64; y += 8) u8g2.drawVLine(45 + k * 38, y, 4);
  for (int i = 0; i < 4; i++) if (g->oy[i] > -8) u8g2.drawRBox(LANES[g->ol[i]] - 6, (int)g->oy[i], 12, 8, 2);
  int px = LANES[g->lane];
  u8g2.drawRFrame(px - 6, 52, 12, 10, 2);
  u8g2.drawBox(px - 3, 54, 6, 3);
  if (g->st == 0) { u8g2.setDrawColor(0); u8g2.drawBox(16, 12, 96, 40); u8g2.setDrawColor(1); startScreen("Dodge", "Tap to switch lanes", g->best); }
  else scoreTop(g->score);
  if (g->st == 2) gameOverScreen("Crash!", g->score, g->best);
}
static bool dodge_input(CardCtx& c, uint8_t gs) {
  if (gameExit(gs)) return true;
  if (!isAction(gs)) return gs == G_RELEASE;
  Dodge* g = (Dodge*)c.d;
  if (g->st == 0) { dodge_reset(g); g->st = 1; g->last = 0; }
  else if (g->st == 1) {
    if ((g->lane == 2 && g->dir > 0) || (g->lane == 0 && g->dir < 0)) g->dir = -g->dir;
    g->lane += g->dir;
    audioSfx("click", EV_GAME);
  } else if (millis() - g->deadAt > 700) g->st = 0;
  return true;
}
const CardType CT_dodge = {"dodge", sizeof(Dodge), 0, CF_RAW | CF_NOROT, dodge_enter, dodge_draw, dodge_input, nullptr, nullptr, nullptr};

// ================================================================== reaction test
// st: 0 state(0 idle,1 wait,2 go,3 result,4 too soon) 1 goAt 2 result ms
static void reaction_draw(CardCtx& c) {
  cardHold = c.st[0] != 0;
  uint32_t best = stateDoc["hs"]["reaction"] | 0;
  char b[32];
  switch (c.st[0]) {
    case 0:
      snprintf(b, sizeof(b), best ? "Best %lu ms" : "No record yet", (unsigned long)best);
      startScreen("Reaction", "Tap, then wait...", 0);
      u8g2.setDrawColor(0); u8g2.drawBox(0, 42, 128, 10); u8g2.setDrawColor(1);
      u8g2.setFont(F_SMALL); textCenter(50, b);
      break;
    case 1:
      if (millis() >= c.st[1]) { c.st[0] = 2; c.st[1] = millis(); audioSfx("beep", EV_GAME); }
      u8g2.setFont(F_HELV14);
      textCenter(38, "Wait for it...");
      break;
    case 2:
      u8g2.drawBox(0, 0, 128, 64);
      u8g2.setDrawColor(0);
      u8g2.setFont(F_HELV14);
      textCenter(40, "TAP!");
      u8g2.setDrawColor(1);
      break;
    case 3:
      snprintf(b, sizeof(b), "%lu ms", (unsigned long)c.st[2]);
      u8g2.setFont(F_NUM24);
      textCenter(36, b);
      u8g2.setFont(F_SMALL);
      snprintf(b, sizeof(b), "Best %lu ms", (unsigned long)best);
      textCenter(54, b);
      break;
    default:
      u8g2.setFont(F_HELV14);
      textCenter(36, "Too soon!");
      u8g2.setFont(F_SMALL);
      textCenter(54, "Tap to try again");
  }
}
static bool reaction_input(CardCtx& c, uint8_t g) {
  if (gameExit(g)) return true;
  if (!isAction(g)) return g == G_RELEASE;
  switch (c.st[0]) {
    case 1: c.st[0] = 4; audioSfx("error", EV_GAME); emoSet(E_CONFUSED, 1500); break;
    case 2: {
      c.st[2] = millis() - c.st[1];
      c.st[0] = 3;
      uint32_t best = stateDoc["hs"]["reaction"] | 0;
      if (!best || c.st[2] < best) { stateDoc["hs"]["reaction"] = c.st[2]; markDirty(DIRTY_STATE); audioSfx("win", EV_GAME); emoSet(E_EXCITED, 2000); }
      else audioSfx("coin", EV_GAME);
      break;
    }
    default: c.st[0] = 1; c.st[1] = millis() + 1500 + rnd(3500);
  }
  return true;
}
const CardType CT_reaction = {"reaction", 0, 0, CF_RAW | CF_NOROT, nullptr, reaction_draw, reaction_input, nullptr, nullptr, nullptr};

// ================================================================== rhythm tap
struct Rhythm { uint8_t st, judge; uint64_t notes, hit; uint32_t start, score, best, combo, judgeAt, deadAt, lastBeat; };
#define RH_BEATS 48
#define RH_MS 500
static void rhythm_enter(CardCtx& c) { Rhythm* g = (Rhythm*)c.d; g->st = 0; g->best = getHighScore("rhythm"); }
static void rhythm_draw(CardCtx& c) {
  Rhythm* g = (Rhythm*)c.d;
  cardHold = true;
  if (g->st == 0) { startScreen("Rhythm", "Tap when notes hit the line", g->best); return; }
  uint32_t now = millis();
  int32_t el = (int32_t)(now - g->start);
  int beat = el / RH_MS;
  if (g->st == 1) {
    if (beat >= 0 && beat != (int)g->lastBeat && beat < RH_BEATS + 4) { g->lastBeat = beat; audioSfx(beat % 4 ? "tick" : "click", EV_GAME); }
    for (int i = 0; i < RH_BEATS; i++) {
      if (!(g->notes >> i & 1) || (g->hit >> i & 1)) continue;
      if (el - i * RH_MS > 160) { g->hit |= (uint64_t)1 << i; g->combo = 0; g->judge = 3; g->judgeAt = now; }
    }
    if (beat > RH_BEATS + 2) lose("rhythm", g->st, g->deadAt, g->score, g->best);
  }
  u8g2.drawVLine(16, 14, 40);
  u8g2.drawFrame(12, 28, 9, 12);
  for (int i = 0; i < RH_BEATS; i++) {
    if (!(g->notes >> i & 1) || (g->hit >> i & 1)) continue;
    int x = 16 + (i * RH_MS - el) / 6;
    if (x > 130) break;
    if (x > -6) u8g2.drawDisc(x, 34, 4);
  }
  scoreTop(g->score);
  if (now - g->judgeAt < 400) {
    static const char* const J[] = {"", "Perfect!", "Good", "Miss"};
    u8g2.setFont(F_SMALL);
    textCenter(62, J[g->judge]);
  } else if (g->combo > 2) {
    char b[12];
    snprintf(b, sizeof(b), "combo x%lu", (unsigned long)g->combo);
    u8g2.setFont(F_SMALL);
    textCenter(62, b);
  }
  if (g->st == 2) gameOverScreen("Song over", g->score, g->best);
}
static bool rhythm_input(CardCtx& c, uint8_t gs) {
  if (gameExit(gs)) return true;
  if (!isAction(gs)) return gs == G_RELEASE;
  Rhythm* g = (Rhythm*)c.d;
  if (g->st == 0) {
    g->notes = 0; g->hit = 0; g->score = 0; g->combo = 0; g->lastBeat = 9999;
    for (int i = 4; i < RH_BEATS; i++) if (i % 2 == 0 || rnd(3) == 0) g->notes |= (uint64_t)1 << i;
    g->start = millis() + 1000;
    g->st = 1;
    return true;
  }
  if (g->st == 2) { if (millis() - g->deadAt > 700) g->st = 0; return true; }
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
const CardType CT_rhythm = {"rhythm", sizeof(Rhythm), 0, CF_RAW | CF_NOROT, rhythm_enter, rhythm_draw, rhythm_input, nullptr, nullptr, nullptr};

// ================================================================== magic 8-ball (our own answers)
static const char* const ANSWERS[] = {"Absolutely", "Signs say yes", "Count on it", "Looks bright", "Yes, go for it",
                                      "Probably", "Ask again later", "Hard to tell", "Maybe tomorrow",
                                      "Focus and ask again", "Not likely", "Doubtful", "My sensors say no", "Nope",
                                      "Definitely not", "The stars are busy"};
// st: 0 state(0 idle 1 shaking 2 shown) 1 start 2 answer
static void eightball_draw(CardCtx& c) {
  uint32_t el = millis() - c.st[1];
  if (c.st[0] == 1 && el > 1200) {
    c.st[0] = 2;
    audioSay(ANSWERS[c.st[2]], EV_GAME);
    emoSet(c.st[2] < 6 ? E_HAPPY : (c.st[2] < 10 ? E_CONFUSED : E_SAD), 2500);
  }
  int jx = c.st[0] == 1 ? (int)rnd(7) - 3 : 0, jy = c.st[0] == 1 ? (int)rnd(5) - 2 : 0;
  u8g2.drawDisc(64 + jx, 32 + jy, 30);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(64 + jx, 32 + jy, 22);
  u8g2.setDrawColor(1);
  if (c.st[0] == 2) {
    u8g2.setFont(F_SMALL);
    wrapText(46 + jx, 28, 38, 8, ANSWERS[c.st[2]], 3);
  } else {
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(59 + jx, 38 + jy, c.st[0] ? "?" : "8");
    if (!c.st[0]) { u8g2.setFont(F_TINY); u8g2.drawStr(0, 62, "ask, then 2x tap"); }
  }
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
// st: 0 rolling 1 start 2 a 3 b
static void dice_draw(CardCtx& c) {
  const char* mode = c.p["mode"] | "dice";
  bool rolling = c.st[0] && millis() - c.st[1] < 900;
  if (c.st[0] && !rolling) { c.st[0] = 0; audioSfx("pop", EV_GAME); }
  if (!c.st[2]) { c.st[2] = 1 + rnd(6); c.st[3] = 1 + rnd(6); }
  int a = rolling ? 1 + rnd(6) : c.st[2], b = rolling ? 1 + rnd(6) : c.st[3];
  if (!strcmp(mode, "coin")) {
    float w = rolling ? fabsf(cosf(millis() / 60.0f)) : 1;
    int rx = max(1, (int)(26 * w));
    u8g2.drawFilledEllipse(64, 30, rx, 26);
    u8g2.setDrawColor(0);
    if (rx > 6) u8g2.drawEllipse(64, 30, max(1, rx - 4), 22);
    if (!rolling) { u8g2.setFont(F_HELV14); textCenter(36, a % 2 ? "H" : "T"); }
    u8g2.setDrawColor(1);
    u8g2.setFont(F_SMALL);
    if (!rolling) textCenter(63, a % 2 ? "Heads" : "Tails");
  } else if (!strcmp(mode, "2dice")) {
    die(14, 12, 40, a);
    die(74, 12, 40, b);
    char t[12];
    snprintf(t, sizeof(t), "Total %d", a + b);
    u8g2.setFont(F_SMALL);
    if (!rolling) textCenter(62, t);
  } else {
    die(40, 6, 48, a);
    u8g2.setFont(F_TINY);
    if (!rolling) u8g2.drawStr(0, 62, "2x tap: roll");
  }
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
  drawTitle(c, "Decide", "2x spin");
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
  u8g2.setFont(F_HELV14);
  const char* txt = o[show] | "?";
  textCenter(40, txt);
  if (!c.st[0] && c.st[1]) { u8g2.drawRFrame(4, 22, 120, 24, 5); }
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
