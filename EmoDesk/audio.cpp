#include "audio.h"
#include "emo.h"
#include "power.h"
#include <ESP_I2S.h>
#if ENABLE_TTS
#include <AudioOutput.h>
#include <ESP8266SAM.h>
#endif

static I2SClass i2s;
static QueueHandle_t aq = nullptr;
static volatile bool abortReq = false, busy = false, speaking = false, contin = false;
static volatile uint32_t metroBeat = 0;
int16_t vizBuf[128];
volatile uint32_t vizStamp = 0;
static uint8_t vizPos = 0;
static volatile int32_t gainQ = 1200;   // Q12 master gain
static volatile int32_t rampQ = 4096;   // Q12 alarm ramp
static bool i2sOk = false, ampState = false;
static uint32_t lastActive = 0;
static int16_t sinTab[256];
static uint32_t ph = 0, rng = 2463534242UL;

enum : uint8_t { A_SFX = 1, A_MEL, A_RTTTL, A_FILE, A_SAY, A_CHIRP, A_NOISE, A_METRO, A_ALARM, A_SPEC };
struct AMsg { uint8_t cmd; int32_t a, b; char* s; };
enum { W_SQ = 0, W_SINE, W_TRI, W_SAW, W_NOISE };
#define DECAY 0x80

// ------------------------------------------------------------------ output
static int16_t obuf[512];
static int on = 0;

static void flushOut() {
  if (on && i2sOk) i2s.write((uint8_t*)obuf, on * 2);
  on = 0;
  vizStamp = millis();
}

static inline void put(int32_t s) {
  int32_t v = (s * gainQ) >> 12;
  v = (v * rampQ) >> 12;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  obuf[on++] = (int16_t)v;
  obuf[on++] = (int16_t)v;
  if ((on & 7) == 0) vizBuf[(vizPos++) & 127] = (int16_t)v;
  if (on >= 512) flushOut();
}

static void silence(uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if ((i & 255) == 0 && abortReq) return;
    put(0);
  }
}

static inline int32_t rnd16() {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return (int16_t)(rng & 0xFFFF);
}

static bool interrupted() { return abortReq; }
static bool newWork() { return abortReq || uxQueueMessagesWaiting(aq) > 0; }

// ------------------------------------------------------------------ synth
static void tone(int f0, int f1, uint32_t ms, uint8_t wave, uint8_t vol) {
  bool decay = wave & DECAY;
  wave &= 0x7F;
  uint32_t n = ms * SAMPLE_RATE / 1000;
  if (!n) return;
  if (f0 <= 0) { silence(n); return; }
  int32_t amp = vol * 110;
  uint32_t att = SAMPLE_RATE / 400, rel = SAMPLE_RATE / 150;
  if (att * 3 > n) att = n / 4;
  if (rel * 3 > n) rel = n / 4;
  if (!att) att = 1;
  if (!rel) rel = 1;
  int64_t inc0 = (int64_t)f0 * 4294967296LL / SAMPLE_RATE;
  int64_t inc1 = (int64_t)f1 * 4294967296LL / SAMPLE_RATE;
  int64_t inc = inc0, dInc = (inc1 - inc0) / (int64_t)n;
  for (uint32_t i = 0; i < n; i++) {
    if ((i & 255) == 0 && interrupted()) return;
    ph += (uint32_t)inc;
    inc += dInc;
    int32_t s;
    switch (wave) {
      case W_SQ: s = (ph & 0x80000000UL) ? 26000 : -26000; break;
      case W_SINE: s = sinTab[ph >> 24]; break;
      case W_TRI: { int32_t p = ph >> 16; s = p < 32768 ? p * 2 - 32768 : 32767 - (p - 32768) * 2; break; }
      case W_SAW: s = (int32_t)(ph >> 16) - 32768; break;
      default: s = rnd16();
    }
    int32_t env = amp;
    if (i < att) env = amp * (int32_t)i / (int32_t)att;
    else if (i > n - rel) env = amp * (int32_t)(n - i) / (int32_t)rel;
    if (decay) { int32_t k = (int32_t)((uint64_t)(n - i) * 1024 / n); env = env * k / 1024 * k / 1024; }
    put((s * env) >> 15);
  }
}

