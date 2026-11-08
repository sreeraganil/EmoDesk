#include "power.h"
#include "cards.h"
#include "services.h"

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

  // Top header: Asset ticker
  u8g2.setFont(F_BOLD);
  String nm(name);
  nm.toUpperCase();
  u8g2.drawStr(1, 9, nm.substring(0, 10).c_str());
  int nw = u8g2.getUTF8Width(nm.substring(0, 10).c_str());

  // Currency tag
  if (cur && *cur) {
    u8g2.setFont(F_TINY);
    String cu(cur);
    cu.toUpperCase();
    char cTag[12];
    snprintf(cTag, sizeof(cTag), "/%s", cu.c_str());
    u8g2.drawStr(nw + 3, 9, cTag);
  }

  // Right pill badge: Up/Down arrow + percentage
  snprintf(b, sizeof(b), "%+.1f%%", chPct);
  u8g2.setFont(F_TINY);
  int bw = u8g2.getUTF8Width(b) + 12;
  int bx = 127 - bw;
  u8g2.drawRFrame(bx, 1, bw, 8, 2);
  drawIconArrow(bx + 3, 2, chPct >= 0);
  u8g2.drawUTF8(bx + 9, 7, b);

  // Divider
  u8g2.drawHLine(0, 11, 128);

  // Main Price
  u8g2.setFont(F_HELV14);
  String ps = fmtPrice(price);
  u8g2.drawStr(1, 28, ps.c_str());

  // Sparkline Chart & Baseline
  if (hn > 1) {
    // Dashed reference baseline for first/open point
    float mn = hist[0], mx = hist[0];
    for (int i = 1; i < hn; i++) { mn = min(mn, hist[i]); mx = max(mx, hist[i]); }
    float rng = mx - mn;
    if (rng > 0.0001f) {
      int basePy = 32 + 25 - (int)((hist[0] - mn) / rng * 25);
      for (int px = 0; px < 128; px += 4) u8g2.drawPixel(px, basePy);
    }
    drawSpark(0, 32, 128, 26, hist, hn);
  } else {
    u8g2.setFont(F_TINY);
    u8g2.drawStr(1, 48, "gathering trend history...");
  }

  // Page indicator dots
  drawPageDots(126, 61, n, idx);
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
  NewsData* nd = (NewsData*)d;
  if (!nd) return false;
  memset(nd, 0, sizeof(NewsData));
  int want = constrain((int)(p["count"] | 6), 1, 8);
  String url = p["url"] | "http://feeds.bbci.co.uk/news/world/rss.xml";
  char buf[256];
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
  return nd->ok;
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
struct YtMedia {
  bool ok;
  bool isShort;
  bool isNew;
  char title[72];
  char age[16];
  double views;
};

struct YtData {
  bool ok;
  char title[32];
  double subs, views;
  uint32_t videos;
  uint32_t nextMilestone;
  uint32_t toMilestone;
  uint8_t milestonePct;
  YtMedia latestVideo;
  YtMedia latestShort;
};

static uint32_t parseIsoDuration(const char* s) {
  if (!s || s[0] != 'P') return 0;
  uint32_t sec = 0, cur = 0;
  for (const char* p = s; *p; p++) {
    if (isdigit((unsigned char)*p)) {
      cur = cur * 10 + (*p - '0');
    } else if (*p == 'H') {
      sec += cur * 3600;
      cur = 0;
    } else if (*p == 'M') {
      sec += cur * 60;
      cur = 0;
    } else if (*p == 'S') {
      sec += cur;
      cur = 0;
    }
  }
  return sec;
}

static bool hasShortTag(const char* title) {
  if (!title) return false;
  String t = title;
  t.toLowerCase();
  return (t.indexOf("#short") >= 0);
}

struct TempUpload {
  char id[16];
  char title[72];
  char age[16];
  bool isNew;
};

