#include "power.h"
#include "cards.h"

// Replace common UTF-8 punctuation / HTML entities with ASCII the OLED fonts can show
static void cleanText(char* s) {
  char* o = s;
  for (char* p = s; *p;) {
    uint8_t c = *p;
    if (c == 0xE2 && (uint8_t)p[1] == 0x80) {
      uint8_t k = p[2];
      if (k == 0x98 || k == 0x99) *o++ = '\'';
      else if (k == 0x9C || k == 0x9D) *o++ = '"';
      else if (k == 0x93 || k == 0x94) *o++ = '-';
      else if (k == 0xA6) { *o++ = '.'; *o++ = '.'; *o++ = '.'; }
      else *o++ = ' ';
      p += 3;
      continue;
    }
    if (c == '&') {
      static const char* const ent[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
                                           {"&apos;", "'"}, {"&#8217;", "'"}, {"&#8216;", "'"}, {"&#8220;", "\""},
                                           {"&#8221;", "\""}, {"&#039;", "'"}, {"&nbsp;", " "}};
      bool done = false;
      for (auto& e : ent) {
        size_t l = strlen(e[0]);
        if (!strncmp(p, e[0], l)) { *o++ = e[1][0]; p += l; done = true; break; }
      }
      if (done) continue;
    }
    if (c == '\n' || c == '\r' || c == '\t') { *o++ = ' '; p++; continue; }
    *o++ = *p++;
  }
  *o = 0;
}

static void textBlock(CardCtx& c, const char* txt, const char* foot) {
  u8g2.setFont(F_SMALL);
  int lines = wrapText(0, 0, 124, 8, txt, 0);
  int avail = foot ? 6 : 7;
  int skip = 0;
  if (lines > avail) {
    int steps = lines - avail + 1;
    skip = ((millis() - c.t0) / 2200) % (steps + 2);
    skip = skip >= steps ? steps - 1 : skip;
    cardHold = true;
    if ((millis() - c.t0) > (uint32_t)(steps + 3) * 2200) cardHold = false;
  }
  int y0 = lines < avail ? 8 + (avail - lines) * 4 : 8;
  wrapText(2, y0, 124, 8, txt, avail, skip);
  if (foot) { u8g2.setFont(F_TINY); textRight(127, 63, foot); }
}

static void parseAlerts(const char* alerts, const char* id, float price, uint8_t& fired, const char* cur) {
  if (!alerts || !*alerts) return;
  char buf[160];
  safeCopy(buf, alerts, sizeof(buf));
  int idx = 0;
  for (char* tok = strtok(buf, ";, "); tok && idx < 8; tok = strtok(nullptr, ";, "), idx++) {
    char* op = strpbrk(tok, "<>");
    if (!op) continue;
    char opc = *op;
    *op = 0;
    if (strcasecmp(tok, id)) continue;
    float lim = atof(op + 1);
    bool hit = opc == '>' ? price > lim : price < lim;
    uint8_t bit = 1 << idx;
    if (hit && !(fired & bit)) {
      fired |= bit;
      char t[80];
      snprintf(t, sizeof(t), "%s %s %s (%s %s)", id, fmtPrice(price).c_str(), cur, opc == '>' ? "above" : "below", op + 1);
      notify("Price alert", t, opc == '>' ? E_EXCITED : E_SAD, "sfx:coin", EV_NOTIFY, false, 12);
    } else if (!hit) fired &= ~bit;
  }
}

// ================================================================== crypto (CoinGecko)
struct Coin { char id[20]; float price, ch; float hist[24]; uint8_t hn, fired; };
struct CryptoData { bool ok; uint8_t n; char cur[6]; Coin c[4]; };