struct Seg { uint16_t f0, f1, ms; uint8_t wave, vol; };
#define SFX(name, ...) static const Seg SX_##name[] = {__VA_ARGS__};
SFX(click, {2200, 2200, 6, W_SQ, 80})
SFX(tap, {900, 1600, 30, W_SINE, 200})
SFX(pop, {300, 1200, 35, W_SINE, 230}, {0, 0, 10, 0, 0})
SFX(coin, {988, 988, 60, W_SQ, 140}, {1319, 1319, 220, W_SQ | DECAY, 140})
SFX(jump, {280, 880, 110, W_SQ, 130})
SFX(hit, {300, 60, 160, W_SAW, 200})
SFX(gameover, {660, 660, 140, W_SQ, 140}, {0, 0, 30, 0, 0}, {520, 520, 140, W_SQ, 140}, {0, 0, 30, 0, 0},
    {390, 390, 140, W_SQ, 140}, {330, 110, 500, W_SQ | DECAY, 150})
SFX(win, {523, 523, 90, W_SQ, 130}, {659, 659, 90, W_SQ, 130}, {784, 784, 90, W_SQ, 130}, {1047, 1047, 320, W_SQ | DECAY, 140})
SFX(levelup, {392, 392, 70, W_TRI, 200}, {523, 523, 70, W_TRI, 200}, {659, 659, 70, W_TRI, 200}, {784, 784, 70, W_TRI, 200},
    {1047, 1047, 250, W_TRI | DECAY, 200})
SFX(beep, {1000, 1000, 120, W_SINE, 200})
SFX(bell, {1318, 1318, 900, W_SINE | DECAY, 230})
SFX(chime, {1568, 1568, 450, W_SINE | DECAY, 220}, {1175, 1175, 800, W_SINE | DECAY, 220})
SFX(cuckoo, {784, 784, 260, W_SINE, 220}, {0, 0, 60, 0, 0}, {622, 622, 420, W_SINE | DECAY, 220})
SFX(alarm, {2000, 2000, 90, W_SQ, 150}, {0, 0, 60, 0, 0}, {2000, 2000, 90, W_SQ, 150}, {0, 0, 60, 0, 0},
    {2000, 2000, 90, W_SQ, 150}, {0, 0, 60, 0, 0}, {2000, 2000, 90, W_SQ, 150}, {0, 0, 400, 0, 0})
SFX(goal, {233, 233, 900, W_SAW, 230}, {0, 0, 80, 0, 0}, {233, 233, 500, W_SAW | DECAY, 230})
SFX(wicket, {392, 392, 180, W_SQ, 150}, {0, 0, 40, 0, 0}, {294, 294, 180, W_SQ, 150}, {0, 0, 40, 0, 0},
    {196, 150, 450, W_SQ | DECAY, 160})
SFX(notify, {880, 880, 80, W_SINE, 220}, {1320, 1320, 160, W_SINE | DECAY, 220})
SFX(msg, {1047, 1047, 70, W_SINE, 220}, {1319, 1319, 70, W_SINE, 220}, {1568, 1568, 180, W_SINE | DECAY, 220})
SFX(startup, {523, 523, 80, W_TRI, 200}, {784, 784, 80, W_TRI, 200}, {1047, 1047, 80, W_TRI, 200}, {1568, 1568, 300, W_TRI | DECAY, 200})
SFX(error, {220, 180, 350, W_SQ, 150})
SFX(purr, {60, 60, 90, W_NOISE, 45}, {0, 0, 40, 0, 0}, {60, 60, 90, W_NOISE, 50}, {0, 0, 40, 0, 0}, {60, 60, 90, W_NOISE, 50},
    {0, 0, 40, 0, 0}, {60, 60, 90, W_NOISE, 45}, {0, 0, 40, 0, 0}, {60, 60, 90, W_NOISE, 40}, {0, 0, 40, 0, 0})
SFX(snore, {120, 90, 800, W_NOISE, 30}, {0, 0, 300, 0, 0}, {90, 140, 600, W_NOISE | DECAY, 25})
SFX(tick, {3000, 3000, 4, W_SQ, 90})
SFX(yum, {600, 900, 100, W_SINE, 220}, {900, 700, 120, W_SINE, 220}, {0, 0, 40, 0, 0}, {700, 1000, 160, W_SINE | DECAY, 220})
SFX(whoosh, {800, 200, 350, W_NOISE | DECAY, 90})
SFX(cheer, {1000, 1000, 1400, W_NOISE | DECAY, 110})
SFX(dizzy, {900, 500, 150, W_SINE, 200}, {500, 900, 150, W_SINE, 200}, {900, 500, 150, W_SINE, 200}, {500, 300, 300, W_SINE | DECAY, 200})
SFX(laugh, {700, 500, 90, W_SQ, 120}, {0, 0, 50, 0, 0}, {700, 500, 90, W_SQ, 120}, {0, 0, 50, 0, 0}, {700, 450, 140, W_SQ | DECAY, 120})
SFX(drop, {1200, 500, 90, W_SINE, 220}, {500, 900, 60, W_SINE | DECAY, 200})

