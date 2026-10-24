#include "input.h"

struct Btn {
  uint8_t pin; bool activeHigh;
  bool st; uint32_t chg, down, lastUp;
  uint8_t taps; bool longF, vlongF;
};

static Btn btn[2];
static bool rawMode = false;
static uint16_t longMs = 700, vlongMs = 3000, multiMs = 320;
static uint32_t lastAct = 0;
struct GEv { uint8_t g, src; };
static GEv ring[12];
static uint8_t rh = 0, rt = 0;

static void push(uint8_t g, uint8_t src) {
  uint8_t n = (rh + 1) % 12;
  if (n == rt) return;
  ring[rh] = {g, src};
  rh = n;
  lastAct = millis();
}

static bool bootEn = true;

void inputApply() {
  bootEn = cfg["boot"]["en"] | true;
  if (!bootEn) {
    btn[1].st = false;
    btn[1].taps = 0;
    btn[1].longF = btn[1].vlongF = false;
  }
  longMs = cfg["input"]["longMs"] | 700;
  vlongMs = cfg["input"]["vlongMs"] | 3000;
  multiMs = cfg["input"]["multiMs"] | 320;
}

void inputBegin() {
  btn[0] = {PIN_TOUCH, true, false, 0, 0, 0, 0, false, false};
  btn[1] = {PIN_BOOT, false, false, 0, 0, 0, 0, false, false};
  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  inputApply();
}

void inputSetRaw(bool raw) {
  rawMode = raw;
  btn[0].taps = 0;
}

void inputPoll() {
  uint32_t now = millis();
  for (int i = 0; i < 2; i++) {
    if (i == SRC_BOOT && !bootEn) continue;
    Btn& b = btn[i];
    bool lvl = digitalRead(b.pin);
    bool pressed = b.activeHigh ? lvl : !lvl;
    bool raw = rawMode && i == SRC_TOUCH;
    if (pressed != b.st && now - b.chg > 25) {
      b.st = pressed;
      b.chg = now;
      lastAct = now;
      if (pressed) {
        b.down = now;
        b.longF = b.vlongF = false;
        if (raw) push(G_PRESS, i);
      } else {
        uint32_t d = now - b.down;
        if (raw) push(G_RELEASE, i);
        else if (b.vlongF) { /* already handled */ }
        else if (d >= longMs) push(G_LONG, i);
        else if (d >= 35) { b.taps++; b.lastUp = now; }
      }
    }
    if (b.st) {
      uint32_t d = now - b.down;
      if (raw && !b.longF && d >= (uint32_t)longMs * 2) { b.longF = true; push(G_LONG, i); }
      if (!raw && !b.vlongF && d >= vlongMs) { b.vlongF = true; b.taps = 0; push(G_VLONG, i); }
    }
    if (!raw && b.taps && !b.st && now - b.lastUp > multiMs) {
      push(b.taps == 1 ? G_TAP : b.taps == 2 ? G_DOUBLE : G_TRIPLE, i);
      b.taps = 0;
    }
  }
}

bool inputGet(uint8_t& g, uint8_t& src) {
  if (rh == rt) return false;
  g = ring[rt].g;
  src = ring[rt].src;
  rt = (rt + 1) % 12;
  return true;
}

void inputInject(uint8_t g, uint8_t src) { push(g, src); }

uint32_t inputHeldMs() { return btn[0].st ? millis() - btn[0].down : 0; }
uint32_t inputLastMs() { return lastAct; }

const char* gestureName(uint8_t g) {
  switch (g) {
    case G_TAP: return "tap";
    case G_DOUBLE: return "double";
    case G_TRIPLE: return "triple";
    case G_LONG: return "long";
    case G_VLONG: return "vlong";
    case G_PRESS: return "press";
    case G_RELEASE: return "release";
  }
  return "none";
}