static bool crypto_fetch(JsonObject p, void* d) {
  CryptoData* cd = (CryptoData*)d;
  String ids = p["coins"] | "bitcoin,ethereum";
  String cur = p["cur"] | "usd";
  ids.replace(" ", "");
  cur.toLowerCase();
  JsonDocument doc;
  if (!httpJson("https://api.coingecko.com/api/v3/simple/price?ids=" + ids + "&vs_currencies=" + cur +
                "&include_24hr_change=true", doc)) return false;
  CryptoData nd;
  memset(&nd, 0, sizeof(nd));
  safeCopy(nd.cur, cur.c_str(), sizeof(nd.cur));
  int start = 0;
  while (start < (int)ids.length() && nd.n < 4) {
    int e = ids.indexOf(',', start);
    if (e < 0) e = ids.length();
    String id = ids.substring(start, e);
    start = e + 1;
    if (!id.length() || doc[id].isNull()) continue;
    Coin& k = nd.c[nd.n++];
    safeCopy(k.id, id.c_str(), sizeof(k.id));
    k.price = doc[id][cur] | 0.0f;
    k.ch = doc[id][cur + "_24h_change"] | 0.0f;
    for (int j = 0; j < cd->n; j++) {
      if (strcmp(cd->c[j].id, k.id)) continue;
      memcpy(k.hist, cd->c[j].hist, sizeof(k.hist));
      k.hn = cd->c[j].hn;
      k.fired = cd->c[j].fired;
    }
    if (k.hn == 24) { memmove(k.hist, k.hist + 1, 23 * sizeof(float)); k.hn = 23; }
    k.hist[k.hn++] = k.price;
    parseAlerts(p["alerts"] | "", k.id, k.price, k.fired, nd.cur);
  }
  nd.ok = nd.n > 0;
  *cd = nd;
  return nd.ok;
}

static void priceScreen(const char* name, const char* cur, float price, float chPct, const float* hist, int hn, int idx, int n) {
  char b[32];
  u8g2.setFont(F_BOLD);
  String nm(name);
  nm.toUpperCase();
  u8g2.drawStr(1, 10, nm.substring(0, 10).c_str());

  snprintf(b, sizeof(b), "%+.1f%%", chPct);
  drawBadge(127, 8, b);

  drawDottedHLine(0, 12, 128);

  u8g2.setFont(F_HELV14);
  String ps = fmtPrice(price);
  u8g2.drawStr(1, 30, ps.c_str());
  int pw = u8g2.getStrWidth(ps.c_str());
  u8g2.setFont(F_TINY);
  String cu(cur);
  cu.toUpperCase();
  u8g2.drawStr(pw + 5, 29, cu.c_str());

  if (hn > 1) {
    drawSpark(0, 36, 128, 24, hist, hn);
    drawDottedHLine(0, 60, 100);
  } else {
    u8g2.setFont(F_TINY);
    u8g2.drawStr(1, 54, "building chart history...");
  }

  for (int i = 0; i < n && n > 1; i++) {
    int dx = 124 - (n - 1 - i) * 6;
    if (i == idx) u8g2.drawBox(dx, 61, 4, 2);
    else u8g2.drawPixel(dx + 1, 62);
  }
}

static void crypto_draw(CardCtx& c) {
  CryptoData* cd = (CryptoData*)c.d;
  if (!dataGate(cd->ok, "crypto prices")) return;
  int i = (c.st[0] + (millis() - c.t0) / 5000) % cd->n;
  Coin& k = cd->c[i];
  priceScreen(k.id, cd->cur, k.price, k.ch, k.hist, k.hn, i, cd->n);
}
static bool crypto_input(CardCtx& c, uint8_t g) { if (g == G_LONG) { c.st[0]++; return true; } return false; }
static void crypto_speak(CardCtx& c, String& s) {
  CryptoData* cd = (CryptoData*)c.d;
  for (int i = 0; i < cd->n; i++)
    s += String(cd->c[i].id) + " " + fmtPrice(cd->c[i].price) + ", " + (cd->c[i].ch >= 0 ? "up " : "down ") +
         String(fabsf(cd->c[i].ch), 1) + " percent. ";
}
const CardType CT_crypto = {"crypto", sizeof(CryptoData), 300, 0, nullptr, crypto_draw, crypto_input, crypto_fetch, crypto_speak, nullptr};

// ================================================================== stocks (Yahoo Finance chart API)
struct Stk { char sym[14]; char cur[5]; float price, prev; float pts[40]; uint8_t n, fired; };
struct StockData { bool ok; uint8_t n; Stk s[3]; };