struct SfxDef { const char* name; const Seg* s; uint8_t n; };
#define SD_(x) {#x, SX_##x, sizeof(SX_##x) / sizeof(Seg)}
static const SfxDef SFXS[] = {SD_(click), SD_(tap), SD_(pop), SD_(coin), SD_(jump), SD_(hit), SD_(gameover), SD_(win),
                              SD_(levelup), SD_(beep), SD_(bell), SD_(chime), SD_(cuckoo), SD_(alarm), SD_(goal),
                              SD_(wicket), SD_(notify), SD_(msg), SD_(startup), SD_(error), SD_(purr), SD_(snore),
                              SD_(tick), SD_(yum), SD_(whoosh), SD_(cheer), SD_(dizzy), SD_(laugh), SD_(drop)};
#define NSFX (sizeof(SFXS) / sizeof(SFXS[0]))

int sfxCount() { return NSFX; }
const char* sfxName(int i) { return (i >= 0 && i < (int)NSFX) ? SFXS[i].name : ""; }

static void playSfx(const char* name) {
  for (unsigned i = 0; i < NSFX; i++) {
    if (strcmp(name, SFXS[i].name) == 0) {
      for (int k = 0; k < SFXS[i].n && !interrupted(); k++) {
        const Seg& s = SFXS[i].s[k];
        tone(s.f0, s.f1, s.ms, s.wave, s.vol);
      }
      return;
    }
  }
}

// ------------------------------------------------------------------ melodies (public domain only)
struct Mel { const char* name; const char* r; };
static const Mel MELS[] = {
  {"Ode to Joy", "ode:d=4,o=5,b=120:e,e,f,g,g,f,e,d,c,c,d,e,e.,8d,2d,e,e,f,g,g,f,e,d,c,c,d,e,d.,8c,2c"},
  {"Twinkle Twinkle", "twinkle:d=4,o=5,b=120:c,c,g,g,a,a,2g,f,f,e,e,d,d,2c,g,g,f,f,e,e,2d,g,g,f,f,e,e,2d,c,c,g,g,a,a,2g,f,f,e,e,d,d,2c"},
  {"Happy Birthday", "hbd:d=4,o=5,b=120:8g.,16g,a,g,c6,2b,8g.,16g,a,g,d6,2c6,8g.,16g,g6,e6,c6,b,a,8f6.,16f6,e6,c6,d6,2c6"},
  {"Fur Elise", "elise:d=8,o=5,b=125:e6,d#6,e6,d#6,e6,b,d6,c6,4a,p,c,e,a,4b,p,e,g#,b,4c6,p,e,e6,d#6,e6,d#6,e6,b,d6,c6,4a,p,c,e,a,4b,p,e,c6,b,2a"},
  {"Jingle Bells", "jingle:d=4,o=5,b=170:e,e,2e,e,e,2e,e,g,c.,8d,1e,f,f,f.,8f,f,e,e,8e,8e,e,d,d,e,2d,2g,e,e,2e,e,e,2e,e,g,c.,8d,1e,f,f,f.,8f,f,e,e,8e,8e,g,g,f,d,1c"},
  {"Westminster", "west:d=4,o=5,b=90:g#,f#,e,2b4,e,g#,f#,2e,g#,e,f#,2b4,b4,f#,g#,2e"},
  {"Korobeiniki", "korob:d=4,o=5,b=140:e,8b4,8c,d,8c,8b4,a4,8a4,8c,e,8d,8c,b4.,8c,d,e,c,a4,2a4,p,8d,f,a,8g,8f,e.,8c,e,8d,8c,b4,8b4,8c,d,e,c,a4,2a4"},
};
#define NMEL (sizeof(MELS) / sizeof(MELS[0]))