static bool youtube_fetch(JsonObject p, void* d) {
  String ch = p["channel"] | "", key = p["key"] | "";
  if (!key.length()) { LOCK(); key = cfg["keys"]["youtube"] | ""; UNLOCK(); }
  ch.trim();
  if (!ch.length() || !key.length()) return false;

  String url = "https://www.googleapis.com/youtube/v3/channels?part=snippet,statistics,contentDetails&fields=" + urlEncode("items(snippet(title),statistics(subscriberCount,viewCount,videoCount),contentDetails(relatedPlaylists(uploads)))") + "&key=" + key;
  if (ch.startsWith("UC") && ch.length() == 24) {
    url += "&id=" + ch;
  } else {
    String handle = ch;
    if (handle.startsWith("@")) handle = handle.substring(1);
    url += "&forHandle=" + handle;
  }

  JsonDocument doc;
  if (!httpJson(url, doc)) return false;
  JsonObject item = doc["items"][0];
  if (item.isNull()) return false;

  JsonObject st = item["statistics"];
  JsonObject sn = item["snippet"];
  if (st.isNull()) return false;

  YtData* y = (YtData*)d;
  double old = y->subs;

  if (!sn.isNull()) {
    safeCopy(y->title, sn["title"] | "", sizeof(y->title));
  } else {
    safeCopy(y->title, "YouTube", sizeof(y->title));
  }

  y->subs = atof(st["subscriberCount"] | "0");
  y->views = atof(st["viewCount"] | "0");
  y->videos = atol(st["videoCount"] | "0");
  y->ok = true;

  // Milestone calculation
  uint32_t userStep = p["milestone"] | 0;
  uint32_t s = (uint32_t)y->subs;
  uint32_t step = userStep;
  if (step < 100) {
    if (s < 1000) step = 100;
    else if (s < 10000) step = 1000;
    else if (s < 50000) step = 5000;
    else if (s < 100000) step = 10000;
    else if (s < 1000000) step = 50000;
    else step = 100000;
  }
  y->nextMilestone = ((s / step) + 1) * step;
  uint32_t prevM = y->nextMilestone - step;
  uint32_t curProg = (s > prevM) ? (s - prevM) : 0;
  float frac = step > 0 ? ((float)curProg / (float)step) : 0.0f;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  y->milestonePct = (uint8_t)(frac * 100.0f);
  y->toMilestone = (y->nextMilestone > s) ? (y->nextMilestone - s) : 0;

  // Milestone celebration alert
  if (y->ok && old > 0 && s > (uint32_t)old && (s / step) > ((uint32_t)old / step)) {
    char b[48];
    snprintf(b, sizeof(b), "%s reached %s subs!", y->title[0] ? y->title : "Channel", fmtNum((s / step) * step).c_str());
    notify("MILESTONE!", b, E_EXCITED, "sfx:cheer", EV_NOTIFY, true, 15);
  }

  // Reset latest media
  memset(&y->latestVideo, 0, sizeof(y->latestVideo));
  memset(&y->latestShort, 0, sizeof(y->latestShort));

  String uploadsId = item["contentDetails"]["relatedPlaylists"]["uploads"] | "";
  if (!uploadsId.length() && ch.startsWith("UC") && ch.length() == 24) {
    uploadsId = "UU" + ch.substring(2);
  }

  // Free channel doc heap before subsequent TLS requests
  doc.clear();

  if (uploadsId.length()) {
    String plUrl = "https://www.googleapis.com/youtube/v3/playlistItems?part=snippet&playlistId=" + uploadsId +
                   "&maxResults=4&fields=" + urlEncode("items(snippet(title,publishedAt,resourceId(videoId)))") +
                   "&key=" + key;
    JsonDocument plDoc;
    JsonDocument plFilter;
    plFilter["items"][0]["snippet"]["title"] = true;
    plFilter["items"][0]["snippet"]["publishedAt"] = true;
    plFilter["items"][0]["snippet"]["resourceId"]["videoId"] = true;

    TempUpload tmp[4];
    int nTmp = 0;
    String idList = "";

    if (httpJson(plUrl, plDoc, &plFilter)) {
      JsonArray arr = plDoc["items"].as<JsonArray>();
      for (JsonObject plItem : arr) {
        if (nTmp >= 4) break;
        JsonObject snip = plItem["snippet"];
        String vId = snip["resourceId"]["videoId"] | "";
        if (!vId.length()) continue;

        safeCopy(tmp[nTmp].id, vId.c_str(), sizeof(tmp[nTmp].id));
        safeCopy(tmp[nTmp].title, snip["title"] | "", sizeof(tmp[nTmp].title));

        const char* pubAt = snip["publishedAt"] | "";
        tmp[nTmp].age[0] = '\0';
        tmp[nTmp].isNew = false;
        if (pubAt[0]) {
          time_t pubT = parseISO(pubAt);
          time_t now = time(nullptr);
          int32_t diff = (int32_t)(now - pubT);
          if (diff < 0) diff = 0;
          if (diff < 3600) snprintf(tmp[nTmp].age, sizeof(tmp[nTmp].age), "%dm ago", max(1, (int)(diff / 60)));
          else if (diff < 86400) snprintf(tmp[nTmp].age, sizeof(tmp[nTmp].age), "%dh ago", (int)(diff / 3600));
          else if (diff < 7 * 86400) snprintf(tmp[nTmp].age, sizeof(tmp[nTmp].age), "%dd ago", (int)(diff / 86400));
          else if (diff < 30 * 86400) snprintf(tmp[nTmp].age, sizeof(tmp[nTmp].age), "%dw ago", (int)(diff / (7 * 86400)));
          else snprintf(tmp[nTmp].age, sizeof(tmp[nTmp].age), "%dmo ago", (int)(diff / (30 * 86400)));
          tmp[nTmp].isNew = (diff <= 48 * 3600);
        }

        if (idList.length()) idList += ",";
        idList += vId;
        nTmp++;
      }
    }

    plDoc.clear(); // Free heap before next TLS call

    if (idList.length()) {
      String vUrl = "https://www.googleapis.com/youtube/v3/videos?part=contentDetails,statistics&id=" + idList +
                    "&fields=" + urlEncode("items(id,statistics(viewCount),contentDetails(duration))") +
                    "&key=" + key;
      JsonDocument vDoc;
      JsonDocument vFilter;
      vFilter["items"][0]["id"] = true;
      vFilter["items"][0]["statistics"]["viewCount"] = true;
      vFilter["items"][0]["contentDetails"]["duration"] = true;

      if (httpJson(vUrl, vDoc, &vFilter)) {
        JsonArray vItems = vDoc["items"].as<JsonArray>();
        for (JsonObject vObj : vItems) {
          const char* curId = vObj["id"] | "";
          if (!curId[0]) continue;

          int idx = -1;
          for (int i = 0; i < nTmp; i++) {
            if (strcmp(tmp[i].id, curId) == 0) { idx = i; break; }
          }
          if (idx < 0) continue;

          double vCount = vObj["statistics"]["viewCount"].as<double>();
          const char* durStr = vObj["contentDetails"]["duration"] | "";
          uint32_t dur = parseIsoDuration(durStr);
          bool isShort = (dur > 0 && dur <= 180) || hasShortTag(tmp[idx].title);

          if (isShort && !y->latestShort.ok) {
            y->latestShort.ok = true;
            y->latestShort.isShort = true;
            y->latestShort.views = vCount;
            y->latestShort.isNew = tmp[idx].isNew;
            safeCopy(y->latestShort.title, tmp[idx].title, sizeof(y->latestShort.title));
            safeCopy(y->latestShort.age, tmp[idx].age, sizeof(y->latestShort.age));
          } else if (!isShort && !y->latestVideo.ok) {
            y->latestVideo.ok = true;
            y->latestVideo.isShort = false;
            y->latestVideo.views = vCount;
            y->latestVideo.isNew = tmp[idx].isNew;
            safeCopy(y->latestVideo.title, tmp[idx].title, sizeof(y->latestVideo.title));
            safeCopy(y->latestVideo.age, tmp[idx].age, sizeof(y->latestVideo.age));
          }
        }
      }
    }

    // Fallback if neither video nor short was categorized but items were retrieved
    if (nTmp > 0 && !y->latestVideo.ok && !y->latestShort.ok) {
      y->latestVideo.ok = true;
      y->latestVideo.isShort = false;
      safeCopy(y->latestVideo.title, tmp[0].title, sizeof(y->latestVideo.title));
      safeCopy(y->latestVideo.age, tmp[0].age, sizeof(y->latestVideo.age));
      y->latestVideo.isNew = tmp[0].isNew;
    }
  }

  y->ok = true;
  return true;
}

