#include "cards.h"
#include "power.h"
#include "feeds.h"

#define CARD_PTR(n) &CT_##n,
static const CardType* const TYPES[] = {CARD_LIST(CARD_PTR)};
#define NTYPES (sizeof(TYPES) / sizeof(TYPES[0]))

static CardInst ci[MAX_CARDS];
static int cn = 0, cur = 0;
static uint32_t shownAt = 0, gen = 0;
bool cardHold = false;
uint32_t gFetchNext = 0;

#define RTC_CARD_MAGIC 0xCA9D0002UL
static RTC_NOINIT_ATTR uint32_t rtcCardMagic;
static RTC_NOINIT_ATTR char rtcActiveCardId[32];
static RTC_NOINIT_ATTR char rtcActiveCardType[16];

const CardType* cardTypeFind(const char* id) {
  for (unsigned i = 0; i < NTYPES; i++) if (id && !strcmp(TYPES[i]->id, id)) return TYPES[i];
  return nullptr;
}

void cardTypesJson(String& out) {
  out = "[";
  for (unsigned i = 0; i < NTYPES; i++) { if (i) out += ','; out += '"'; out += TYPES[i]->id; out += '"'; }
  out += "]";
}

static JsonObject cardObj(int i) { return cardsDoc["cards"][ci[i].ai]; }

CardCtx cardCtx(int i) {
  CardCtx c;
  JsonObject o = cardObj(i);
  c.p = o["p"];
  c.d = ci[i].d;
  c.idx = i;
  c.st = ci[i].st;
  c.t0 = shownAt;
  return c;
}

String cardName(int i, const char* def) {
  if (i < 0 || i >= cn) return def;
  const char* n = cardObj(i)["name"] | "";
  return *n ? String(n) : String(def);
}

int cardsCount() { return cn; }
int cardsCurrent() { return cur; }
const char* cardsCurrentType() { return cn ? ci[cur].t->id : "none"; }
const char* cardsTypeAt(int i) { return (i >= 0 && i < cn && ci[i].t) ? ci[i].t->id : ""; }
String cardsCurrentId() { return cn ? String(cardObj(cur)["id"] | "") : String(""); }
bool cardsCurrentShowBat() { return cn ? (cardObj(cur)["p"]["bat"] | false) : false; }

bool cardsHaveType(const char* type) {
  LOCK();
  bool r = false;
  for (int i = 0; i < cn && !r; i++)
    if (!strcmp(ci[i].t->id, type) && (cardObj(i)["en"] | true)) r = true;
  UNLOCK();
  return r;
}

bool cardVisible(int i) {
  if (i < 0 || i >= cn) return false;
  JsonObject o = cardObj(i);
  if (!(o["en"] | true)) return false;
  JsonArray pf = o["pf"];
  if (!pf.isNull() && pf.size()) {
    const char* prof = cfg["profile"] | "Default";
    bool ok = false;
    for (JsonVariant v : pf) if (v == prof) ok = true;
    if (!ok) return false;
  }
  struct tm t;
  if (nowTm(t)) {
    if (!dayAllowed(o["days"] | 127, t.tm_wday)) return false;
    int f = parseHM(o["from"] | ""), to = parseHM(o["to"] | "");
    if (f >= 0 && to >= 0 && !inWindow(t.tm_hour * 60 + t.tm_min, f, to)) return false;
  }
  return true;
}

// ------------------------------------------------------------------ slide transition
static uint8_t oldBuf[1024];
static uint8_t newBuf[1024];
static uint32_t transStart = 0;
static int transDir = 1;
static bool inTrans = false;
static int lastDir = 1;