int melodyCount() {
  JsonArray a = cfg["audio"]["melodies"];
  return NMEL + (a.isNull() ? 0 : a.size());
}
const char* melodyName(int i) {
  if (i < (int)NMEL) return i >= 0 ? MELS[i].name : "";
  return cfg["audio"]["melodies"][i - NMEL]["n"] | "Custom";
}
int melodyFind(const char* name) {
  int n = melodyCount();
  for (int i = 0; i < n; i++) if (icontains(melodyName(i), name)) return i;
  return -1;
}

static uint8_t themeWave() {
  const char* t = cfg["audio"]["theme"] | "cute";
  if (!strcmp(t, "robot")) return W_SQ;
  if (!strcmp(t, "retro")) return W_TRI;
  return W_SINE;
}
static volatile uint8_t curWave = W_SINE;

static void playRtttl(const char* p) {
  while (*p && *p != ':') p++;
  if (!*p) return;
  p++;
  int d = 4, o = 6, b = 63;
  while (*p && *p != ':') {
    while (*p == ' ' || *p == ',') p++;
    char k = *p;
    if (p[1] == '=') {
      int v = atoi(p + 2);
      if (k == 'd' && v) d = v;
      else if (k == 'o' && v) o = v;
      else if (k == 'b' && v) b = v;
    }
    while (*p && *p != ',' && *p != ':') p++;
  }
  if (*p == ':') p++;
  long whole = (60L * 1000L / b) * 4;
  while (*p && !interrupted()) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    int num = 0;
    while (isdigit((unsigned char)*p)) num = num * 10 + (*p++ - '0');
    long dur = num ? whole / num : whole / d;
    int note = 0;
    switch (tolower(*p)) {
      case 'c': note = 1; break; case 'd': note = 3; break; case 'e': note = 5; break;
      case 'f': note = 6; break; case 'g': note = 8; break; case 'a': note = 10; break;
      case 'b': case 'h': note = 12; break; default: note = 0;
    }
    if (*p) p++;
    if (*p == '#') { note++; p++; }
    if (*p == '.') { dur += dur / 2; p++; }
    int scale = o;
    if (isdigit((unsigned char)*p)) scale = *p++ - '0';
    if (*p == '.') { dur += dur / 2; p++; }
    while (*p && *p != ',') p++;
    if (note) {
      int f = (int)(440.0f * powf(2.0f, ((scale - 4) * 12 + (note - 10)) / 12.0f));
      tone(f, f, dur * 9 / 10, curWave, 190);
      tone(0, 0, dur / 10, 0, 0);
    } else tone(0, 0, dur, 0, 0);
  }
}

// ------------------------------------------------------------------ WAV
static void playWav(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) { logf("audio: no file %s", path); return; }
  char id[4];
  uint32_t sz = 0;
  if (f.read((uint8_t*)id, 4) != 4 || memcmp(id, "RIFF", 4)) { f.close(); return; }
  f.read((uint8_t*)&sz, 4);
  f.read((uint8_t*)id, 4);
  uint16_t fmt = 1, ch = 1, bits = 16;
  uint32_t rate = SAMPLE_RATE, dataLen = 0;
  while (f.available()) {
    if (f.read((uint8_t*)id, 4) != 4) break;
    f.read((uint8_t*)&sz, 4);
    if (!memcmp(id, "fmt ", 4)) {
      uint32_t br; uint16_t ba;
      f.read((uint8_t*)&fmt, 2); f.read((uint8_t*)&ch, 2); f.read((uint8_t*)&rate, 4);
      f.read((uint8_t*)&br, 4); f.read((uint8_t*)&ba, 2); f.read((uint8_t*)&bits, 2);
      if (sz > 16) f.seek(f.position() + sz - 16);
    } else if (!memcmp(id, "data", 4)) { dataLen = sz; break; }
    else f.seek(f.position() + sz + (sz & 1));
  }
  if (!dataLen || fmt != 1 || (bits != 8 && bits != 16) || !rate) { f.close(); logf("audio: bad wav"); return; }
  uint8_t bps = (bits / 8) * ch;
  uint32_t total = dataLen / bps, idx = 0;
  uint32_t step = (uint32_t)((uint64_t)rate * 65536 / SAMPLE_RATE), frac = 0;
  uint8_t buf[512];
  int bl = 0, bp = 0;
  auto next = [&](int32_t& out) -> bool {
    if (idx >= total) return false;
    if (bp + bps > bl) {
      bl = f.read(buf, (sizeof(buf) / bps) * bps);
      bp = 0;
      if (bl < bps) return false;
    }
    out = bits == 8 ? ((int32_t)buf[bp] - 128) << 8 : (int16_t)(buf[bp] | (buf[bp + 1] << 8));
    bp += bps;
    idx++;
    return true;
  };
  int32_t cur = 0;
  if (!next(cur)) { f.close(); return; }
  bool more = true;
  uint32_t n = 0;
  while (more) {
    if ((n++ & 255) == 0 && interrupted()) break;
    put(cur * 3 / 4);
    frac += step;
    while (frac >= 65536) { frac -= 65536; if (!next(cur)) { more = false; break; } }
  }
  f.close();
}