static bool stock_fetch(JsonObject p, void* d) {
  StockData* sd = (StockData*)d;
  String syms = p["symbols"] | "AAPL";
  syms.replace(" ", "");
  StockData nd;
  memset(&nd, 0, sizeof(nd));
  int start = 0;
  while (start < (int)syms.length() && nd.n < 3) {
    int e = syms.indexOf(',', start);
    if (e < 0) e = syms.length();
    String sym = syms.substring(start, e);
    start = e + 1;
    if (!sym.length()) continue;
    JsonDocument filter;
    JsonObject r = filter["chart"]["result"].add<JsonObject>();
    r["meta"]["regularMarketPrice"] = true;
    r["meta"]["chartPreviousClose"] = true;
    r["meta"]["currency"] = true;
    r["indicators"]["quote"][0]["close"] = true;
    JsonDocument doc;
    if (!httpJson("https://query1.finance.yahoo.com/v8/finance/chart/" + sym + "?range=1d&interval=15m", doc, &filter,
                  nullptr, nullptr, true)) continue;
    JsonObject res = doc["chart"]["result"][0];
    if (res.isNull()) continue;
    Stk& k = nd.s[nd.n++];
    safeCopy(k.sym, sym.c_str(), sizeof(k.sym));
    safeCopy(k.cur, res["meta"]["currency"] | "", sizeof(k.cur));
    k.price = res["meta"]["regularMarketPrice"] | 0.0f;
    k.prev = res["meta"]["chartPreviousClose"] | k.price;
    JsonArray cl = res["indicators"]["quote"][0]["close"];
    float tmp[100];
    int tn = 0;
    for (JsonVariant v : cl) if (!v.isNull() && tn < 100) tmp[tn++] = v.as<float>();
    int from = tn > 40 ? tn - 40 : 0;
    for (int i = from; i < tn; i++) k.pts[k.n++] = tmp[i];
    for (int j = 0; j < sd->n; j++) if (!strcmp(sd->s[j].sym, k.sym)) k.fired = sd->s[j].fired;
    parseAlerts(p["alerts"] | "", k.sym, k.price, k.fired, k.cur);
  }
  nd.ok = nd.n > 0;
  *sd = nd;
  return nd.ok;
}
static void stock_draw(CardCtx& c) {
  StockData* sd = (StockData*)c.d;
  if (!dataGate(sd->ok, "stock prices")) return;
  int i = (c.st[0] + (millis() - c.t0) / 5000) % sd->n;
  Stk& k = sd->s[i];
  float ch = k.prev ? (k.price - k.prev) / k.prev * 100 : 0;
  priceScreen(k.sym, k.cur, k.price, ch, k.pts, k.n, i, sd->n);
}
static void stock_speak(CardCtx& c, String& s) {
  StockData* sd = (StockData*)c.d;
  for (int i = 0; i < sd->n; i++) {
    Stk& k = sd->s[i];
    float ch = k.prev ? (k.price - k.prev) / k.prev * 100 : 0;
    s += String(k.sym) + " " + fmtPrice(k.price) + ", " + (ch >= 0 ? "up " : "down ") + String(fabsf(ch), 1) + " percent. ";
  }
}
const CardType CT_stock = {"stock", sizeof(StockData), 600, 0, nullptr, stock_draw, crypto_input, stock_fetch, stock_speak, nullptr};

// ================================================================== currency
struct CurData { bool ok; char base[5]; uint8_t n; char to[6][5]; float rate[6]; };
static bool currency_fetch(JsonObject p, void* d) {
  String base = p["base"] | "USD", to = p["to"] | "EUR,GBP,JPY";
  base.toUpperCase();
  to.toUpperCase();
  to.replace(" ", "");
  JsonDocument filter;
  filter["result"] = true;
  CurData nd;
  memset(&nd, 0, sizeof(nd));
  safeCopy(nd.base, base.c_str(), sizeof(nd.base));
  int start = 0;
  while (start < (int)to.length() && nd.n < 6) {
    int e = to.indexOf(',', start);
    if (e < 0) e = to.length();
    String t = to.substring(start, e);
    start = e + 1;
    if (t.length() == 3) { safeCopy(nd.to[nd.n++], t.c_str(), 5); filter["rates"][t] = true; }
  }
  JsonDocument doc;
  if (!httpJson("https://open.er-api.com/v6/latest/" + base, doc, &filter)) return false;
  for (int i = 0; i < nd.n; i++) nd.rate[i] = doc["rates"][nd.to[i]] | 0.0f;
  nd.ok = !strcmp(doc["result"] | "", "success");
  *(CurData*)d = nd;
  return nd.ok;
}
static void currency_draw(CardCtx& c) {
  CurData* cd = (CurData*)c.d;
  if (!dataGate(cd->ok, "exchange rates")) return;
  char b[24];
  snprintf(b, sizeof(b), "1 %s =", cd->base);
  drawTitle(c, "Currency", b);
  int rows = min((int)cd->n, 5);
  for (int i = 0; i < rows; i++) {
    int y = 21 + i * 10;
    u8g2.setFont(F_MED);
    u8g2.drawStr(4, y, cd->to[i]);
    textRight(124, y, fmtPrice(cd->rate[i]).c_str());
  }
}
static void currency_speak(CardCtx& c, String& s) {
  CurData* cd = (CurData*)c.d;
  for (int i = 0; i < cd->n; i++) s += "One " + String(cd->base) + " is " + fmtPrice(cd->rate[i]) + " " + cd->to[i] + ". ";
}
const CardType CT_currency = {"currency", sizeof(CurData), 3600, 0, nullptr, currency_draw, nullptr, currency_fetch, currency_speak, nullptr};