static void enterCard(int i) {
  if (i < 0 || i >= cn) return;
  if (cn > 1 && i != cur && (cfg["rotate"]["trans"] | true)) {
    memcpy(oldBuf, u8g2.getBufferPtr(), 1024);
    transStart = millis();
    transDir = (lastDir != 0) ? lastDir : 1;
    inTrans = true;
  }
  cur = i;
  shownAt = millis();
  logf("[CARD] Active: #%d [%s] (\"%s\")", i, ci[i].t->id, cardName(i, ci[i].t->id).c_str());

  // Persist active card across reboots / WiFi reconnects
  const char* cid = cardObj(i)["id"] | "";
  if (*cid) {
    rtcCardMagic = RTC_CARD_MAGIC;
    safeCopy(rtcActiveCardId, cid, sizeof(rtcActiveCardId));
    safeCopy(rtcActiveCardType, ci[i].t->id, sizeof(rtcActiveCardType));
    stateDoc["activeCard"] = cid;
    stateDoc["activeCardType"] = ci[i].t->id;
    markDirty(DIRTY_STATE);
  }

  inputSetRaw(ci[i].t->flags & CF_RAW);
  if (ci[i].t->enter) { CardCtx c = cardCtx(i); ci[i].t->enter(c); }
  if (ci[i].t->fetch) {
    if (ci[i].nextFetch == 0 || (int32_t)(ci[i].nextFetch - millis()) > 5000) {
      ci[i].nextFetch = millis();
    }
  }
  if (!strcmp(ci[i].t->id, "cricket") || !strcmp(ci[i].t->id, "cricfix")) {
    cricRefresh();
  } else if (!strcmp(ci[i].t->id, "weather") || !strcmp(ci[i].t->id, "forecast") || !strcmp(ci[i].t->id, "sunmoon")) {
    if (!homeW.ok) feedsRefresh();
  }
}

void cardsRebuild() {
  LOCK();
  // Preserve target card across rebuild:
  // If cn > 0, preserve currently active card.
  // If cn == 0 (bootup), retrieve persisted card from RTC RAM or state.json.
  String savedId = "";
  String savedType = "";
  if (cn > 0) {
    savedId = String(cardObj(cur)["id"] | "");
    savedType = String(ci[cur].t->id);
  } else {
    if (rtcCardMagic == RTC_CARD_MAGIC && rtcActiveCardId[0]) {
      savedId = String(rtcActiveCardId);
      savedType = String(rtcActiveCardType);
    } else if (stateDoc["activeCard"].is<const char*>() && stateDoc["activeCard"].as<const char*>()[0]) {
      savedId = String(stateDoc["activeCard"].as<const char*>());
      if (stateDoc["activeCardType"].is<const char*>()) {
        savedType = String(stateDoc["activeCardType"].as<const char*>());
      }
    }
  }

  for (int i = 0; i < cn; i++) { free(ci[i].d); ci[i].d = nullptr; }
  cn = 0;
  gen++;
  int ai = 0;
  for (JsonObject c : cardsDoc["cards"].as<JsonArray>()) {
    const CardType* t = cardTypeFind(c["type"] | "");
    if (t && cn < MAX_CARDS) {
      if (!c["p"].is<JsonObject>()) c["p"].to<JsonObject>();
      CardInst& x = ci[cn];
      memset(&x, 0, sizeof(x));
      x.t = t;
      x.ai = ai;
      x.d = t->dsize ? calloc(1, t->dsize) : nullptr;
      cn++;
    }
    ai++;
  }

  int target = -1;
  // 1. Try to find by card ID
  if (savedId.length()) {
    for (int i = 0; i < cn; i++) {
      if (savedId == (cardObj(i)["id"] | "") && cardVisible(i)) {
        target = i;
        break;
      }
    }
    // 2. If ID not matched (e.g. cards regenerated), match by savedId as type
    if (target < 0) {
      for (int i = 0; i < cn; i++) {
        if (savedId == ci[i].t->id && cardVisible(i)) {
          target = i;
          break;
        }
      }
    }
  }

  // 3. Match by savedType
  if (target < 0 && savedType.length()) {
    for (int i = 0; i < cn; i++) {
      if (savedType == ci[i].t->id && cardVisible(i)) {
        target = i;
        break;
      }
    }
  }

  // 4. Fallback to emo card if visible
  if (target < 0) {
    for (int i = 0; i < cn; i++) {
      if (!strcmp(ci[i].t->id, "emo") && cardVisible(i)) {
        target = i;
        break;
      }
    }
  }

  // 5. Fallback to first visible card
  if (target < 0) {
    for (int i = 0; i < cn; i++) {
      if (cardVisible(i)) {
        target = i;
        break;
      }
    }
  }

  if (cn && target >= 0) enterCard(target);
  UNLOCK();
  mqttCardsChanged();
}

void cardsBegin() {
  cardsRebuild();
}