// ------------------------------------------------------------------ TTS
#if ENABLE_TTS
class SamOut : public AudioOutput {
 public:
  bool begin() override { return true; }
  bool ConsumeSample(int16_t sample[2]) override { put(sample[0]); return true; }
  bool stop() override { return true; }
};
#endif

static void say(const char* text) {
#if ENABLE_TTS
  SamOut out;
  ESP8266SAM sam;
  LOCK();
  String vs = cfg["audio"]["voice"] | "sam";
  int sp = cfg["audio"]["speed"] | 0, pi = cfg["audio"]["pitch"] | 0;
  UNLOCK();
  const char* v = vs.c_str();
  if (!strcmp(v, "elf")) sam.SetVoice(ESP8266SAM::VOICE_ELF);
  else if (!strcmp(v, "robot")) sam.SetVoice(ESP8266SAM::VOICE_ROBOT);
  else if (!strcmp(v, "stuffy")) sam.SetVoice(ESP8266SAM::VOICE_STUFFY);
  else if (!strcmp(v, "oldlady")) sam.SetVoice(ESP8266SAM::VOICE_OLDLADY);
  else if (!strcmp(v, "et")) sam.SetVoice(ESP8266SAM::VOICE_ET);
  else sam.SetVoice(ESP8266SAM::VOICE_SAM);
  sam.SetSpeed(sp > 0 ? sp : 95);
  sam.SetPitch(pi > 0 ? pi : 64);
  speaking = true;
  char chunk[124];
  const char* p = text;
  while (*p && !interrupted()) {
    while (*p == ' ') p++;
    int len = strlen(p), take = len;
    if (len > 120) {
      take = 120;
      for (int i = 120; i > 40; i--) if (p[i] == ' ' || p[i] == '.' || p[i] == ',') { take = i; break; }
    }
    int k = 0;
    for (int i = 0; i < take && k < 122; i++) chunk[k++] = toupper((unsigned char)p[i]);
    chunk[k] = 0;
    if (k) sam.Say(&out, chunk);
    p += take;
    tone(0, 0, 60, 0, 0);
  }
  speaking = false;
#else
  // No TTS library: robot babble with one blip per word
  speaking = true;
  for (const char* p = text; *p && !interrupted(); p++) {
    if (*p == ' ' || p == text) {
      int f = 500 + esp_random() % 500;
      tone(f, f + 150 - (int)(esp_random() % 300), 70, curWave, 160);
      tone(0, 0, 30, 0, 0);
    }
  }
  speaking = false;
#endif
}

// ------------------------------------------------------------------ chirps
static void blip(int f0, int f1, int ms, uint8_t w, uint8_t v) { tone(f0, f1, ms, w, v); tone(0, 0, 25, 0, 0); }