// ================================================================== news (RSS)
struct NewsData { bool ok; uint8_t n; char t[8][112]; };

static int readUntil(Stream& s, char* out, int cap, const char* end, uint32_t& budget) {
  int n = 0, el = strlen(end);
  char tail[12] = {0};
  uint32_t idle = millis();
  while (budget > 0 && millis() - idle < 4000) {
    int ch = s.read();
    if (ch < 0) { delay(2); continue; }
    idle = millis();
    budget--;
    memmove(tail, tail + 1, 10);
    tail[10] = (char)ch;
    if (out && n < cap - 1) out[n++] = (char)ch;
    if (!memcmp(tail + 11 - el, end, el)) {
      if (out) { n = n > el ? n - el : 0; out[n < cap - 1 ? n : cap - 1] = 0; }
      return n;
    }
  }
  if (out) out[n < cap - 1 ? n : cap - 1] = 0;
  return -1;
}

static bool news_fetch(JsonObject p, void* d) {
  NewsData* nd = (NewsData*)malloc(sizeof(NewsData));
  if (!nd) return false;
  memset(nd, 0, sizeof(NewsData));
  int want = constrain((int)(p["count"] | 6), 1, 8);
  String url = p["url"] | "http://feeds.bbci.co.uk/news/world/rss.xml";
  char buf[400];
  httpGet(url, [&](Stream& s) {
    uint32_t budget = 300000;
    int seen = 0;
    while (nd->n < want) {
      if (readUntil(s, nullptr, 0, "<title", budget) < 0) break;
      if (readUntil(s, nullptr, 0, ">", budget) < 0) break;
      if (readUntil(s, buf, sizeof(buf), "</title>", budget) < 0) break;
      if (seen++ == 0) continue;   // channel title
      char* t = buf;
      if (!strncmp(t, "<![CDATA[", 9)) t += 9;
      char* e = strstr(t, "]]>");
      if (e) *e = 0;
      cleanText(t);
      while (*t == ' ') t++;
      if (*t) safeCopy(nd->t[nd->n++], t, sizeof(nd->t[0]));
    }
    return nd->n > 0;
  }, nullptr, nullptr, true);
  nd->ok = nd->n > 0;
  bool ok = nd->ok;
  if (ok) memcpy(d, nd, sizeof(NewsData));
  free(nd);
  return ok;
}
static int newsIdx(CardCtx& c, NewsData* n) { return (c.st[0] + (millis() - c.t0) / 9000) % n->n; }
static void news_draw(CardCtx& c) {
  NewsData* n = (NewsData*)c.d;
  if (!dataGate(n->ok, "headlines")) return;
  int i = newsIdx(c, n);
  char b[12];
  snprintf(b, sizeof(b), "%d/%d", i + 1, n->n);
  drawTitle(c, "News", b);
  u8g2.setFont(F_MED);
  wrapText(0, 21, 127, 11, n->t[i], 4);
}
static void news_speak(CardCtx& c, String& s) {
  NewsData* n = (NewsData*)c.d;
  if (n->ok) s = n->t[newsIdx(c, n)];
}
const CardType CT_news = {"news", sizeof(NewsData), 1800, 0, nullptr, news_draw, crypto_input, news_fetch, news_speak, nullptr};