static void drawMediaPage(CardCtx& c, const char* chName, const char* rBadge, const YtMedia& m) {
  drawHeader(chName, rBadge);

  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 21, m.isShort ? "Latest Short:" : "Latest Video:");

  if (m.isNew) {
    drawBadge(126, 21, "NEW");
  } else {
    drawBadge(126, 21, m.isShort ? "SHORT" : "VIDEO");
  }

  u8g2.setFont(F_SMALL);
  marquee(2, 33, 124, m.title, c.t0, 24);

  drawDottedHLine(0, 46, 128);

  u8g2.setFont(F_SMALL);
  char vRow[48];
  snprintf(vRow, sizeof(vRow), "%s views  |  %s", fmtNum(m.views).c_str(), m.age[0] ? m.age : "Recent");
  textCenter(58, vRow);
}

static void youtube_draw(CardCtx& c) {
  YtData* y = (YtData*)c.d;
  if (!(c.p["channel"] | "")[0]) { drawCenteredMsg("Set channel ID or handle", "in card settings"); return; }
  if (!dataGate(y->ok, "YouTube stats")) return;

  int mediaPages = (y->latestVideo.ok ? 1 : 0) + (y->latestShort.ok ? 1 : 0);
  int totalPages = 1 + mediaPages;
  bool autoCycle = (c.p["cycle"] | true);
  uint8_t page = (autoCycle && totalPages > 1) ? (((millis() - c.t0) / 4500) % totalPages) : 0;

  const char* chName = y->title[0] ? y->title : "YouTube";
  char pBuf[8];
  snprintf(pBuf, sizeof(pBuf), "%d/%d", page + 1, totalPages);
  const char* rBadge = (totalPages > 1) ? pBuf : "Stats";

  if (page == 0) {
    drawHeader(chName, rBadge);

    // YouTube Red Play Button icon
    u8g2.drawRBox(2, 16, 22, 16, 4);
    u8g2.setDrawColor(0);
    u8g2.drawTriangle(10, 20, 10, 28, 17, 24);
    u8g2.setDrawColor(1);

    u8g2.setFont(F_HELV14);
    String s = y->subs < 100000 ? String((long)y->subs) : fmtNum(y->subs);
    u8g2.drawStr(29, 31, s.c_str());

    u8g2.setFont(F_SMALL);
    u8g2.drawStr(29, 41, "subscribers");

    drawDottedHLine(0, 47, 128);

    u8g2.setFont(F_SMALL);
    char b[48];
    snprintf(b, sizeof(b), "%s views  |  %lu vids", fmtNum(y->views).c_str(), (unsigned long)y->videos);
    textCenter(58, b);

  } else {
    // Media subpages (Latest Video and/or Latest Short)
    if (y->latestVideo.ok && y->latestShort.ok) {
      if (page == 1) {
        drawMediaPage(c, chName, rBadge, y->latestVideo);
      } else {
        drawMediaPage(c, chName, rBadge, y->latestShort);
      }
    } else if (y->latestVideo.ok) {
      drawMediaPage(c, chName, rBadge, y->latestVideo);
    } else if (y->latestShort.ok) {
      drawMediaPage(c, chName, rBadge, y->latestShort);
    }
  }
}