void cardsNext(int dir) {
  if (!cn) return;
  for (int k = 1; k <= cn; k++) {
    int j = ((cur + dir * k) % cn + cn) % cn;
    if (cardVisible(j)) { lastDir = dir; enterCard(j); return; }
  }
}

void cardsGoto(int i) { if (i >= 0 && i < cn) { lastDir = (i >= cur) ? 1 : -1; enterCard(i); } }

void cardsHome() {
  lastDir = -1;
  for (int i = 0; i < cn; i++) if (!strcmp(ci[i].t->id, "emo") && cardVisible(i)) { enterCard(i); return; }
  for (int i = 0; i < cn; i++) if (cardVisible(i)) { enterCard(i); return; }
}

void cardsLoop() {
  if (!cn) return;
  uint32_t now = millis();
  LOCK();
  for (int i = 0; i < cn; i++) {
    if (ci[i].t->tick) { CardCtx c = cardCtx(i); ci[i].t->tick(c); }
  }
  static uint32_t lastVis = 0;
  if (now - lastVis > 1000) {
    lastVis = now;
    if (!cardVisible(cur)) {
      logf("[CARD] Active card #%d is no longer visible -> next", cur);
      cardsNext(1);
    }
  }
  bool rot = cfg["rotate"]["auto"] | true;
  if (rot && !cardHold && !(ci[cur].t->flags & CF_NOROT) && now - inputLastMs() > 8000) {
    int dur = cardObj(cur)["dur"] | 0;
    if (dur <= 0) dur = cfg["rotate"]["dur"] | 10;
    if (now - shownAt > dur * 1000UL) {
      logf("[CARD] Auto-rotate timer expired -> next");
      cardsNext(1);
    }
  }
  int home = cfg["rotate"]["home"] | 0;
  if (!rot && home > 0 && now - inputLastMs() > home * 1000UL && now - shownAt > home * 1000UL &&
      strcmp(ci[cur].t->id, "emo") != 0 && !(ci[cur].t->flags & CF_NOROT) && !cardHold) {
    if (netOnline() || !netHasCreds()) {
      logf("[CARD] Idle timer (%ds) expired -> Home", home);
      cardsHome();
    }
  }
  UNLOCK();
}

void cardsDraw() {
  if (!cn || !cardVisible(cur)) {
    drawCenteredMsg("No cards to show", "Add cards in the dashboard");
    return;
  }
  cardHold = false;
  CardCtx c = cardCtx(cur);

  if (inTrans) {
    uint32_t elapsed = millis() - transStart;
    const uint32_t TRANS_MS = 160;
    if (elapsed < TRANS_MS) {
      // 1. Draw new card to u8g2 buffer and capture it
      ci[cur].t->draw(c);
      memcpy(newBuf, u8g2.getBufferPtr(), 1024);

      // 2. Compute smooth ease-out horizontal shift
      float prog = (float)elapsed / (float)TRANS_MS;
      if (prog > 1.0f) prog = 1.0f;
      float ease = 1.0f - (1.0f - prog) * (1.0f - prog);
      int shift = (int)(ease * 128.0f);
      if (shift < 0) shift = 0;
      if (shift > 128) shift = 128;

      // 3. Composite horizontal slide into u8g2 buffer (8 vertical pages of 128 bytes)
      uint8_t* dst = u8g2.getBufferPtr();
      for (int p = 0; p < 8; p++) {
        uint8_t* dPage = dst + p * 128;
        const uint8_t* oPage = oldBuf + p * 128;
        const uint8_t* nPage = newBuf + p * 128;
        if (transDir > 0) {
          // Slide left: old exits left, new enters from right
          for (int x = 0; x < 128 - shift; x++) dPage[x] = oPage[x + shift];
          for (int x = 128 - shift; x < 128; x++) dPage[x] = nPage[x - (128 - shift)];
        } else {
          // Slide right: old exits right, new enters from left
          for (int x = 0; x < shift; x++) dPage[x] = nPage[128 - shift + x];
          for (int x = shift; x < 128; x++) dPage[x] = oPage[x - shift];
        }
      }
      return;
    } else {
      inTrans = false;
    }
  }

  ci[cur].t->draw(c);
}

bool cardsInput(uint8_t g) {
  if (!cn) return false;
  LOCK();
  bool r = false;
  if (ci[cur].t->input) { CardCtx c = cardCtx(cur); r = ci[cur].t->input(c, g); }
  UNLOCK();
  return r;
}