// ================================================================== quote / joke / fact
struct TextData { bool ok; char t[240]; char a[40]; };
static const char* const LOCAL_QUOTES[][2] = {
  {"Well begun is half done.", "Aristotle"},
  {"The journey of a thousand miles begins with one step.", "Lao Tzu"},
  {"Knowing yourself is the beginning of all wisdom.", "Aristotle"},
  {"It does not matter how slowly you go as long as you do not stop.", "Confucius"},
  {"We suffer more often in imagination than in reality.", "Seneca"},
  {"Waste no more time arguing what a good person should be. Be one.", "Marcus Aurelius"},
  {"Little by little, one travels far.", "Proverb"},
};

static bool quote_fetch(JsonObject p, void* d) {
  bool rnd = !strcmp(p["mode"] | "today", "random");
  JsonDocument doc;
  if (!httpJson(rnd ? "https://zenquotes.io/api/random" : "https://zenquotes.io/api/today", doc)) return false;
  TextData* t = (TextData*)d;
  safeCopy(t->t, doc[0]["q"] | "", sizeof(t->t));
  safeCopy(t->a, doc[0]["a"] | "", sizeof(t->a));
  cleanText(t->t);
  t->ok = t->t[0] != 0;
  if (!rnd) gFetchNext = 6 * 3600;
  return t->ok;
}
static void quote_draw(CardCtx& c) {
  TextData* t = (TextData*)c.d;
  const char *q = t->t, *a = t->a;
  if (!t->ok) {
    struct tm tm;
    int i = (nowTm(tm) ? tm.tm_yday : 0) % (sizeof(LOCAL_QUOTES) / sizeof(LOCAL_QUOTES[0]));
    q = LOCAL_QUOTES[i][0];
    a = LOCAL_QUOTES[i][1];
  }
  char foot[44];
  snprintf(foot, sizeof(foot), "- %s", a);
  textBlock(c, q, foot);
}
static bool refresh_input(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE) { cardRefreshNow(c.idx); audioSfx("pop", EV_UI); return true; }
  return false;
}
static void quote_speak(CardCtx& c, String& s) {
  TextData* t = (TextData*)c.d;
  if (t->ok) s = String(t->t) + " By " + t->a + ".";
}
const CardType CT_quote = {"quote", sizeof(TextData), 3600, 0, nullptr, quote_draw, refresh_input, quote_fetch, quote_speak, nullptr};

static bool joke_fetch(JsonObject p, void* d) {
  JsonDocument doc;
  if (!httpJson("https://v2.jokeapi.dev/joke/Programming,Misc,Pun,Spooky?safe-mode&type=single", doc)) return false;
  TextData* t = (TextData*)d;
  safeCopy(t->t, doc["joke"] | "", sizeof(t->t));
  cleanText(t->t);
  t->ok = t->t[0] != 0;
  return t->ok;
}
static void joke_draw(CardCtx& c) {
  TextData* t = (TextData*)c.d;
  if (!dataGate(t->ok, "a joke")) return;
  textBlock(c, t->t, "2x tap: new");
}
static void text_speak(CardCtx& c, String& s) {
  TextData* t = (TextData*)c.d;
  if (t->ok) s = t->t;
}
static void joke_enter(CardCtx& c) {
  TextData* t = (TextData*)c.d;
  if (t->ok && (c.p["speak"] | false)) { audioSay(t->t, EV_UI); emoSet(E_HAPPY, 3000); }
}
const CardType CT_joke = {"joke", sizeof(TextData), 3600, 0, joke_enter, joke_draw, refresh_input, joke_fetch, text_speak, nullptr};

static bool fact_fetch(JsonObject p, void* d) {
  JsonDocument doc;
  if (!httpJson("https://uselessfacts.jsph.pl/api/v2/facts/random?language=en", doc)) return false;
  TextData* t = (TextData*)d;
  safeCopy(t->t, doc["text"] | "", sizeof(t->t));
  cleanText(t->t);
  t->ok = t->t[0] != 0;
  return t->ok;
}
static void fact_draw(CardCtx& c) {
  TextData* t = (TextData*)c.d;
  if (!dataGate(t->ok, "a fun fact")) return;
  textBlock(c, t->t, "Fun fact");
}
const CardType CT_fact = {"fact", sizeof(TextData), 3600, 0, joke_enter, fact_draw, refresh_input, fact_fetch, text_speak, nullptr};