static void youtube_speak(CardCtx& c, String& s) {
  YtData* y = (YtData*)c.d;
  if (!y || !y->ok) { s = "YouTube data not available."; return; }
  s = (y->title[0] ? String(y->title) : "YouTube channel") + " has " + numWords((long)y->subs) + " subscribers and " +
      fmtNum(y->views) + " total views across " + String(y->videos) + " videos.";
  if (y->latestVideo.ok) {
    s += " Latest video, " + String(y->latestVideo.title) + ", has " + fmtNum(y->latestVideo.views) + " views.";
  }
  if (y->latestShort.ok) {
    s += " Latest short, " + String(y->latestShort.title) + ", has " + fmtNum(y->latestShort.views) + " views.";
  }
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


// ================================================================== cinema (CineWish & TMDB)
enum CineView : uint8_t { CV_SOON = 0, CV_OUT, CV_LIST, CV_PICK, CV_STATS, CV_COUNT };

CinemaData latestCinema = {false, false, 0, {}, "", "", 0.0f, 0, 0, ""};

static const uint8_t XBM_STAR5[] = {0x04, 0x0E, 0x1F, 0x0E, 0x0A};
static const uint8_t XBM_FILM7[] = {0x7F, 0x55, 0x7F, 0x41, 0x7F, 0x55, 0x7F};
static const uint8_t XBM_TV7[]   = {0x14, 0x08, 0x7F, 0x41, 0x41, 0x7F, 0x22};

static void cClapper(int x, int y) {
  u8g2.drawFrame(x, y + 6, 22, 12);
  u8g2.drawBox(x, y, 22, 5);
  u8g2.setDrawColor(0);
  for (int k = 0; k < 4; k++) {
    u8g2.drawLine(x + 2 + k * 6, y + 4, x + 5 + k * 6, y);
    u8g2.drawLine(x + 3 + k * 6, y + 4, x + 6 + k * 6, y);
  }
  u8g2.setDrawColor(1);
  u8g2.drawHLine(x + 3, y + 10, 16);
  u8g2.drawHLine(x + 3, y + 13, 10);
}

static void cRating(int x, int y, uint8_t r, int barW) {
  u8g2.drawXBMP(x, y, 5, 5, XBM_STAR5);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d.%d", r / 10, r % 10);
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(x + 7, y + 6, buf);
  if (barW > 0) {
    int bx = x + 25;
    u8g2.drawFrame(bx, y, barW, 5);
    int fillW = ((barW - 2) * r) / 100;
    if (fillW > 0) u8g2.drawBox(bx + 1, y + 1, fillW, 3);
  }
}

static void cDots(int i, int n, int y = 60) {
  if (n < 2) return;
  int x = 64 - (n * 5 - 2) / 2;
  for (int k = 0; k < n; k++, x += 5) {
    if (k == i) u8g2.drawBox(x, y, 3, 3);
    else u8g2.drawPixel(x + 1, y + 1);
  }
}

static void cFit(int x, int y, int w, const char* s) {
  if (!s || !*s || w <= 8) return;
  char b[48]; strlcpy(b, s, sizeof(b));
  if (u8g2.getUTF8Width(b) <= w) { u8g2.drawUTF8(x, y, b); return; }
  int dw = u8g2.getUTF8Width("..");
  int n = strlen(b);
  while (n > 0) {
    n--;
    while (n > 0 && ((uint8_t)b[n] & 0xC0) == 0x80) n--;
    b[n] = 0;
    if (u8g2.getUTF8Width(b) + dw <= w) break;
  }
  strlcat(b, "..", sizeof(b));
  u8g2.drawUTF8(x, y, b);
}

static void cDateFmt(char* b, size_t n, const char* s, int16_t d) {
  static const char* const MOS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (s && strlen(s) >= 10 && s[4] == '-' && s[7] == '-') {
    int mo = atoi(s + 5);
    int dy = atoi(s + 8);
    if (mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
      snprintf(b, n, "%02d %s", dy, MOS[mo - 1]);
      return;
    }
  }
  if (s && strlen(s) >= 4) {
    snprintf(b, n, "Year %.4s", s);
    return;
  }
  if (d <= 0) strlcpy(b, "Today", n);
  else if (d == 1) strlcpy(b, "Tomorrow", n);
  else snprintf(b, n, "In %dd", d);
}

static int16_t daysFromDate(const char* s) {
  if (!s || strlen(s) < 4) return 0;
  int yr = atoi(s);
  int mo = 1, dy = 1;
  if (strlen(s) >= 7 && s[4] == '-') mo = atoi(s + 5);
  if (strlen(s) >= 10 && s[7] == '-') dy = atoi(s + 8);
  if (yr < 1970 || mo < 1 || mo > 12 || dy < 1 || dy > 31) return 0;

  struct tm target;
  memset(&target, 0, sizeof(target));
  target.tm_year = yr - 1900;
  target.tm_mon  = mo - 1;
  target.tm_mday = dy;
  target.tm_hour = 12;
  time_t targetTime = mktime(&target);

  time_t now = time(nullptr);
  if (now > 1600000000) {
    double diff = difftime(targetTime, now);
    return (int16_t)round(diff / 86400.0);
  }
  return 0;
}

static int cFilter(const CinemaData* cd, bool upcoming, int* outIdx) {
  if (!cd || !cd->n) return 0;
  int count = 0;
  for (int i = 0; i < cd->n; i++) {
    // Days >= 0 are Upcoming (Today, Tomorrow, X days). Days < 0 are Released (Past).
    if ((cd->items[i].days >= 0) == upcoming) {
      if (outIdx && count < 8) outIdx[count] = i;
      count++;
    }
  }
  return count;
}

static bool cinema_fetch(JsonObject p, void* d) {
  CinemaData* cd = (CinemaData*)d;
  if (!cd) return false;
  memset(cd, 0, sizeof(CinemaData));

  String feed = p["feed"] | "wishlist";
  String token = p["token"].is<const char*>() ? (p["token"] | "") : "";
  token.trim();
  if (!token.length()) {
    if (cfg["cinema"].is<JsonObject>() && cfg["cinema"]["token"].is<const char*>()) {
      token = cfg["cinema"]["token"] | "";
    } else if (cfg["cinema_token"].is<const char*>()) {
      token = cfg["cinema_token"] | "";
    }
    token.trim();
  }
#ifdef DEFAULT_CINEMA_TOKEN
  if (!token.length()) {
    token = DEFAULT_CINEMA_TOKEN;
    token.trim();
  }
#endif

  String srv = p["server"].is<const char*>() ? (p["server"] | "") : "";
  if (!srv.length()) {
    srv = cfg["cinema"]["url"].is<const char*>() ? (cfg["cinema"]["url"] | "") : DEFAULT_CINEMA_SERVER;
  }
  srv.trim();
  while (srv.endsWith("/")) srv = srv.substring(0, srv.length() - 1);
  if (!srv.length()) return false;

  // Filter for fallback standard endpoints (Wishlist & TMDB)
  JsonDocument filter;
  filter["total"] = true;
  JsonObject itemF = filter["data"][0];
  itemF["_id"] = true;
  itemF["id"] = true;
  itemF["tmdbId"] = true;
  itemF["title"] = true;
  itemF["name"] = true;
  itemF["release_date"] = true;
  itemF["first_air_date"] = true;
  itemF["year"] = true;
  itemF["rating"] = true;
  itemF["vote_average"] = true;
  itemF["type"] = true;
  itemF["media_type"] = true;
  itemF["genre"] = true;
  JsonObject arrF = filter[0];
  arrF["title"] = true;
  arrF["name"] = true;
  arrF["release_date"] = true;
  arrF["first_air_date"] = true;
  arrF["vote_average"] = true;
  arrF["media_type"] = true;

  String authHeader;
  const char* hName = nullptr;
  const char* hVal = nullptr;
  if (token.length()) {
    authHeader = "Bearer " + token;
    hName = "Authorization";
    hVal = authHeader.c_str();
  }

  // 1. First attempt: call dedicated high-performance /api/emo route
  String emoUrl = srv + "/api/emo?feed=" + feed;
  if (token.length()) emoUrl += "&token=" + token;

  JsonDocument doc;
  bool ok = httpJson(emoUrl, doc, nullptr, hName, hVal);

  if (ok && doc["items"].is<JsonArray>()) {
    cd->ok = doc["ok"] | true;
    cd->linked = doc["linked"] | false;
    cd->watchedMonth = doc["stats"]["w"] | 0;
    cd->listSize = doc["stats"]["l"] | 0;

    const char* pt = doc["pick"]["t"] | "";
    const char* pg = doc["pick"]["g"] | "";
    safeCopy(cd->pickTitle, pt, sizeof(cd->pickTitle));
    safeCopy(cd->pickGenre, pg, sizeof(cd->pickGenre));
    cd->pickRating = doc["pick"]["r"] | 0.0f;

    for (JsonObject obj : doc["items"].as<JsonArray>()) {
      if (cd->n >= 8) break;
      const char* t = obj["t"].is<const char*>() ? (const char*)obj["t"] : (const char*)obj["title"];
      if (!t || !*t) continue;

      CineItem& ci = cd->items[cd->n++];
      const char* idStr = obj["id"] | obj["_id"] | "";
      safeCopy(ci.id, idStr, sizeof(ci.id));

      safeCopy(ci.title, t, sizeof(ci.title));
      cleanText(ci.title);

      const char* yrStr = obj["y"] | obj["release_date"] | obj["year"] | "";
      safeCopy(ci.date, yrStr, sizeof(ci.date));

      ci.isTV = obj["k"].is<int>() ? (uint8_t)obj["k"].as<int>() : 0;
      const char* gStr = obj["g"] | obj["genre"] | (ci.isTV ? "Series" : "Cinema");
      safeCopy(ci.genre, gStr, sizeof(ci.genre));

      ci.days = obj["d"].is<int>() ? (int16_t)obj["d"].as<int>() : daysFromDate(ci.date);
      ci.rating = obj["r"] | (obj["rating"] | (obj["vote_average"] | 0.0f));
    }

    if (cd->n > 0) {
      if (!cd->listSize) cd->listSize = cd->n;
      latestCinema = *cd;
      return true;
    }
  }

  // 2. Fallback to standard endpoints if /api/emo not available
  String endpoint = "/api/wishlist?page=1&limit=20";
  if (feed == "theaters" || feed == "now-playing") endpoint = "/api/tmdb/now-playing";
  else if (feed == "upcoming") endpoint = "/api/tmdb/upcoming";
  else if (feed == "ott") endpoint = "/api/tmdb/ott";
  else if (feed == "trending") endpoint = "/api/tmdb/trending";

  if (endpoint.startsWith("/api/wishlist") && token.length()) {
    endpoint += "&token=" + token;
  }

  String url = srv + endpoint;
  doc.clear();
  if (!httpJson(url, doc, &filter, hName, hVal)) {
    if (endpoint.startsWith("/api/wishlist")) {
      url = srv + "/api/tmdb/trending";
      if (!httpJson(url, doc, &filter)) {
        cd->ok = false;
        strlcpy(cd->err, "HTTP fail", sizeof(cd->err));
        return false;
      }
    } else {
      cd->ok = false;
      strlcpy(cd->err, "HTTP fail", sizeof(cd->err));
      return false;
    }
  }

  JsonArray arr;
  if (doc.is<JsonArray>()) {
    arr = doc.as<JsonArray>();
    cd->listSize = arr.size();
  } else if (doc["data"].is<JsonArray>()) {
    arr = doc["data"].as<JsonArray>();
    cd->listSize = doc["total"] | (uint16_t)arr.size();
    cd->linked = true;
  } else {
    cd->ok = false;
    strlcpy(cd->err, "Bad payload", sizeof(cd->err));
    return false;
  }

  float bestR = -1.0f;
  for (JsonObject obj : arr) {
    if (cd->n >= 8) break;
    const char* t = obj["title"].is<const char*>() ? (const char*)obj["title"] : (const char*)obj["name"];
    if (!t || !*t) continue;

    CineItem& ci = cd->items[cd->n++];
    if (obj["_id"].is<const char*>()) safeCopy(ci.id, obj["_id"], sizeof(ci.id));
    else if (obj["id"].is<const char*>()) safeCopy(ci.id, obj["id"], sizeof(ci.id));
    else if (obj["id"].is<long>()) snprintf(ci.id, sizeof(ci.id), "%ld", (long)obj["id"]);
    else ci.id[0] = 0;

    safeCopy(ci.title, t, sizeof(ci.title));
    cleanText(ci.title);

    const char* dt = obj["release_date"].is<const char*>() ? (const char*)obj["release_date"] : 
                    (obj["first_air_date"].is<const char*>() ? (const char*)obj["first_air_date"] : (const char*)obj["year"]);
    if (dt && *dt) safeCopy(ci.date, dt, sizeof(ci.date));

    ci.isTV = (obj["first_air_date"].is<const char*>() || 
              (obj["media_type"].is<const char*>() && !strcmp(obj["media_type"], "tv")) ||
              (obj["type"].is<const char*>() && !strcmp(obj["type"], "tv"))) ? 1 : 0;

    if (obj["genre"].is<const char*>()) {
      safeCopy(ci.genre, obj["genre"], sizeof(ci.genre));
    } else {
      strlcpy(ci.genre, ci.isTV ? "Series" : "Cinema", sizeof(ci.genre));
    }

    if (obj["days"].is<int>()) {
      ci.days = obj["days"];
    } else {
      ci.days = daysFromDate(ci.date);
    }

    if (obj["rating"].is<float>()) ci.rating = obj["rating"].as<float>();
    else ci.rating = obj["vote_average"] | 0.0f;

    if (ci.rating > bestR) {
      bestR = ci.rating;
      safeCopy(cd->pickTitle, ci.title, sizeof(cd->pickTitle));
      safeCopy(cd->pickGenre, ci.genre, sizeof(cd->pickGenre));
      cd->pickRating = ci.rating;
    }
  }

  cd->watchedMonth = cd->n;
  cd->ok = (cd->n > 0);
  if (cd->ok) latestCinema = *cd;
  return cd->ok;
}

static void viewSoon(CardCtx& c, CinemaData* cd, int subIdx) {
  int ix[8], n = cFilter(cd, true, ix);
  if (!n) {
    drawTitle(c, "Coming Soon", nullptr);
    drawCenteredMsg("No upcoming films", "In your wishlist");
    return;
  }
  int i = (unsigned)subIdx % n;
  const CineItem& m = cd->items[ix[i]];
  char hr[10]; snprintf(hr, sizeof(hr), "%d/%d", i + 1, n);
  drawTitle(c, "Coming Soon", hr);

  u8g2.setFont(F_MED);
  marquee(2, 20, 124, m.title, c.st[2], 28);

  if (m.days <= 0) {
    u8g2.setFont(F_BOLD);
    u8g2.drawStr(2, 45, "RELEASING");
    u8g2.setFont(F_HELV14);
    u8g2.drawStr(2, 60, "TODAY!");
    u8g2.drawXBMP(92, 48, 7, 7, m.isTV ? XBM_TV7 : XBM_FILM7);
    cFit(102, 55, 24, m.genre);
  } else {
    char num[8]; snprintf(num, sizeof(num), "%u", (unsigned)m.days);
    u8g2.setFont(F_NUM24);
    u8g2.drawStr(2, 52, num);
    int rx = 2 + u8g2.getStrWidth(num) + 6;

    u8g2.setFont(F_MED);
    u8g2.drawStr(rx, 36, m.days == 1 ? "DAY" : "DAYS");

    u8g2.setFont(F_SMALL);
    char dtBuf[16]; cDateFmt(dtBuf, sizeof(dtBuf), m.date, m.days);
    u8g2.drawStr(rx, 46, dtBuf);
    u8g2.drawXBMP(rx, 50, 7, 7, m.isTV ? XBM_TV7 : XBM_FILM7);
    cFit(rx + 10, 57, 116 - rx, m.genre);
  }
  cDots(i, n);
}

static void viewOut(CardCtx& c, CinemaData* cd, int subIdx) {
  int ix[8], n = cFilter(cd, false, ix);
  if (!n) {
    drawTitle(c, "Out Now", nullptr);
    drawCenteredMsg("All caught up", "Nothing waiting");
    return;
  }
  int i = (unsigned)subIdx % n;
  const CineItem& m = cd->items[ix[i]];
  int a = m.days < 0 ? -m.days : 0;
  char hr[10]; snprintf(hr, sizeof(hr), "%d/%d", i + 1, n);
  drawTitle(c, "Out Now", hr);

  u8g2.setFont(F_MED);
  marquee(2, 20, 124, m.title, c.st[2], 28);

  u8g2.setFont(F_SMALL);
  u8g2.drawXBMP(2, 25, 7, 7, m.isTV ? XBM_TV7 : XBM_FILM7);
  cFit(12, 31, 80, m.genre);

  char b[24];
  if (a == 0)       strlcpy(b, "Out today", sizeof(b));
  else if (a == 1)  strlcpy(b, "Out yesterday", sizeof(b));
  else if (a < 7)   snprintf(b, sizeof(b), "Out %d days ago", a);
  else if (a < 60)  snprintf(b, sizeof(b), "Out %d wk ago", a / 7);
  else if (a < 365) snprintf(b, sizeof(b), "Out %d mo ago", a / 30);
  else {
    if (m.date[0]) snprintf(b, sizeof(b), "Released %.4s", m.date);
    else snprintf(b, sizeof(b), "Out %d yr ago", a / 365);
  }
  u8g2.drawStr(2, 42, b);

  if (a <= 7) {
    u8g2.drawBox(104, 24, 22, 9);
    u8g2.setDrawColor(0); u8g2.drawStr(107, 31, "NEW"); u8g2.setDrawColor(1);
  }
  if (m.rating > 0.05f) cRating(2, 48, (uint8_t)(m.rating * 10), 60);
  else                  u8g2.drawStr(2, 55, "Not rated");
  cDots(i, n);
}

static void viewList(CardCtx& c, CinemaData* cd, int sel) {
  char hr[8]; snprintf(hr, sizeof(hr), "%d", cd->n);
  drawTitle(c, "Wishlist", hr);
  if (!cd->n) {
    drawCenteredMsg("List is empty", "Add in CineWish");
    return;
  }
  u8g2.setFont(F_SMALL);
  int cur = (unsigned)sel % cd->n;
  int first = constrain(cur - 2, 0, max(0, cd->n - 5));
  for (int r = 0; r < 5 && first + r < cd->n; r++) {
    const CineItem& m = cd->items[first + r];
    int y = 11 + r * 10;
    char tag[8];
    if (m.days > 0)       snprintf(tag, sizeof(tag), "%dd", m.days);
    else if (m.days == 0) strlcpy(tag, "NOW", sizeof(tag));
    else                  strlcpy(tag, "OUT", sizeof(tag));

    if (first + r == cur) {
      u8g2.drawBox(0, y, 125, 10);
      u8g2.setDrawColor(0);
    }
    cFit(3, y + 8, 96, m.title);
    u8g2.drawStr(122 - u8g2.getStrWidth(tag), y + 8, tag);
    u8g2.setDrawColor(1);
  }
  if (cd->n > 5) {
    int h = 50 * 5 / cd->n;
    int sy = 11 + (50 - h) * first / (cd->n - 5);
    u8g2.drawBox(126, sy, 2, h);
  }
}

static void viewPick(CardCtx& c, CinemaData* cd) {
  drawTitle(c, "Tonight's Pick", nullptr);
  if (!cd->pickTitle[0]) {
    drawCenteredMsg("No picks yet", "Rate in CineWish");
    return;
  }
  cClapper(4, 20);
  u8g2.setFont(F_MED);
  wrapText(30, 24, 96, 11, cd->pickTitle, 2);
  if (cd->pickRating > 0.05f) cRating(30, 43, (uint8_t)(cd->pickRating * 10), 0);
  u8g2.setFont(F_SMALL);
  cFit(60, 49, 66, cd->pickGenre);
  u8g2.drawHLine(0, 53, 128);
  u8g2.drawStr(2, 62, "picked for you");
}

static void viewStats(CardCtx& c, CinemaData* cd) {
  drawTitle(c, "CineWish", nullptr);
  char b[8];
  u8g2.setFont(F_NUM24);
  snprintf(b, sizeof(b), "%u", cd->watchedMonth);
  int w1 = u8g2.getStrWidth(b);
  u8g2.drawStr(32 - w1 / 2, 40, b);

  snprintf(b, sizeof(b), "%u", cd->listSize);
  int w2 = u8g2.getStrWidth(b);
  u8g2.drawStr(96 - w2 / 2, 40, b);

  u8g2.drawVLine(64, 14, 34);
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(14, 50, "watched");
  u8g2.drawStr(7, 58, "this month");
  u8g2.drawStr(79, 50, "on your");
  u8g2.drawStr(74, 58, "wishlist");
}

static void cinema_enter(CardCtx& c) {
  uint32_t now = millis();
  c.st[0] = CV_SOON;
  c.st[1] = 0;
  c.st[2] = now;
  c.st[3] = now;
}

static void cinema_draw(CardCtx& c) {
  CinemaData* cd = (CinemaData*)c.d;
  if (!cd) return;
  if (!dataGate(cd->ok, "cinema")) return;

  uint32_t& view  = c.st[0];
  uint32_t& idx   = c.st[1];
  uint32_t& tLast = c.st[2];
  uint32_t& tView = c.st[3];

  uint32_t now = millis();
  if (!tLast) tLast = now;
  if (!tView) tView = now;

  // Check if view mode is forced in card params, e.g. "view": "soon", "out", "watchlist", "pick", "stats"
  const char* forcedView = c.p["view"] | "auto";
  bool isAuto = !strcmp(forcedView, "auto");
  if (!isAuto) {
    if (!strcmp(forcedView, "soon")) view = CV_SOON;
    else if (!strcmp(forcedView, "out")) view = CV_OUT;
    else if (!strcmp(forcedView, "watchlist") || !strcmp(forcedView, "list")) view = CV_LIST;
    else if (!strcmp(forcedView, "pick")) view = CV_PICK;
    else if (!strcmp(forcedView, "stats")) view = CV_STATS;
  } else {
    // Auto mode: switch screens every 7 seconds
    if (now - tView > 7000) {
      tView = now;
      tLast = now;
      view = (view + 1) % CV_COUNT;
      idx = 0;
    }
  }

  // Advance items inside current view every 3.5 seconds
  if (now - tLast > 3500) {
    tLast = now;
    idx++;
  }

  // Skip views that have no data so screen never gets stuck on an empty placeholder
  int ixSoon[8], nSoon = cFilter(cd, true, ixSoon);
  int ixOut[8],  nOut  = cFilter(cd, false, ixOut);

  if (isAuto) {
    if (view == CV_SOON && !nSoon) view = (nOut ? CV_OUT : CV_LIST);
    if (view == CV_OUT && !nOut)   view = CV_LIST;
    if (view == CV_PICK && !cd->pickTitle[0]) view = CV_STATS;
  }

  switch (view % CV_COUNT) {
    case CV_SOON:  viewSoon(c, cd, idx);  break;
    case CV_OUT:   viewOut(c, cd, idx);   break;
    case CV_LIST:  viewList(c, cd, idx);  break;
    case CV_PICK:  viewPick(c, cd);       break;
    case CV_STATS: viewStats(c, cd);      break;
    default:       viewSoon(c, cd, idx);  break;
  }
}

// Return false so all EmoDesk global touch gestures work unimpeded:
// Tap -> Next Card, Double-tap -> Prev Card, Triple-tap -> Home, Long-press -> Speak Card!
static bool cinema_input(CardCtx& c, uint8_t g) {
  return false;
}

static void cinema_speak(CardCtx& c, String& s) {
  CinemaData* cd = (CinemaData*)c.d;
  if (!cd || !cd->ok || !cd->n) {
    s = "No cinema information available.";
    return;
  }
  uint32_t view = c.st[0];
  uint32_t idx  = c.st[1];

  if (view == CV_SOON) {
    int ix[8], n = cFilter(cd, true, ix);
    if (n) {
      const CineItem& m = cd->items[ix[idx % n]];
      s = String(m.title) + " releases in " + String(m.days) + " days.";
      return;
    }
  } else if (view == CV_OUT) {
    int ix[8], n = cFilter(cd, false, ix);
    if (n) {
      const CineItem& m = cd->items[ix[idx % n]];
      s = String(m.title) + " is out now.";
      if (m.rating > 0.05f) {
        char rBuf[24]; snprintf(rBuf, sizeof(rBuf), " Rated %.1f.", m.rating);
        s += rBuf;
      }
      return;
    }
  } else if (view == CV_PICK && cd->pickTitle[0]) {
    s = "Tonight's pick is " + String(cd->pickTitle) + ".";
    return;
  } else if (view == CV_STATS) {
    s = "You have watched " + String(cd->watchedMonth) + " movies, with " + String(cd->listSize) + " on your wishlist.";
    return;
  }
  const CineItem& m = cd->items[idx % cd->n];
  s = "Featured: " + String(m.title);
}

const CardType CT_cinema = {"cinema", sizeof(CinemaData), 1800, 0, cinema_enter, cinema_draw, cinema_input, cinema_fetch, cinema_speak, nullptr};