static void chirp(int e) {
  uint8_t w = curWave;
  int base = w == W_SINE ? 1400 : (w == W_SQ ? 600 : 900);
  uint8_t v = w == W_SQ ? 110 : 200;
  uint32_t r = esp_random();
  switch (e) {
    case E_HAPPY: for (int i = 0; i < 3; i++) blip(base + i * 180, base + i * 180 + 250, 60, w, v); break;
    case E_EXCITED: for (int i = 0; i < 6; i++) blip(base + ((r >> (i * 3)) & 7) * 90, base + 400, 40, w, v); break;
    case E_SAD: case E_CRY: blip(base, base * 6 / 10, 260, w, v); blip(base * 7 / 10, base * 45 / 100, 380, w, v); break;
    case E_ANGRY: for (int i = 0; i < 5; i++) blip(base * 35 / 100, base * 30 / 100, 45, W_SQ, 120); break;
    case E_SURPRISED: blip(base * 6 / 10, base * 16 / 10, 140, w, v); break;
    case E_LOVE: blip(base, base * 12 / 10, 120, w, v); blip(base * 12 / 10, base * 15 / 10, 220, w, v); break;
    case E_SLEEPY: case E_SLEEP: case E_BORED: blip(base * 8 / 10, base * 4 / 10, 500, w, v / 2); break;
    case E_CONFUSED: blip(base * 8 / 10, base * 13 / 10, 120, w, v); blip(base * 13 / 10, base * 9 / 10, 120, w, v);
      blip(base * 9 / 10, base * 14 / 10, 160, w, v); break;
    case E_DIZZY: for (int i = 0; i < 4; i++) blip(i % 2 ? base * 12 / 10 : base * 8 / 10, i % 2 ? base * 8 / 10 : base * 12 / 10, 90, w, v); break;
    default: {
      int n = 2 + r % 4;
      for (int i = 0; i < n; i++) {
        int f = base * 7 / 10 + (int)((r >> (i * 4)) & 15) * base / 20;
        blip(f, f + (((r >> (i * 2)) & 1) ? 200 : -150), 50 + ((r >> i) & 3) * 20, w, v);
      }
    }
  }
}

// ------------------------------------------------------------------ continuous
static void noiseGen(int type, uint32_t ms) {
  contin = true;
  uint32_t total = ms ? (uint32_t)((uint64_t)ms * SAMPLE_RATE / 1000) : 0xFFFFFFF0UL;
  uint32_t fade = SAMPLE_RATE * 8;
  int32_t b0 = 0, b1 = 0, b2 = 0, brown = 0, dropEnv = 0;
  uint32_t dropPh = 0, dropInc = 0, i = 0;
  while (i < total && !newWork()) {
    for (int k = 0; k < 256 && i < total; k++, i++) {
      int32_t w = rnd16(), s;
      b0 += (w - b0) >> 1; b1 += (w - b1) >> 3; b2 += (w - b2) >> 5;
      int32_t pink = (b0 + b1 * 2 + b2 * 3) / 4;
      brown += w >> 5; brown -= brown >> 7;
      if (brown > 32000) brown = 32000;
      if (brown < -32000) brown = -32000;
      switch (type) {
        case 0: s = w / 3; break;
        case 1: s = pink; break;
        case 2: s = brown; break;
        case 3:
          s = pink / 2;
          if (dropEnv < 40 && (rng & 2047) == 7) { dropEnv = 14000; dropInc = (1500 + (rng % 2500)) * (4294967296ULL / SAMPLE_RATE); }
          if (dropEnv >= 40) { dropPh += dropInc; s += (sinTab[dropPh >> 24] * dropEnv) >> 15; dropEnv -= dropEnv >> 7; }
          break;
        default: {
          int32_t lfo = sinTab[(i / (SAMPLE_RATE * 9 / 256)) & 255];   // ~9 s swell
          s = (brown * (19000 + lfo / 3)) >> 15;
        }
      }
      int32_t g = 4096;
      if (ms && total - i < fade) g = (int32_t)((uint64_t)(total - i) * 4096 / fade);
      if (i < (uint32_t)SAMPLE_RATE) g = g * (int32_t)i / SAMPLE_RATE;
      put(((s * g) >> 12) / 2);
    }
  }
  contin = false;
}

static void metronome(int bpm) {
  if (bpm < 20) return;
  contin = true;
  uint32_t period = (uint32_t)SAMPLE_RATE * 60 / bpm, beat = 0, click = SAMPLE_RATE * 15 / 1000;
  while (!newWork()) {
    metroBeat = millis();
    int f = beat % 4 == 0 ? 1800 : 1200;
    tone(f, f, 15, W_SQ, beat % 4 == 0 ? 170 : 120);
    for (uint32_t i = click; i < period; i++) {
      if ((i & 255) == 0 && newWork()) { contin = false; return; }
      put(0);
    }
    beat++;
  }
  contin = false;
}