// ================================================================== word of the day (offline)
static const char* const WORDS[][3] = {
  {"Serendipity", "noun", "finding something good without looking for it"},
  {"Ephemeral", "adj", "lasting for a very short time"},
  {"Luminous", "adj", "full of light; glowing"},
  {"Resilient", "adj", "able to recover quickly from difficulty"},
  {"Quixotic", "adj", "idealistic in an unrealistic way"},
  {"Mellifluous", "adj", "sweet and smooth to hear"},
  {"Petrichor", "noun", "the pleasant smell after rain on dry ground"},
  {"Sonder", "noun", "realising every passer-by has a life as full as yours"},
  {"Ineffable", "adj", "too great to be expressed in words"},
  {"Halcyon", "adj", "calm, peaceful and happy"},
  {"Eloquent", "adj", "fluent and persuasive in speaking"},
  {"Tenacious", "adj", "holding firmly; not giving up"},
  {"Ubiquitous", "adj", "found everywhere"},
  {"Candor", "noun", "the quality of being open and honest"},
  {"Zenith", "noun", "the highest point"},
  {"Wanderlust", "noun", "a strong desire to travel"},
  {"Gregarious", "adj", "fond of company; sociable"},
  {"Lucid", "adj", "clear and easy to understand"},
  {"Aplomb", "noun", "calm self-confidence"},
  {"Verdant", "adj", "green with grass or plants"},
  {"Diligent", "adj", "careful and hard-working"},
  {"Effervescent", "adj", "bubbly; lively and enthusiastic"},
  {"Nostalgia", "noun", "a fond longing for the past"},
  {"Pragmatic", "adj", "dealing with things sensibly and practically"},
  {"Solace", "noun", "comfort in a time of sadness"},
  {"Vivid", "adj", "producing strong, clear images in the mind"},
  {"Whimsical", "adj", "playfully quaint or fanciful"},
  {"Fortitude", "noun", "courage in pain or adversity"},
  {"Kinetic", "adj", "relating to motion"},
  {"Benevolent", "adj", "well-meaning and kindly"},
  {"Curious", "adj", "eager to know or learn something"},
};
#define NWORDS (sizeof(WORDS) / sizeof(WORDS[0]))
static int wordIdx(CardCtx& c) {
  struct tm t;
  return ((nowTm(t) ? t.tm_yday : 0) + c.st[0]) % NWORDS;
}
static void word_draw(CardCtx& c) {
  int i = wordIdx(c);
  drawTitle(c, "Word of the day", WORDS[i][1]);
  u8g2.setFont(F_HELV14);
  textCenter(31, WORDS[i][0]);
  u8g2.setFont(F_SMALL);
  wrapText(2, 44, 124, 9, WORDS[i][2], 3);
}
static bool word_input(CardCtx& c, uint8_t g) { if (g == G_LONG) { c.st[0]++; return true; } return false; }
static void word_speak(CardCtx& c, String& s) { int i = wordIdx(c); s = String(WORDS[i][0]) + ". " + WORDS[i][2] + "."; }
const CardType CT_word = {"word", 0, 0, 0, nullptr, word_draw, word_input, nullptr, word_speak, nullptr};

// ================================================================== ISS
struct IssData { bool ok; float lat, lon; int people; uint8_t calls; };
static bool iss_fetch(JsonObject p, void* d) {
  IssData* s = (IssData*)d;
  JsonDocument doc;
  if (!httpJson("http://api.open-notify.org/iss-now.json", doc)) return false;
  s->lat = atof(doc["iss_position"]["latitude"] | "0");
  s->lon = atof(doc["iss_position"]["longitude"] | "0");
  if (!s->people || s->calls++ % 60 == 0) {
    JsonDocument a;
    if (httpJson("http://api.open-notify.org/astros.json", a)) s->people = a["number"] | 0;
  }
  s->ok = true;
  return true;
}
static void iss_draw(CardCtx& c) {
  IssData* s = (IssData*)c.d;
  if (!dataGate(s->ok, "space station")) return;
  int mx = 0, my = 12, mw = 76, mh = 40;
  u8g2.drawFrame(mx, my, mw, mh);
  for (int x = mx; x < mx + mw; x += 3) u8g2.drawPixel(x, my + mh / 2);
  for (int y = my; y < my + mh; y += 3) u8g2.drawPixel(mx + mw / 2, y);
  for (int k = 1; k < 4; k++) for (int y = my; y < my + mh; y += 6) u8g2.drawPixel(mx + k * mw / 4, y);
  int px = mx + (int)((s->lon + 180) / 360 * (mw - 1)), py = my + (int)((90 - s->lat) / 180 * (mh - 1));
  u8g2.drawCircle(px, py, 2 + (millis() / 300) % 3);
  u8g2.drawDisc(px, py, 1);
  drawTitle(c, "ISS tracker");
  char b[20];
  u8g2.setFont(F_SMALL);
  snprintf(b, sizeof(b), "Lat %.1f", s->lat); u8g2.drawStr(80, 22, b);
  snprintf(b, sizeof(b), "Lon %.1f", s->lon); u8g2.drawStr(80, 32, b);
  snprintf(b, sizeof(b), "Crew %d", s->people); u8g2.drawStr(80, 46, b);
  u8g2.setFont(F_TINY);
  u8g2.drawStr(0, 62, "people in space right now");
}
static void iss_speak(CardCtx& c, String& s) {
  IssData* d = (IssData*)c.d;
  if (d->ok) s = "The space station is at latitude " + String((int)d->lat) + ", longitude " + String((int)d->lon) +
                 ". There are " + String(d->people) + " people in space.";
}
const CardType CT_iss = {"iss", sizeof(IssData), 30, 0, nullptr, iss_draw, nullptr, iss_fetch, iss_speak, nullptr};