void cardsSpeak() {
  if (!cn) return;
  String s = cardsCurrentText();
  if (!s.length()) s = emoSpeech();
  audioSay(s, EV_UI);
}

String cardsCurrentText() {
  if (!cn) return "";
  String s;
  LOCK();
  if (ci[cur].t->speak) { CardCtx c = cardCtx(cur); ci[cur].t->speak(c, s); }
  UNLOCK();
  return s;
}

void cardRefreshNow(int i) {
  LOCK();
  if (i >= 0 && i < cn) ci[i].nextFetch = millis();
  UNLOCK();
}

void cardsFetchLoop() {
  uint32_t now = millis();
  LOCK();
  if (cur < 0 || cur >= cn) { UNLOCK(); return; }
  int pick = cur;
  if (!ci[pick].t->fetch || !(cardObj(pick)["en"] | true)) { UNLOCK(); return; }
  if ((int32_t)(now - ci[pick].nextFetch) < 0) { UNLOCK(); return; }
  uint32_t g = gen;
  const CardType* t = ci[pick].t;
  JsonDocument pd;
  pd.set(cardObj(pick)["p"]);
  int refreshMin = cardObj(pick)["p"]["refresh"] | 0;
  void* tmp = t->dsize ? malloc(t->dsize) : nullptr;
  if (tmp) memcpy(tmp, ci[pick].d, t->dsize);
  ci[pick].nextFetch = now + 60000;   // provisional, avoids re-picking while fetching
  UNLOCK();

  gFetchNext = 0;
  logf("[FETCH] Card #%d [%s] fetching...", pick, t->id);
  bool ok = t->fetch(pd.as<JsonObject>(), tmp);
  uint32_t next = gFetchNext ? gFetchNext : (refreshMin > 0 ? refreshMin * 60 : t->refresh);
  if (!ok) next = 30;
  else next *= powerScale();   // battery saver: fetch less often
  logf("[FETCH] Card #%d [%s] %s (next in %us)", pick, t->id, ok ? "OK" : "FAILED", (unsigned)next);

  LOCK();
  if (g == gen) {
    if (tmp && ok) memcpy(ci[pick].d, tmp, t->dsize);
    ci[pick].nextFetch = millis() + next * 1000UL;
  }
  UNLOCK();
  free(tmp);
}

// ------------------------------------------------------------------ helpers
bool dataGate(bool ok, const char* what) {
  if (ok) return true;
  if (netPortal() && !netOnline()) drawCenteredMsg("No Wi-Fi", "Setup: 192.168.4.1");
  else if (!netOnline()) drawCenteredMsg("Offline", what);
  else {
    u8g2.setFont(F_MED);
    textCenter(24, "Loading");
    u8g2.setFont(F_SMALL);
    textCenter(37, what);
    int bx = 40, by = 46, bw = 48, bh = 6;
    u8g2.drawRFrame(bx, by, bw, bh, 2);
    int pW = 12;
    int span = bw - 4 - pW;
    int phase = (millis() / 25) % (span * 2);
    int xPos = phase <= span ? phase : (span * 2 - phase);
    u8g2.drawRBox(bx + 2 + xPos, by + 1, pW, bh - 2, 1);
  }
  return false;
}

void drawTitle(CardCtx& c, const char* def, const char* right) {
  String n = cardName(c.idx, def);
  drawHeader(n.c_str(), right);
}

uint32_t getHighScore(const char* game) { return stateDoc["hs"][game] | 0; }

void highScore(const char* game, uint32_t score, uint32_t& best) {
  best = getHighScore(game);
  if (score > best) {
    best = score;
    stateDoc["hs"][game] = score;
    markDirty(DIRTY_STATE);
  }
}

bool locFromParams(JsonObject p, float& lat, float& lon, bool& custom) {
  JsonObject l = p["loc"];
  if (!l.isNull() && !l["lat"].isNull()) {
    lat = l["lat"] | 0.0f;
    lon = l["lon"] | 0.0f;
    custom = true;
    return true;
  }
  custom = false;
  LOCK();
  bool has = !cfg["loc"]["lat"].isNull();
  lat = cfg["loc"]["lat"] | 0.0f;
  lon = cfg["loc"]["lon"] | 0.0f;
  UNLOCK();
  return has;
}