static void playSpecTask(const char* spec) {
  if (!strncmp(spec, "sfx:", 4)) playSfx(spec + 4);
  else if (!strncmp(spec, "mel:", 4)) { int i = atoi(spec + 4); if (i >= 0 && i < (int)NMEL) playRtttl(MELS[i].r); }
  else if (!strncmp(spec, "file:", 5)) playWav(spec + 5);
  else if (!strncmp(spec, "rtttl:", 6)) playRtttl(spec + 6);
  else if (!strncmp(spec, "say:", 4)) say(spec + 4);
  else playSfx(spec);
}

static void alarmLoop(const char* spec, bool ramp) {
  contin = true;
  uint32_t start = millis();
  while (!interrupted() && millis() - start < 300000UL) {
    if (ramp) {
      uint32_t el = millis() - start;
      rampQ = el > 30000 ? 4096 : 1100 + (int32_t)(el * 2996 / 30000);
    }
    playSpecTask(spec);
    silence(SAMPLE_RATE / 3);
  }
  rampQ = 4096;
  contin = false;
}

// ------------------------------------------------------------------ task
static void amp(bool onOff) {
  if (onOff == ampState) return;
  ampState = onOff;
  digitalWrite(PIN_AMP_SD, onOff ? HIGH : LOW);
  if (onOff) delay(8);
}

static void audioTask(void*) {
  AMsg m;
  for (;;) {
    if (xQueueReceive(aq, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
      abortReq = false;
      busy = true;
      if (!i2sOk) {
        i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
        i2sOk = i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        if (!i2sOk) logf("I2S init failed");
      }
      if (i2sOk) {
        amp(true);
        LOCK();
        curWave = themeWave();
        UNLOCK();
        switch (m.cmd) {
          case A_SFX: if (m.s) playSfx(m.s); break;
          case A_MEL: if (m.a >= 0 && m.a < (int)NMEL) playRtttl(MELS[m.a].r); break;
          case A_RTTTL: if (m.s) playRtttl(m.s); break;
          case A_FILE: if (m.s) playWav(m.s); break;
          case A_SAY: if (m.s) say(m.s); break;
          case A_CHIRP: chirp(m.a); break;
          case A_NOISE: noiseGen(m.a, m.b); break;
          case A_METRO: metronome(m.a); break;
          case A_ALARM: if (m.s) alarmLoop(m.s, m.a); break;
          case A_SPEC: if (m.s) playSpecTask(m.s); break;
        }
        silence(SAMPLE_RATE / 20);
        flushOut();
      }
      if (m.s) free(m.s);
      lastActive = millis();
      busy = uxQueueMessagesWaiting(aq) > 0;
    } else {
      gAudioStackFree = uxTaskGetStackHighWaterMark(NULL);
      if (!ampState || millis() - lastActive <= 2000) continue;
      silence(1024);
      flushOut();
      amp(false);
      memset(vizBuf, 0, sizeof(vizBuf));
    }
  }
}

#if ENABLE_TTS
#define AUDIO_STACK 10240   // the SAM voice needs room
#else
#define AUDIO_STACK 6144
#endif
static TaskHandle_t audioTaskH = nullptr;

// The audio task (and its stack) only exists once a speaker is switched on.
static void audioEnsureTask() {
  if (audioTaskH || !aq || gSafeMode || !audioInstalled()) return;
  if (xTaskCreate(audioTask, "audio", AUDIO_STACK, nullptr, 3, &audioTaskH) != pdPASS) audioTaskH = nullptr;
}

void audioBegin() {
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);
  for (int i = 0; i < 256; i++) sinTab[i] = (int16_t)(sinf(i * 2 * PI / 256) * 30000);
  aq = xQueueCreate(8, sizeof(AMsg));
  audioApply();
}

void audioApply() {
  int v = cfg["audio"]["vol"] | 40;
  v = constrain(v, 0, 100);
  gainQ = (int32_t)v * v * 4096 / 10000 * 8 / 10;
  audioEnsureTask();
}

bool audioInstalled() { return cfg["audio"]["installed"] | false; }
bool audioMuted() { return cfg["audio"]["muted"] | false; }
void audioSetMuted(bool m) { LOCK(); cfg["audio"]["muted"] = m; UNLOCK(); markDirty(DIRTY_CFG); if (m) audioStop(); }
int audioVolume() { return cfg["audio"]["vol"] | 40; }
void audioSetVolume(int v) {
  v = constrain(v, 0, 100);
  LOCK(); cfg["audio"]["vol"] = v; UNLOCK();
  audioApply();
  markDirty(DIRTY_CFG);
}