// ================================================================== YouTube
struct YtData { bool ok; double subs, views; uint32_t videos; };
static bool youtube_fetch(JsonObject p, void* d) {
  String ch = p["channel"] | "", key = p["key"] | "";
  if (!key.length()) { LOCK(); key = cfg["keys"]["youtube"] | ""; UNLOCK(); }
  if (!ch.length() || !key.length()) return false;
  JsonDocument doc;
  if (!httpJson("https://www.googleapis.com/youtube/v3/channels?part=statistics&id=" + ch + "&key=" + key, doc)) return false;
  JsonObject st = doc["items"][0]["statistics"];
  if (st.isNull()) return false;
  YtData* y = (YtData*)d;
  double old = y->subs;
  y->subs = atof(st["subscriberCount"] | "0");
  y->views = atof(st["viewCount"] | "0");
  y->videos = atol(st["videoCount"] | "0");
  int ms = p["milestone"] | 1000;
  if (y->ok && ms > 0 && floor(y->subs / ms) > floor(old / ms)) {
    char b[48];
    snprintf(b, sizeof(b), "%s subscribers!", fmtNum(floor(y->subs / ms) * ms).c_str());
    notify("Milestone!", b, E_EXCITED, "sfx:cheer", EV_NOTIFY, true, 15);
  }
  y->ok = true;
  return true;
}
static void youtube_draw(CardCtx& c) {
  YtData* y = (YtData*)c.d;
  if (!(c.p["channel"] | "")[0]) { drawCenteredMsg("Set channel ID + key", "in the card settings"); return; }
  if (!dataGate(y->ok, "YouTube stats")) return;
  drawTitle(c, "YouTube");
  u8g2.drawRBox(2, 16, 22, 16, 4);
  u8g2.setDrawColor(0);
  u8g2.drawTriangle(10, 20, 10, 28, 17, 24);
  u8g2.setDrawColor(1);
  u8g2.setFont(F_HELV14);
  String s = y->subs < 100000 ? String((long)y->subs) : fmtNum(y->subs);
  u8g2.drawStr(30, 32, s.c_str());
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(30, 41, "subscribers");
  char b[40];
  snprintf(b, sizeof(b), "%s views  %lu videos", fmtNum(y->views).c_str(), (unsigned long)y->videos);
  textCenter(58, b);
}
static void youtube_speak(CardCtx& c, String& s) {
  YtData* y = (YtData*)c.d;
  if (y->ok) s = "The channel has " + numWords((long)y->subs) + " subscribers.";
}
const CardType CT_youtube = {"youtube", sizeof(YtData), 1800, 0, nullptr, youtube_draw, nullptr, youtube_fetch, youtube_speak, nullptr};

// ================================================================== GitHub
struct GhData { bool ok; uint32_t followers, repos, stars; };
static bool github_fetch(JsonObject p, void* d) {
  String u = p["user"] | "";
  if (!u.length()) return false;
  String tok = p["token"] | "";
  String auth = "Bearer " + tok;
  JsonDocument doc;
  if (!httpJson("https://api.github.com/users/" + u, doc, nullptr, tok.length() ? "Authorization" : nullptr, auth.c_str())) return false;
  GhData* g = (GhData*)d;
  g->followers = doc["followers"] | 0;
  g->repos = doc["public_repos"] | 0;
  JsonDocument filter;
  filter[0]["stargazers_count"] = true;
  JsonDocument r;
  if (httpJson("https://api.github.com/users/" + u + "/repos?per_page=100", r, &filter, tok.length() ? "Authorization" : nullptr, auth.c_str())) {
    uint32_t s = 0;
    for (JsonObject x : r.as<JsonArray>()) s += x["stargazers_count"] | 0;
    g->stars = s;
  }
  g->ok = true;
  return true;
}
static void github_draw(CardCtx& c) {
  GhData* g = (GhData*)c.d;
  if (!(c.p["user"] | "")[0]) { drawCenteredMsg("Set a GitHub user", "in the card settings"); return; }
  if (!dataGate(g->ok, "GitHub stats")) return;
  drawTitle(c, "GitHub", c.p["user"] | "");
  const char* L[] = {"Stars", "Followers", "Repos"};
  uint32_t V[] = {g->stars, g->followers, g->repos};
  for (int i = 0; i < 3; i++) {
    int x = i * 43;
    char b[12];
    snprintf(b, sizeof(b), "%lu", (unsigned long)V[i]);
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(x + (42 - u8g2.getStrWidth(b)) / 2, 38, b);
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(x + (42 - u8g2.getStrWidth(L[i])) / 2, 52, L[i]);
  }
}
static void github_speak(CardCtx& c, String& s) {
  GhData* g = (GhData*)c.d;
  if (g->ok) s = numWords(g->stars) + " stars and " + numWords(g->followers) + " followers.";
}
const CardType CT_github = {"github", sizeof(GhData), 3600, 0, nullptr, github_draw, nullptr, github_fetch, github_speak, nullptr};

// ================================================================== battery
static void battery_draw(CardCtx& c) {
  int pct = batteryPercent();
  drawTitle(c, "Battery", powerSaver() ? "saver on" : (pct >= 0 && pct <= 15 ? "LOW" : nullptr));
  char b[32];
  if (pct < 0) {
    drawCenteredMsg("No Battery", "Wire 1:2 divider to GPIO0");
    return;
  }
  int p = constrain(pct, 0, 100);

  int x = 8, y = 16, w = 50, h = 24;
  u8g2.drawRFrame(x, y, w, h, 3);
  u8g2.drawRBox(x + w, y + 7, 3, h - 14, 1);
  int fw = (w - 6) * p / 100;
  if (fw >= 4) u8g2.drawRBox(x + 3, y + 3, fw, h - 6, 2);
  else if (fw > 0) u8g2.drawBox(x + 3, y + 3, fw, h - 6);

  char numStr[8];
  snprintf(numStr, sizeof(numStr), "%d", p);
  u8g2.setFont(F_NUM24);
  int nw = u8g2.getStrWidth(numStr);
  u8g2.setFont(F_MED);
  int pw = u8g2.getStrWidth("%");
  int totW = nw + 2 + pw;
  int startX = 64 + (64 - totW) / 2;
  u8g2.setFont(F_NUM24);
  u8g2.drawStr(startX, 38, numStr);
  u8g2.setFont(F_MED);
  u8g2.drawStr(startX + nw + 2, 28, "%");

  drawDottedHLine(0, 44, 128);

  snprintf(b, sizeof(b), "%.2f V   (ADC %d)", batteryMv() / 1000.0f, batteryRaw());
  u8g2.setFont(F_SMALL);
  textCenter(53, b);

  const char* m = powerModeName();
  snprintf(b, sizeof(b), "saver: %s%s", m, !strcmp(m, "auto") ? (powerSaver() ? " [active]" : " [standby]") : "");
  u8g2.setFont(F_TINY);
  textCenter(62, b);
}
static void battery_speak(CardCtx& c, String& s) {
  int pct = batteryPercent();
  if (pct >= 0) s = "Battery is at " + String(pct) + " percent." + (powerSaver() ? " Battery saver is on." : "");
  else s = "Battery not connected.";
}
const CardType CT_battery = {"battery", 0, 0, 0, nullptr, battery_draw, nullptr, nullptr, battery_speak, nullptr};