bool audioAllowed(uint8_t ev) {
  if (!audioInstalled() || !aq || !audioTaskH) return false;
  if (ev == EV_ALARM) return true;
  if (powerQuiet()) return false;   // battery saver "quiet" option
  if (audioMuted()) return false;
  if (ev == EV_UI) return true;
  if (ev < EV_COUNT && !(cfg["audio"]["ev"][EV_KEYS[ev]] | true)) return false;
  if ((cfg["audio"]["quiet"] | false) &&
      inWindow(nowMin(), parseHM(cfg["audio"]["qFrom"] | "22:00"), parseHM(cfg["audio"]["qTo"] | "07:00")))
    return false;
  return true;
}

static void send(uint8_t cmd, int32_t a, int32_t b, const char* s) {
  AMsg m = {cmd, a, b, nullptr};
  if (s) m.s = strdup(s);
  if (xQueueSend(aq, &m, 0) != pdTRUE && m.s) free(m.s);
}

void audioSfx(const char* name, uint8_t ev) {
  if (audioAllowed(ev)) {
    logf("[AUDIO] SFX: %s (ev: %s)", name, EV_KEYS[ev]);
    send(A_SFX, 0, 0, name);
  }
}
void audioRtttl(const char* r, uint8_t ev) { if (audioAllowed(ev)) send(A_RTTTL, 0, 0, r); }
void audioFile(const char* path, uint8_t ev) { if (audioAllowed(ev)) send(A_FILE, 0, 0, path); }
void audioChirp(int e, uint8_t ev) {
  if (!emoActive()) return;
  if (ev == EV_EMO && !(cfg["emo"]["chirps"] | true)) return;
  if (audioAllowed(ev) && !busy) send(A_CHIRP, e, 0, nullptr);
}
void audioNoise(int type, uint32_t ms) { if (audioAllowed(EV_UI)) send(A_NOISE, type, ms, nullptr); }
void audioMetronome(int bpm) {
  if (bpm <= 0) { if (contin) audioStop(); return; }
  if (audioAllowed(EV_UI)) send(A_METRO, bpm, 0, nullptr);
}

void audioMelody(int idx, uint8_t ev) {
  if (!audioAllowed(ev) || idx < 0) return;
  logf("[AUDIO] Melody: #%d (ev: %s)", idx, EV_KEYS[ev]);
  if (idx < (int)NMEL) { send(A_MEL, idx, 0, nullptr); return; }
  LOCK();
  String r = cfg["audio"]["melodies"][idx - NMEL]["r"] | "";
  UNLOCK();
  if (r.length()) send(A_RTTTL, 0, 0, r.c_str());
}

void audioSay(const String& text, uint8_t ev) {
  if (!audioAllowed(ev) || !text.length()) return;
  logf("[AUDIO] Say: \"%.35s\"", text.c_str());
  String t = ttsClean(text);
  if (t.length() > 600) t = t.substring(0, 600);
  send(A_SAY, 0, 0, t.c_str());
}

static String resolveSpec(const char* spec) {
  if (!strncmp(spec, "mel:", 4)) {
    int i = atoi(spec + 4);
    if (i >= (int)NMEL) {
      LOCK();
      String r = cfg["audio"]["melodies"][i - NMEL]["r"] | "";
      UNLOCK();
      return "rtttl:" + r;
    }
  }
  if (!strncmp(spec, "say:", 4)) return "say:" + ttsClean(String(spec + 4));
  return String(spec);
}

void audioPlaySpec(const char* spec, uint8_t ev) {
  if (!spec || !*spec || !audioAllowed(ev)) return;
  String s = resolveSpec(spec);
  send(A_SPEC, 0, 0, s.c_str());
}

void audioAlarm(const char* spec) {
  if (!audioAllowed(EV_ALARM)) return;
  audioStop();
  String s = resolveSpec(spec && *spec ? spec : "sfx:alarm");
  send(A_ALARM, cfg["audio"]["ramp"] | true, 0, s.c_str());
}

void audioStop() {
  if (!aq) return;
  AMsg m;
  while (xQueueReceive(aq, &m, 0) == pdTRUE) if (m.s) free(m.s);
  abortReq = true;
}

bool audioBusy() { return busy; }
bool audioSpeaking() { return speaking; }
bool audioContinuous() { return contin; }
uint32_t audioMetroBeatMs() { return metroBeat; }
