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

static void textBlock(CardCtx& c, const char* hdr, const char* txt, const char* foot, const char* rBadge = nullptr) {
  drawTitle(c, hdr, rBadge);
  u8g2.setFont(F_SMALL);
  int bodyW = 120;
  int lines = wrapText(0, 0, bodyW, 9, txt, 0);
  int avail = (foot && *foot) ? 4 : 5;
  int skip = 0;
  if (lines > avail) {
    int steps = lines - avail + 1;
    skip = ((millis() - c.t0) / 2400) % (steps + 2);
    skip = skip >= steps ? steps - 1 : skip;
    cardHold = true;
    if ((millis() - c.t0) > (uint32_t)(steps + 3) * 2400) cardHold = false;
  }
  int yStart = 20;
  if (lines < avail) yStart += ((avail - lines) * 9) / 2;
  wrapText(2, yStart, bodyW, 9, txt, avail, skip);

  if (lines > avail) {
    int sbH = avail * 9;
    int thumbH = max(4, (sbH * avail) / lines);
    int maxStep = max(1, lines - avail);
    int thumbY = 13 + ((sbH - thumbH) * skip) / maxStep;
    u8g2.drawVLine(126, 13, sbH);
    u8g2.drawBox(125, thumbY, 3, thumbH);
  }
  if (foot && *foot) {
    drawDottedHLine(0, 53, 128);
    u8g2.setFont(F_TINY);
    drawTextRightFit(126, 62, 122, foot);
  }
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
  if (!cd) return false;
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
  snprintf(b, sizeof(b), "%+.1f%%", chPct);

  String nm(name);
  nm.toUpperCase();
  drawHeader(nm.c_str(), b);

  // Price row (y=13..29): auto-fit font so large values + currency never clip
  String ps = fmtPrice(price);
  String cu(cur && *cur ? cur : "");
  cu.toUpperCase();

  u8g2.setFont(F_TINY);
  int cw = cu.length() ? (u8g2.getStrWidth(cu.c_str()) + 4) : 0;
  int maxPw = 114 - cw;

  u8g2.setFont(F_HELV14);
  if (u8g2.getStrWidth(ps.c_str()) > maxPw) {
    u8g2.setFont(F_BOLD);
    if (u8g2.getStrWidth(ps.c_str()) > maxPw) u8g2.setFont(F_MED);
  }
  u8g2.drawStr(2, 28, ps.c_str());
  int pw = u8g2.getStrWidth(ps.c_str());

  if (cu.length() && pw + 4 + cw <= 116) {
    u8g2.setFont(F_TINY);
    u8g2.drawStr(pw + 5, 27, cu.c_str());
  }
  drawIconArrow(119, 17, chPct >= 0.0f);

  // Sparkline & range footer
  if (hn > 1) {
    float mn = hist[0], mx = hist[0];
    for (int i = 1; i < hn; i++) { mn = min(mn, hist[i]); mx = max(mx, hist[i]); }
    float rng = mx - mn;
    if (rng > 0.0001f) {
      int basePy = 31 + 24 - (int)((hist[0] - mn) / rng * 24);
      drawDottedHLine(2, basePy, 124);
    }
    drawSpark(2, 31, 124, 24, hist, hn);
    char rBuf[36];
    snprintf(rBuf, sizeof(rBuf), "L:%s H:%s", fmtPrice(mn).c_str(), fmtPrice(mx).c_str());
    u8g2.setFont(F_TINY);
    drawTextFit(2, 63, (n > 1) ? 100 : 124, rBuf);
  } else {
    drawDottedHLine(2, 43, 124);
    u8g2.setFont(F_TINY);
    textCenter(51, "gathering price history...");
  }

  drawPageDots(126, 61, n, idx);
}

static void crypto_draw(CardCtx& c) {
  CryptoData* cd = (CryptoData*)c.d;
  if (!cd || !dataGate(cd->ok, "crypto prices")) return;
  if (!cd->n) { drawCenteredMsg("No coins configured"); return; }
  int i = (c.st[0] + (millis() - c.t0) / 5000) % cd->n;
  Coin& k = cd->c[i];
  priceScreen(k.id, cd->cur, k.price, k.ch, k.hist, k.hn, i, cd->n);
}
static bool crypto_input(CardCtx& c, uint8_t g) { if (g == G_LONG) { c.st[0]++; return true; } return false; }
static void crypto_speak(CardCtx& c, String& s) {
  CryptoData* cd = (CryptoData*)c.d;
  if (!cd || !cd->ok) return;
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
  if (!sd) return false;
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
  if (!sd || !dataGate(sd->ok, "stock prices")) return;
  if (!sd->n) { drawCenteredMsg("No symbols loaded"); return; }
  int i = (c.st[0] + (millis() - c.t0) / 5000) % sd->n;
  Stk& k = sd->s[i];
  float ch = k.prev ? (k.price - k.prev) / k.prev * 100 : 0;
  priceScreen(k.sym, k.cur, k.price, ch, k.pts, k.n, i, sd->n);
}
static void stock_speak(CardCtx& c, String& s) {
  StockData* sd = (StockData*)c.d;
  if (!sd || !sd->ok) return;
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
  if (!d) return false;
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
  nd.ok = !strcmp(doc["result"] | "", "success") && nd.n > 0;
  *(CurData*)d = nd;
  return nd.ok;
}
static void currency_draw(CardCtx& c) {
  CurData* cd = (CurData*)c.d;
  if (!cd || !dataGate(cd->ok, "exchange rates")) return;
  if (!cd->n) { drawCenteredMsg("No currency pairs"); return; }
  char b[24];
  snprintf(b, sizeof(b), "1 %s =", cd->base);
  drawTitle(c, "Currency", b);
  int pages = (cd->n > 5) ? 2 : 1;
  int pg = (pages > 1) ? (((millis() - c.t0) / 4500) % pages) : 0;
  int first = pg * 4;
  int perPage = (pages > 1) ? 4 : 5;
  int rows = min((int)cd->n - first, perPage);
  for (int r = 0; r < rows; r++) {
    int i = first + r;
    int y = 21 + r * 10;
    u8g2.drawRFrame(2, y - 8, 28, 10, 2);
    u8g2.setFont(F_SMALL);
    drawTextFitCenter(16, y - 1, 24, cd->to[i]);
    u8g2.setFont(F_MED);
    drawTextRightFit(126, y, 90, fmtPrice(cd->rate[i]).c_str());
    if (r < rows - 1) drawDottedHLine(34, y + 2, 92);
  }
  drawPageDots(126, 61, pages, pg);
}
static void currency_speak(CardCtx& c, String& s) {
  CurData* cd = (CurData*)c.d;
  if (!cd || !cd->ok) return;
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
static int newsIdx(CardCtx& c, NewsData* n) {
  if (!n || !n->n) return 0;
  return (c.st[0] + (millis() - c.t0) / 9000) % n->n;
}
static void news_draw(CardCtx& c) {
  NewsData* n = (NewsData*)c.d;
  if (!n || !dataGate(n->ok, "headlines")) return;
  if (!n->n) { drawCenteredMsg("No headlines"); return; }
  int i = newsIdx(c, n);
  char b[12];
  snprintf(b, sizeof(b), "%d/%d", i + 1, n->n);
  drawTitle(c, "News", b);

  u8g2.setFont(F_MED);
  int linesM = wrapText(0, 0, 124, 11, n->t[i], 0);
  if (linesM <= 3) {
    int y0 = 21 + ((3 - linesM) * 11) / 2;
    wrapText(2, y0, 124, 11, n->t[i], 3);
  } else {
    u8g2.setFont(F_SMALL);
    int linesS = wrapText(0, 0, 124, 9, n->t[i], 0);
    int skip = (linesS > 4) ? (((millis() - c.t0) / 2500) % max(1, linesS - 3)) : 0;
    wrapText(2, 20, 124, 9, n->t[i], 4, skip);
  }

  drawDottedHLine(0, 55, 128);
  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 63, "Hold: next story");
  drawPageDots(126, 61, n->n, i);
}
static void news_speak(CardCtx& c, String& s) {
  NewsData* n = (NewsData*)c.d;
  if (n && n->ok && n->n) s = n->t[newsIdx(c, n)];
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
  if (!d) return false;
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
  const char *q = (t && t->ok) ? t->t : nullptr, *a = (t && t->ok) ? t->a : nullptr;
  if (!q) {
    struct tm tm;
    int i = (nowTm(tm) ? tm.tm_yday : 0) % (sizeof(LOCAL_QUOTES) / sizeof(LOCAL_QUOTES[0]));
    q = LOCAL_QUOTES[i][0];
    a = LOCAL_QUOTES[i][1];
  }
  char foot[44];
  snprintf(foot, sizeof(foot), "- %s", a ? a : "");
  textBlock(c, "Quote", q, foot);
}
static bool refresh_input(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE) { cardRefreshNow(c.idx); audioSfx("pop", EV_UI); return true; }
  return false;
}
static void quote_speak(CardCtx& c, String& s) {
  TextData* t = (TextData*)c.d;
  if (t && t->ok) s = String(t->t) + " By " + t->a + ".";
}
const CardType CT_quote = {"quote", sizeof(TextData), 3600, 0, nullptr, quote_draw, refresh_input, quote_fetch, quote_speak, nullptr};

static bool joke_fetch(JsonObject p, void* d) {
  if (!d) return false;
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
  if (!t || !dataGate(t->ok, "a joke")) return;
  textBlock(c, "Joke", t->t, nullptr, "2x: new");
}
static void text_speak(CardCtx& c, String& s) {
  TextData* t = (TextData*)c.d;
  if (t && t->ok) s = t->t;
}
static void joke_enter(CardCtx& c) {
  TextData* t = (TextData*)c.d;
  if (t && t->ok && (c.p["speak"] | false)) { audioSay(t->t, EV_UI); emoSet(E_HAPPY, 3000); }
}
const CardType CT_joke = {"joke", sizeof(TextData), 3600, 0, joke_enter, joke_draw, refresh_input, joke_fetch, text_speak, nullptr};

static bool fact_fetch(JsonObject p, void* d) {
  if (!d) return false;
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
  if (!t || !dataGate(t->ok, "a fun fact")) return;
  textBlock(c, "Fun Fact", t->t, nullptr, "2x: new");
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
  drawTitle(c, "Word", WORDS[i][1]);

  u8g2.setFont(F_HELV14);
  if (u8g2.getStrWidth(WORDS[i][0]) > 124) {
    u8g2.setFont(F_BOLD);
    if (u8g2.getStrWidth(WORDS[i][0]) > 124) u8g2.setFont(F_MED);
  }
  textCenter(29, WORDS[i][0]);
  drawDottedHLine(16, 33, 96);

  u8g2.setFont(F_SMALL);
  wrapText(4, 44, 120, 9, WORDS[i][2], 2);

  char idxBuf[12];
  snprintf(idxBuf, sizeof(idxBuf), "%d/%d", i + 1, (int)NWORDS);
  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 63, "Hold: next");
  textRight(126, 63, idxBuf);
}
static bool word_input(CardCtx& c, uint8_t g) { if (g == G_LONG) { c.st[0]++; return true; } return false; }
static void word_speak(CardCtx& c, String& s) { int i = wordIdx(c); s = String(WORDS[i][0]) + ". " + WORDS[i][2] + "."; }
const CardType CT_word = {"word", 0, 0, 0, nullptr, word_draw, word_input, nullptr, word_speak, nullptr};

// ================================================================== ISS
struct IssData { bool ok; float lat, lon; int people; uint8_t calls; };
static bool iss_fetch(JsonObject p, void* d) {
  IssData* s = (IssData*)d;
  if (!s) return false;
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
  if (!s || !dataGate(s->ok, "space station")) return;
  char crw[16];
  snprintf(crw, sizeof(crw), "%d crew", s->people);
  drawTitle(c, "ISS Live", s->people > 0 ? crw : nullptr);

  int mx = 0, my = 12, mw = 78, mh = 41;
  u8g2.drawFrame(mx, my, mw, mh);
  for (int x = mx + 2; x < mx + mw - 1; x += 3) u8g2.drawPixel(x, my + mh / 2);
  for (int y = my + 2; y < my + mh - 1; y += 3) u8g2.drawPixel(mx + mw / 2, y);
  for (int k = 1; k < 4; k++) for (int y = my + 2; y < my + mh - 1; y += 6) u8g2.drawPixel(mx + k * mw / 4, y);
  int px = constrain(mx + 2 + (int)((s->lon + 180.0f) / 360.0f * (mw - 5)), mx + 3, mx + mw - 4);
  int py = constrain(my + 2 + (int)((90.0f - s->lat) / 180.0f * (mh - 5)), my + 3, my + mh - 4);
  u8g2.drawCircle(px, py, 2 + (millis() / 300) % 3);
  u8g2.drawDisc(px, py, 1);

  char b[16];
  u8g2.setFont(F_TINY);
  u8g2.drawStr(82, 19, "LATITUDE");
  u8g2.setFont(F_MED);
  snprintf(b, sizeof(b), "%+.1f", s->lat);
  drawTextRightFit(127, 29, 45, b);

  u8g2.setFont(F_TINY);
  u8g2.drawStr(82, 38, "LONGITUDE");
  u8g2.setFont(F_MED);
  snprintf(b, sizeof(b), "%+.1f", s->lon);
  drawTextRightFit(127, 48, 45, b);

  drawDottedHLine(0, 55, 128);
  u8g2.setFont(F_TINY);
  char orb[40];
  if (s->people > 0) snprintf(orb, sizeof(orb), "%d people in orbit · 408km", s->people);
  else snprintf(orb, sizeof(orb), "Orbit 408km · 27,600 km/h");
  textCenter(63, orb);
}
static void iss_speak(CardCtx& c, String& s) {
  IssData* d = (IssData*)c.d;
  if (d && d->ok) s = "The space station is at latitude " + String((int)d->lat) + ", longitude " + String((int)d->lon) +
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
  YtData* y = (YtData*)d;
  if (!y) return false;
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
  u8g2.drawStr(2, 20, m.isShort ? "Latest Short" : "Latest Video");
  drawBadge(126, 21, m.isNew ? "NEW" : (m.isShort ? "SHORT" : "VIDEO"));

  u8g2.setFont(F_SMALL);
  int lines = wrapText(0, 0, 124, 9, m.title, 0);
  if (lines <= 2) {
    int y0 = (lines == 1) ? 37 : 32;
    wrapText(2, y0, 124, 9, m.title, 2);
  } else {
    marquee(2, 37, 124, m.title, c.t0, 24);
  }

  drawDottedHLine(0, 49, 128);
  u8g2.setFont(F_SMALL);
  char vRow[48];
  snprintf(vRow, sizeof(vRow), "%s views  |  %s", fmtNum(m.views).c_str(), m.age[0] ? m.age : "Recent");
  drawTextFitCenter(64, 60, 124, vRow);
}

static void youtube_draw(CardCtx& c) {
  YtData* y = (YtData*)c.d;
  if (!y) return;
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

    u8g2.drawRBox(2, 15, 22, 16, 4);
    u8g2.setDrawColor(0);
    u8g2.drawTriangle(10, 19, 10, 27, 17, 23);
    u8g2.setDrawColor(1);

    String s = y->subs < 100000 ? String((long)y->subs) : fmtNum(y->subs);
    u8g2.setFont(F_HELV14);
    drawTextFit(29, 28, 97, s.c_str());

    u8g2.setFont(F_TINY);
    u8g2.drawStr(29, 36, "subscribers");

    if (y->nextMilestone > 0) {
      drawBar(2, 41, 86, 5, y->milestonePct / 100.0f);
      char mBuf[20];
      snprintf(mBuf, sizeof(mBuf), "Goal %s", fmtNum(y->nextMilestone).c_str());
      u8g2.setFont(F_TINY);
      drawTextRightFit(126, 46, 36, mBuf);
    }

    drawDottedHLine(0, 51, 128);
    u8g2.setFont(F_SMALL);
    char b[48];
    snprintf(b, sizeof(b), "%s views  |  %lu vids", fmtNum(y->views).c_str(), (unsigned long)y->videos);
    drawTextFitCenter(64, 61, 124, b);

  } else {
    if (y->latestVideo.ok && y->latestShort.ok) {
      drawMediaPage(c, chName, rBadge, page == 1 ? y->latestVideo : y->latestShort);
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
  GhData* g = (GhData*)d;
  if (!g) return false;
  String u = p["user"] | "";
  if (!u.length()) return false;
  String tok = p["token"] | "";
  String auth = "Bearer " + tok;
  JsonDocument doc;
  if (!httpJson("https://api.github.com/users/" + u, doc, nullptr, tok.length() ? "Authorization" : nullptr, auth.c_str())) return false;
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
  if (!g || !dataGate(g->ok, "GitHub stats")) return;
  drawTitle(c, "GitHub", c.p["user"] | "");
  const char* L[] = {"Stars", "Followers", "Repos"};
  uint32_t V[] = {g->stars, g->followers, g->repos};
  for (int i = 0; i < 3; i++) {
    int x = i * 43;
    int cx = x + 21;
    u8g2.drawRFrame(x, 14, 41, 46, 3);
    if (i == 0) drawIconStar(cx - 4, 17);
    else if (i == 1) drawIconHeart(cx - 4, 17);
    else u8g2.drawFrame(cx - 4, 18, 8, 6);
    String numS = (V[i] >= 1000) ? fmtNum((double)V[i]) : String((unsigned long)V[i]);
    u8g2.setFont(F_HELV14);
    drawTextFitCenter(cx, 41, 37, numS.c_str());
    u8g2.setFont(F_TINY);
    drawTextFitCenter(cx, 54, 37, L[i]);
  }
}
static void github_speak(CardCtx& c, String& s) {
  GhData* g = (GhData*)c.d;
  if (g && g->ok) s = numWords(g->stars) + " stars and " + numWords(g->followers) + " followers.";
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
enum CineView : uint8_t { CV_HERO = 0, CV_LIST, CV_PICK, CV_COUNT };

CinemaData latestCinema = {false, false, 0, {}, "", "", 0.0f, 0, 0, ""};

static const uint8_t XBM_STAR5[] = {0x04, 0x0E, 0x1F, 0x0E, 0x0A};
static const uint8_t XBM_STAR7[] = {0x10, 0x10, 0x38, 0x7C, 0xFE, 0x7C, 0x28};
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
  if (r > 100) r = 100;
  u8g2.drawXBMP(x, y, 5, 5, XBM_STAR5);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d.%d", r / 10, r % 10);
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(x + 7, y + 6, buf);
  if (barW > 0) {
    int bx = x + 27;
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
  if (!s || strlen(s) < 4) return -9999;
  int yr = atoi(s);
  int mo = 1, dy = 1;
  if (strlen(s) >= 7 && s[4] == '-') mo = atoi(s + 5);
  if (strlen(s) >= 10 && s[7] == '-') dy = atoi(s + 8);
  if (yr < 1970 || mo < 1 || mo > 12 || dy < 1 || dy > 31) return -9999;

  struct tm target;
  memset(&target, 0, sizeof(target));
  target.tm_year = yr - 1900;
  target.tm_mon  = mo - 1;
  target.tm_mday = dy;
  target.tm_hour = 12;
  target.tm_isdst = -1;
  time_t targetTime = mktime(&target);

  time_t now = time(nullptr);
  if (now > 1600000000) {
    double diff = difftime(targetTime, now);
    return (int16_t)round(diff / 86400.0);
  }
  return -9999;
}

static int cFilter(const CinemaData* cd, bool upcoming, int* outIdx) {
  if (!cd || !cd->n) return 0;
  int count = 0;
  for (int i = 0; i < cd->n; i++) {
    if (cd->items[i].days == -9999) continue;
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
  String srv = p["server"].is<const char*>() ? (p["server"] | "") : "";
  {
    LOCK();
    if (!token.length()) {
      if (cfg["cinema"].is<JsonObject>() && cfg["cinema"]["token"].is<const char*>()) {
        token = cfg["cinema"]["token"] | "";
      } else if (cfg["cinema_token"].is<const char*>()) {
        token = cfg["cinema_token"] | "";
      }
      token.trim();
    }
    if (!srv.length()) {
      srv = cfg["cinema"]["url"].is<const char*>() ? (cfg["cinema"]["url"] | "") : DEFAULT_CINEMA_SERVER;
    }
    UNLOCK();
  }
#ifdef DEFAULT_CINEMA_TOKEN
  if (!token.length()) {
    token = DEFAULT_CINEMA_TOKEN;
    token.trim();
  }
#endif
  srv.trim();
  while (srv.endsWith("/")) srv = srv.substring(0, srv.length() - 1);
  if (!srv.length()) return false;

  JsonDocument emoFilter;
  emoFilter["ok"] = true;
  emoFilter["linked"] = true;
  emoFilter["stats"]["w"] = true;
  emoFilter["stats"]["l"] = true;
  emoFilter["pick"]["t"] = true;
  emoFilter["pick"]["g"] = true;
  emoFilter["pick"]["r"] = true;
  JsonObject ef = emoFilter["items"].add<JsonObject>();
  ef["id"] = true; ef["_id"] = true;
  ef["t"] = true; ef["title"] = true;
  ef["y"] = true; ef["release_date"] = true; ef["year"] = true;
  ef["k"] = true; ef["g"] = true; ef["genre"] = true;
  ef["d"] = true; ef["r"] = true; ef["rating"] = true; ef["vote_average"] = true;

  // Separate object filter (/api/wishlist) and array filter (/api/tmdb/*) for ArduinoJson v7
  JsonDocument wishFilter;
  wishFilter["total"] = true;
  JsonObject itemF = wishFilter["data"].add<JsonObject>();
  itemF["_id"] = true;
  itemF["id"] = true;
  itemF["tmdbId"] = true;
  itemF["title"] = true;
  itemF["name"] = true;
  itemF["release_date"] = true;
  itemF["first_air_date"] = true;
  itemF["year"] = true;
  itemF["days"] = true;
  itemF["rating"] = true;
  itemF["vote_average"] = true;
  itemF["type"] = true;
  itemF["media_type"] = true;
  itemF["genre"] = true;

  JsonDocument arrFilter;
  JsonObject arrF = arrFilter.add<JsonObject>();
  arrF["id"] = true;
  arrF["title"] = true;
  arrF["name"] = true;
  arrF["release_date"] = true;
  arrF["first_air_date"] = true;
  arrF["days"] = true;
  arrF["vote_average"] = true;
  arrF["rating"] = true;
  arrF["media_type"] = true;
  arrF["type"] = true;
  arrF["genre"] = true;

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
  bool ok = httpJson(emoUrl, doc, &emoFilter, hName, hVal);
  emoFilter.clear();

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
      if (cd->n >= 5) break;
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
      { LOCK(); latestCinema = *cd; UNLOCK(); }
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
  JsonDocument* fbFilter = endpoint.startsWith("/api/wishlist") ? &wishFilter : &arrFilter;
  if (!httpJson(url, doc, fbFilter, hName, hVal)) {
    if (endpoint.startsWith("/api/wishlist")) {
      url = srv + "/api/tmdb/trending";
      if (!httpJson(url, doc, &arrFilter)) {
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
    if (cd->n >= 5) break;
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
  if (cd->ok) { LOCK(); latestCinema = *cd; UNLOCK(); }
  return cd->ok;
}

// ------------------------------------------------------------------ Redesigned Cinema Screens
static void drawCineHero(CardCtx& c, CinemaData* cd, int curIdx) {
  if (!cd || !cd->n) return;
  int i = (unsigned)curIdx % cd->n;
  const CineItem& m = cd->items[i];

  char hrBuf[12];
  snprintf(hrBuf, sizeof(hrBuf), "%d/%d", i + 1, cd->n);
  drawHeader("CINEWISH", hrBuf);

  // 1. Movie Title (clean, no jittery marquee, max space utilized)
  u8g2.setFont(F_MED);
  int tw = u8g2.getStrWidth(m.title);
  if (tw <= 124) {
    u8g2.drawStr(2, 22, m.title);
  } else {
    u8g2.setFont(F_SMALL);
    drawTextFit(2, 21, 124, m.title);
  }
  u8g2.drawHLine(0, 24, 128);

  // 2. Left Status Badge (Countdown or TMDB Rating)
  if (m.days >= 0) {
    // Upcoming / Premiere countdown
    u8g2.drawRFrame(2, 27, 44, 24, 3);
    if (m.days == 0) {
      u8g2.drawRBox(2, 27, 44, 24, 3);
      u8g2.setDrawColor(0);
      u8g2.setFont(F_SMALL);
      u8g2.drawStr(12, 38, "OUT");
      u8g2.drawStr(6, 47, "TODAY!");
      u8g2.setDrawColor(1);
    } else if (m.days == 1) {
      u8g2.setFont(F_TINY);
      u8g2.drawStr(4, 36, "RELEASING");
      u8g2.setFont(F_SMALL);
      u8g2.drawStr(4, 46, "TOMORROW");
    } else {
      char numBuf[8];
      snprintf(numBuf, sizeof(numBuf), "%d", m.days);
      u8g2.setFont(F_HELV14);
      int nw = u8g2.getStrWidth(numBuf);
      u8g2.drawStr(6, 46, numBuf);
      u8g2.setFont(F_TINY);
      u8g2.drawStr(8 + nw, 36, "DAYS");
      u8g2.drawStr(8 + nw, 46, "LEFT");
    }
  } else {
    // Out Now: TMDB Rating badge
    u8g2.drawRFrame(2, 27, 44, 24, 3);
    u8g2.drawXBMP(6, 31, 7, 7, XBM_STAR7);
    char rBuf[8];
    snprintf(rBuf, sizeof(rBuf), "%.1f", m.rating > 0.05f ? m.rating : 7.5f);
    u8g2.setFont(F_MED);
    u8g2.drawStr(17, 39, rBuf);
    u8g2.setFont(F_TINY);
    u8g2.drawStr(5, 47, "TMDB SCORE");
  }

  // 3. Right Info Block (Media Format, Genre, Date)
  // Format pill [MOVIE] / [SERIES]
  u8g2.setFont(F_TINY);
  u8g2.drawRFrame(50, 27, 26, 10, 2);
  u8g2.drawStr(53, 34, m.isTV ? "SERIES" : "MOVIE");

  // Genre
  cFit(80, 34, 46, m.genre);

  // Release Date
  char dtBuf[20];
  cDateFmt(dtBuf, sizeof(dtBuf), m.date, m.days);
  u8g2.setFont(F_SMALL);
  drawTextFit(50, 46, 76, dtBuf);

  // 4. Bottom Footer & Navigation Dots
  u8g2.drawHLine(0, 53, 128);
  u8g2.setFont(F_TINY);
  if (m.days >= 0) {
    u8g2.drawStr(2, 61, "PREMIERE COUNTDOWN");
  } else if (m.rating >= 8.0f) {
    u8g2.drawStr(2, 61, "TOP RATED");
  } else {
    u8g2.drawStr(2, 61, "IN THEATERS / OTT");
  }

  cDots(i, cd->n, 59);
}

static void drawCineList(CardCtx& c, CinemaData* cd, int sel) {
  if (!cd || !cd->n) {
    drawHeader("WISHLIST", "0 Titles");
    drawCenteredMsg("List is empty", "Add in CineWish");
    return;
  }

  char hrBuf[12];
  snprintf(hrBuf, sizeof(hrBuf), "%d Titles", cd->n);
  drawHeader("WISHLIST", hrBuf);

  int cur = (unsigned)sel % cd->n;
  int first = constrain(cur - 1, 0, max(0, cd->n - 3));

  // 3 perfectly spaced 14px rows (zero overlap)
  for (int r = 0; r < 3 && first + r < cd->n; r++) {
    const CineItem& m = cd->items[first + r];
    int y = 13 + r * 16;
    bool isSel = (first + r == cur);

    if (isSel) {
      u8g2.drawRBox(0, y, 128, 14, 2);
      u8g2.setDrawColor(0);
    }

    // Left score or countdown
    char tagLeft[10];
    if (m.days >= 0) snprintf(tagLeft, sizeof(tagLeft), "%dd", m.days);
    else snprintf(tagLeft, sizeof(tagLeft), "%.1f", m.rating > 0.05f ? m.rating : 7.0f);

    u8g2.setFont(F_TINY);
    u8g2.drawStr(3, y + 10, tagLeft);

    // Title
    u8g2.setFont(F_SMALL);
    cFit(26, y + 10, 76, m.title);

    // Right status badge
    const char* tagRight = (m.days > 0) ? "SOON" : ((m.days == 0) ? "NOW" : "OUT");
    u8g2.setFont(F_TINY);
    int rw = u8g2.getStrWidth(tagRight);
    u8g2.drawStr(125 - rw, y + 10, tagRight);

    u8g2.setDrawColor(1);
  }

  u8g2.drawHLine(0, 61, 128);
}

static void drawCinePick(CardCtx& c, CinemaData* cd) {
  drawHeader("RECOMMENDED", "CineWish");
  if (!cd->pickTitle[0]) {
    drawCenteredMsg("No picks yet", "Add to Wishlist");
    return;
  }

  // Clapper & Popcorn Graphic
  cClapper(3, 16);

  // Title
  u8g2.setFont(F_MED);
  drawTextFit(30, 24, 96, cd->pickTitle);

  // Rating Pill
  u8g2.setFont(F_TINY);
  u8g2.drawRFrame(30, 28, 40, 10, 2);
  u8g2.drawXBMP(33, 30, 7, 7, XBM_STAR7);
  char rBuf[8];
  snprintf(rBuf, sizeof(rBuf), "%.1f", cd->pickRating > 0.05f ? cd->pickRating : 8.5f);
  u8g2.drawStr(43, 36, rBuf);

  // Genre
  cFit(74, 36, 52, cd->pickGenre[0] ? cd->pickGenre : "Featured");

  // Footer banner
  u8g2.drawHLine(0, 48, 128);
  u8g2.setFont(F_SMALL);
  textCenter(59, "Tonight's Must-Watch");
}

static void cinema_enter(CardCtx& c) {
  uint32_t now = millis();
  c.st[0] = CV_HERO;
  c.st[1] = 0;
  c.st[2] = now;
  c.st[3] = now;

  // If memory instance is empty, immediately restore cached latestCinema
  CinemaData* cd = (CinemaData*)c.d;
  if (cd && !cd->n && latestCinema.n > 0) {
    *cd = latestCinema;
  }
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

  const char* forcedView = c.p["view"] | "auto";
  bool isAuto = !strcmp(forcedView, "auto");
  if (!isAuto) {
    if (!strcmp(forcedView, "watchlist") || !strcmp(forcedView, "list")) view = CV_LIST;
    else if (!strcmp(forcedView, "pick")) view = CV_PICK;
    else view = CV_HERO;
  } else {
    // Smooth readable dwell: change movie every 6 seconds
    if (now - tLast > 6000) {
      tLast = now;
      idx = (idx + 1) % (cd->n ? cd->n : 1);
    }
  }

  switch (view % CV_COUNT) {
    case CV_HERO: drawCineHero(c, cd, idx); break;
    case CV_LIST: drawCineList(c, cd, idx); break;
    case CV_PICK: drawCinePick(c, cd);      break;
    default:      drawCineHero(c, cd, idx); break;
  }
}

// Interactive touch gestures:
// Double Tap: switches view mode (Hero card -> List -> Tonight's pick)
// Global single-tap is preserved for next card navigation!
static bool cinema_input(CardCtx& c, uint8_t g) {
  CinemaData* cd = (CinemaData*)c.d;
  if (!cd || !cd->n) return false;

  uint32_t& view = c.st[0];
  uint32_t& tLast = c.st[2];

  if (g == G_DOUBLE) {
    view = (view + 1) % CV_COUNT;
    tLast = millis();
    return true;
  }
  return false;
}

static void cinema_speak(CardCtx& c, String& s) {
  CinemaData* cd = (CinemaData*)c.d;
  if (!cd || !cd->ok || !cd->n) {
    s = "No cinema information available.";
    return;
  }
  uint32_t idx = c.st[1] % cd->n;
  const CineItem& m = cd->items[idx];

  s = String(m.title) + ", ";
  if (m.genre[0]) s += String(m.genre) + ". ";
  if (m.days > 1) {
    s += "Releasing in " + String(m.days) + " days.";
  } else if (m.days == 1) {
    s += "Releasing tomorrow!";
  } else if (m.days == 0) {
    s += "Releasing today in theaters!";
  } else {
    s += "Out now.";
    if (m.rating > 0.05f) {
      char rBuf[24];
      snprintf(rBuf, sizeof(rBuf), " Rated %.1f on TMDB.", m.rating);
      s += rBuf;
    }
  }
}

const CardType CT_cinema = {"cinema", sizeof(CinemaData), 1800, 0, cinema_enter, cinema_draw, cinema_input, cinema_fetch, cinema_speak, nullptr};

// ------------------------------------------------------------------ Package & Delivery Tracker (delivery)
DeliveryData latestDelivery = {false, 0, {}};

void deliverySyncFromState() {
  LOCK();
  latestDelivery.count = 0;
  if (stateDoc["deliveries"].is<JsonArray>()) {
    JsonArray arr = stateDoc["deliveries"].as<JsonArray>();
    for (JsonObject o : arr) {
      if (latestDelivery.count >= 4) break;
      DeliveryItem& it = latestDelivery.items[latestDelivery.count];
      memset(&it, 0, sizeof(it));
      safeCopy(it.id, o["id"] | "", sizeof(it.id));
      safeCopy(it.title, o["item"] | (o["title"] | "Package"), sizeof(it.title));
      safeCopy(it.courier, o["courier"] | "Courier", sizeof(it.courier));
      safeCopy(it.status, o["status"] | "In transit", sizeof(it.status));
      safeCopy(it.eta, o["eta"] | "Soon", sizeof(it.eta));
      safeCopy(it.awb, o["awb"] | "", sizeof(it.awb));
      if (!o["stage"].isNull()) {
        it.stage = (uint8_t)constrain((int)(o["stage"] | 2), 0, 4);
      } else {
        String st = String(it.status);
        st.toLowerCase();
        if (st.indexOf("delivered") >= 0) it.stage = 4;
        else if (st.indexOf("out") >= 0 || (st.indexOf("deliver") >= 0 && st.indexOf("today") >= 0)) it.stage = 3;
        else if (st.indexOf("transit") >= 0 || st.indexOf("reach") >= 0 || st.indexOf("hub") >= 0) it.stage = 2;
        else if (st.indexOf("ship") >= 0 || st.indexOf("dispatch") >= 0) it.stage = 1;
        else if (st.indexOf("order") >= 0 || st.indexOf("place") >= 0) it.stage = 0;
        else it.stage = 2;
      }
      latestDelivery.count++;
    }
  }
  latestDelivery.ok = (latestDelivery.count > 0);
  UNLOCK();
}

bool deliveryAddOrUpdate(const JsonObject& d) {
  LOCK();
  if (!stateDoc["deliveries"].is<JsonArray>()) {
    stateDoc["deliveries"].to<JsonArray>();
  }
  JsonArray arr = stateDoc["deliveries"].as<JsonArray>();

  const char* inId = d["id"] | "";
  const char* inItem = d["item"] | (d["title"] | "");
  const char* inCourier = d["courier"] | "";

  JsonObject target;
  for (JsonObject o : arr) {
    if (inId && *inId && !strcmp(o["id"] | "", inId)) {
      target = o;
      break;
    }
    if (inItem && *inItem && !strcasecmp(o["item"] | (o["title"] | ""), inItem)) {
      target = o;
      break;
    }
  }

  bool isNew = target.isNull();
  if (isNew) {
    if (arr.size() >= 4) {
      arr.remove(0);
    }
    target = arr.add<JsonObject>();
    char genId[16];
    snprintf(genId, sizeof(genId), "d%06lx", (unsigned long)(esp_random() & 0xFFFFFF));
    target["id"] = (inId && *inId) ? inId : genId;
  }

  if (inItem && *inItem) target["item"] = inItem;
  if (inCourier && *inCourier) target["courier"] = inCourier;
  if (!d["status"].isNull()) target["status"] = d["status"].as<const char*>();
  if (!d["eta"].isNull()) target["eta"] = d["eta"].as<const char*>();
  if (!d["awb"].isNull()) target["awb"] = d["awb"].as<const char*>();
  if (!d["stage"].isNull()) target["stage"] = d["stage"].as<int>();
  target["updated"] = (uint32_t)time(nullptr);

  markDirty(DIRTY_STATE);
  UNLOCK();

  deliverySyncFromState();

  String st = String(d["status"] | "");
  st.toLowerCase();
  int stg = d["stage"] | -1;
  if (stg == 3 || st.indexOf("out") >= 0) {
    notify("Delivery Alert", inItem && *inItem ? inItem : "Package", E_HAPPY, "sfx:chime", EV_NOTIFY, true, 8);
  } else if (isNew) {
    toastShow("Package added");
  }

  return true;
}

bool deliveryDelete(const char* id) {
  LOCK();
  if (!stateDoc["deliveries"].is<JsonArray>()) { UNLOCK(); return false; }
  JsonArray arr = stateDoc["deliveries"].as<JsonArray>();
  if (!id || !*id || !strcmp(id, "all")) {
    arr.clear();
  } else if (!strcmp(id, "delivered")) {
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
      int s = arr[i]["stage"] | 0;
      const char* st = arr[i]["status"] | "";
      if (s == 4 || icontains(st, "delivered")) {
        arr.remove(i);
      }
    }
  } else {
    for (int i = 0; i < (int)arr.size(); i++) {
      if (!strcmp(arr[i]["id"] | "", id)) {
        arr.remove(i);
        break;
      }
    }
  }
  markDirty(DIRTY_STATE);
  UNLOCK();
  deliverySyncFromState();
  return true;
}

static void delivery_enter(CardCtx& c) {
  deliverySyncFromState();
}

static void delivery_draw(CardCtx& c) {
  if (latestDelivery.count == 0) {
    drawHeader("DELIVERY", "No Orders");
    int bx = 56, by = 16;
    u8g2.drawRFrame(bx, by, 16, 14, 2);
    u8g2.drawLine(bx, by + 4, bx + 16, by + 4);
    u8g2.drawLine(bx + 8, by + 4, bx + 8, by + 14);
    u8g2.setFont(F_SMALL);
    textCenter(44, "All Caught Up");
    u8g2.setFont(F_TINY);
    textCenter(56, "Add in Web Dashboard");
    return;
  }

  int idx = (latestDelivery.count > 0) ? ((millis() - c.t0) / 6000) % latestDelivery.count : 0;
  const DeliveryItem& it = latestDelivery.items[idx];

  char cntBuf[16];
  snprintf(cntBuf, sizeof(cntBuf), "%d/%d", idx + 1, latestDelivery.count);
  drawHeader("DELIVERY", cntBuf);

  // 1. Courier Tag (Pill)
  char tag[10];
  const char* cour = it.courier;
  if (icontains(cour, "amazon")) safeCopy(tag, "AMZN", sizeof(tag));
  else if (icontains(cour, "flipkart")) safeCopy(tag, "FLPK", sizeof(tag));
  else if (icontains(cour, "ekart")) safeCopy(tag, "EKART", sizeof(tag));
  else if (icontains(cour, "delhivery")) safeCopy(tag, "DLHV", sizeof(tag));
  else if (icontains(cour, "post")) safeCopy(tag, "POST", sizeof(tag));
  else if (icontains(cour, "blue")) safeCopy(tag, "BLUEDT", sizeof(tag));
  else {
    safeCopy(tag, cour, sizeof(tag));
    for (int i = 0; tag[i]; i++) tag[i] = toupper((unsigned char)tag[i]);
  }

  u8g2.setFont(F_TINY);
  int tagW = u8g2.getStrWidth(tag) + 6;
  if (tagW > 40) tagW = 40;
  u8g2.drawRFrame(0, 12, tagW, 10, 2);
  u8g2.drawStr(3, 19, tag);

  // 2. Item Title
  u8g2.setFont(F_SMALL);
  drawTextFit(tagW + 4, 20, 128 - (tagW + 4), it.title);

  // 3. 4-Stage Visual Timeline
  const int dotX[4] = {16, 48, 80, 112};
  const int trackY = 32;
  u8g2.drawLine(dotX[0], trackY, dotX[3], trackY);

  int stg = constrain((int)it.stage, 0, 4);
  int activeDot = (stg >= 4) ? 3 : (stg < 0 ? 0 : stg);
  if (activeDot > 0) {
    u8g2.drawLine(dotX[0], trackY - 1, dotX[activeDot], trackY - 1);
    u8g2.drawLine(dotX[0], trackY + 1, dotX[activeDot], trackY + 1);
  }

  for (int s = 0; s < 4; s++) {
    int x = dotX[s];
    if (s < activeDot) {
      u8g2.drawDisc(x, trackY, 3);
    } else if (s == activeDot) {
      u8g2.setDrawColor(0);
      u8g2.drawDisc(x, trackY, 5);
      u8g2.setDrawColor(1);
      u8g2.drawCircle(x, trackY, 4);
      u8g2.drawDisc(x, trackY, 2);
    } else {
      u8g2.setDrawColor(0);
      u8g2.drawDisc(x, trackY, 3);
      u8g2.setDrawColor(1);
      u8g2.drawCircle(x, trackY, 3);
    }
  }

  // 4. Status & ETA Banner
  if (stg == 3) {
    // Out for delivery: inverted banner
    u8g2.drawRBox(0, 45, 128, 18, 3);
    u8g2.setDrawColor(0);
    u8g2.setFont(F_SMALL);
    textCenter(54, "Out for Delivery!");
    u8g2.setFont(F_TINY);
    if (it.eta[0]) textCenter(61, it.eta);
    u8g2.setDrawColor(1);
  } else if (stg == 4) {
    u8g2.drawRFrame(0, 45, 128, 18, 3);
    u8g2.setFont(F_SMALL);
    textCenter(54, "Delivered");
    u8g2.setFont(F_TINY);
    if (it.eta[0]) textCenter(61, it.eta);
  } else {
    u8g2.drawRFrame(0, 45, 128, 18, 3);
    u8g2.setFont(F_SMALL);
    char stBuf[32];
    safeCopy(stBuf, it.status[0] ? it.status : (stg == 1 ? "Shipped" : "In Transit"), sizeof(stBuf));
    drawTextFit(4, 54, 66, stBuf);
    u8g2.setFont(F_TINY);
    if (it.eta[0]) {
      char etaBuf[32];
      snprintf(etaBuf, sizeof(etaBuf), "ETA: %s", it.eta);
      int ew = u8g2.getStrWidth(etaBuf);
      u8g2.drawStr(124 - ew, 55, etaBuf);
    }
  }
}

static bool delivery_input(CardCtx& c, uint8_t g) {
  // Always keep global touch gestures! Do not consume G_TAP so navigation never locks!
  return false;
}

static bool delivery_fetch(JsonObject p, void* d) {
  deliverySyncFromState();
  String url = p["url"].is<const char*>() ? (p["url"] | "") : "";
  if (!url.length()) {
    LOCK();
    if (cfg["delivery"].is<JsonObject>() && cfg["delivery"]["url"].is<const char*>()) {
      url = cfg["delivery"]["url"] | "";
    } else if (cfg["delivery_url"].is<const char*>()) {
      url = cfg["delivery_url"] | "";
    }
    UNLOCK();
  }
  url.trim();
  if (url.length()) {
    JsonDocument doc;
    if (httpJson(url, doc)) {
      if (doc.is<JsonObject>()) {
        deliveryAddOrUpdate(doc.as<JsonObject>());
      } else if (doc.is<JsonArray>()) {
        for (JsonObject o : doc.as<JsonArray>()) {
          deliveryAddOrUpdate(o);
        }
      }
    }
  }
  return true;
}

static void delivery_speak(CardCtx& c, String& s) {
  if (latestDelivery.count == 0) {
    s = "You have no active deliveries.";
    return;
  }
  int idx = (latestDelivery.count > 0) ? ((millis() - c.t0) / 6000) % latestDelivery.count : 0;
  const DeliveryItem& it = latestDelivery.items[idx];
  s = String("Your ") + it.courier + " order, " + it.title + ", is " + it.status + ". Expected " + it.eta + ".";
}

const CardType CT_delivery = {"delivery", sizeof(DeliveryData), 300, 0, delivery_enter, delivery_draw, delivery_input, delivery_fetch, delivery_speak, nullptr};

// ================================================================== boxoffice (Real-Time Movie Collections)
BoxOfficeData latestBoxOffice = {false, 0, {}, "₹"};

static bool isBoxOfficeRemoved(const char* title) {
  if (!title || !*title) return false;
  if (!stateDoc["boxoffice_removed"].is<JsonArray>()) return false;
  JsonArray rem = stateDoc["boxoffice_removed"].as<JsonArray>();
  for (JsonVariant v : rem) {
    const char* r = v.as<const char*>();
    if (r && (icontains(title, r) || icontains(r, title))) return true;
  }
  return false;
}

void boxofficeSyncFromState() {
  LOCK();
  if (!stateDoc["boxoffice"].is<JsonArray>()) {
    stateDoc["boxoffice"].to<JsonArray>();
    markDirty(DIRTY_STATE);
  }
  JsonArray arr = stateDoc["boxoffice"].as<JsonArray>();

  // If any movie in the list was marked removed, purge it immediately
  if (stateDoc["boxoffice_removed"].is<JsonArray>()) {
    JsonArray rem = stateDoc["boxoffice_removed"].as<JsonArray>();
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
      const char* t = arr[i]["title"] | arr[i]["movie"] | "";
      for (JsonVariant v : rem) {
        const char* r = v.as<const char*>();
        if (r && (icontains(t, r) || icontains(r, t))) {
          arr.remove(i);
          markDirty(DIRTY_STATE);
          break;
        }
      }
    }
  }

  // Clear any legacy dummy or stale entries so fresh live collections display cleanly
  if (arr.size() == 1 && strcmp(arr[0]["title"] | "", "Kalki 2898 AD") == 0 && strcmp(arr[0]["today"] | "", "34.5 Cr") == 0) {
    arr.clear();
    markDirty(DIRTY_STATE);
  }
  for (int i = (int)arr.size() - 1; i >= 0; i--) {
    const char* tot = arr[i]["total"] | "";
    if (strstr(tot, "947.25") != nullptr) {
      arr.remove(i);
      markDirty(DIRTY_STATE);
    }
  }
  latestBoxOffice.count = 0;
  for (JsonObject o : arr) {
    if (latestBoxOffice.count >= 5) break;
    BoxOfficeItem& it = latestBoxOffice.items[latestBoxOffice.count];
    safeCopy(it.title, o["title"] | o["movie"] | o["name"] | "", sizeof(it.title));
    safeCopy(it.day, o["day"] | o["status"] | "Today", sizeof(it.day));
    safeCopy(it.today, o["today"] | o["daily"] | o["day_gross"] | "-", sizeof(it.today));
    safeCopy(it.total, o["total"] | o["worldwide"] | o["gross"] | o["ww"] | "-", sizeof(it.total));
    safeCopy(it.net, o["net"] | o["domestic"] | o["india"] | "", sizeof(it.net));
    safeCopy(it.verdict, o["verdict"] | o["trend"] | o["tag"] | "Trending", sizeof(it.verdict));
    it.updatedAt = o["updated"] | (uint32_t)time(nullptr);
    latestBoxOffice.count++;
  }
  latestBoxOffice.ok = (latestBoxOffice.count > 0);
  UNLOCK();
}

bool boxofficeAddOrUpdate(const JsonObject& d, bool forceUser) {
  const char* title = d["title"] | d["movie"] | d["name"] | "";
  if (!title || !*title) return false;
  LOCK();

  // If not explicitly added by user, skip if user previously removed this movie
  if (!forceUser && isBoxOfficeRemoved(title)) {
    UNLOCK();
    return false;
  }

  // If user explicitly added it, remove from blacklist
  if (forceUser && stateDoc["boxoffice_removed"].is<JsonArray>()) {
    JsonArray rem = stateDoc["boxoffice_removed"].as<JsonArray>();
    for (int i = (int)rem.size() - 1; i >= 0; i--) {
      const char* r = rem[i].as<const char*>();
      if (r && (icontains(title, r) || icontains(r, title))) {
        rem.remove(i);
      }
    }
  }

  if (!stateDoc["boxoffice"].is<JsonArray>()) {
    stateDoc["boxoffice"].to<JsonArray>();
  }
  JsonArray arr = stateDoc["boxoffice"].as<JsonArray>();
  JsonObject target;
  for (JsonObject o : arr) {
    if (icontains(o["title"] | "", title) || icontains(title, o["title"] | "")) {
      target = o;
      break;
    }
  }
  if (target.isNull()) {
    if (arr.size() >= 5) arr.remove(0);
    target = arr.add<JsonObject>();
  }
  target["title"] = title;
  if (!d["day"].isNull()) target["day"] = d["day"];
  if (!d["today"].isNull()) target["today"] = d["today"];
  else if (!d["daily"].isNull()) target["today"] = d["daily"];
  else if (!d["day_gross"].isNull()) target["today"] = d["day_gross"];

  if (!d["total"].isNull()) target["total"] = d["total"];
  else if (!d["worldwide"].isNull()) target["total"] = d["worldwide"];
  else if (!d["gross"].isNull()) target["total"] = d["gross"];
  else if (!d["ww"].isNull()) target["total"] = d["ww"];

  if (!d["net"].isNull()) target["net"] = d["net"];
  else if (!d["domestic"].isNull()) target["net"] = d["domestic"];
  else if (!d["india"].isNull()) target["net"] = d["india"];

  if (!d["verdict"].isNull()) target["verdict"] = d["verdict"];
  else if (!d["trend"].isNull()) target["verdict"] = d["trend"];
  target["updated"] = (uint32_t)time(nullptr);
  markDirty(DIRTY_STATE);
  UNLOCK();
  boxofficeSyncFromState();
  return true;
}

bool boxofficeDelete(const char* title) {
  LOCK();
  if (!stateDoc["boxoffice"].is<JsonArray>()) {
    stateDoc["boxoffice"].to<JsonArray>();
  }
  if (!stateDoc["boxoffice_removed"].is<JsonArray>()) {
    stateDoc["boxoffice_removed"].to<JsonArray>();
  }
  JsonArray arr = stateDoc["boxoffice"].as<JsonArray>();
  JsonArray rem = stateDoc["boxoffice_removed"].as<JsonArray>();

  if (!title || !*title || !strcasecmp(title, "all")) {
    arr.clear();
    rem.clear();
  } else {
    // Add to removed blacklist so background sync never brings it back
    bool alreadyRem = false;
    for (JsonVariant v : rem) {
      const char* r = v.as<const char*>();
      if (r && (icontains(title, r) || icontains(r, title))) {
        alreadyRem = true;
        break;
      }
    }
    if (!alreadyRem) {
      rem.add(title);
    }

    // Remove matching movie from active list
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
      const char* t = arr[i]["title"] | arr[i]["movie"] | "";
      if (icontains(t, title) || icontains(title, t) || !strcasecmp(t, title)) {
        arr.remove(i);
      }
    }
  }
  markDirty(DIRTY_STATE);
  UNLOCK();
  boxofficeSyncFromState();
  return true;
}

static void boxoffice_enter(CardCtx& c) {
  boxofficeSyncFromState();
}

static void boxoffice_draw(CardCtx& c) {
  boxofficeSyncFromState();
  if (latestBoxOffice.count == 0) {
    drawHeader("BOX OFFICE", "No Data");
    u8g2.setFont(F_SMALL);
    textCenter(34, "No Tracked Movies");
    u8g2.setFont(F_TINY);
    textCenter(48, "Update in Web Dashboard");
    return;
  }

  int idx = ((millis() - c.t0) / 6000) % latestBoxOffice.count;
  const BoxOfficeItem& it = latestBoxOffice.items[idx];

  // 1. TOP HEADER: [Film Icon] [MOVIE TITLE in F_BOLD] ... [DAY BADGE]
  int rightEdge = 127 - displayBatReserve();
  char dayBuf[16];
  if (it.day[0]) safeCopy(dayBuf, it.day, sizeof(dayBuf));
  else snprintf(dayBuf, sizeof(dayBuf), "%d/%d", idx + 1, latestBoxOffice.count);

  u8g2.setFont(F_TINY);
  int dw = u8g2.getUTF8Width(dayBuf);
  int bw = dw + 6;
  int bx = rightEdge - bw;

  // Film clapper icon
  u8g2.drawXBMP(2, 2, 7, 7, XBM_FILM7);

  // Movie Title in bold, large font
  int maxTitleW = max(24, bx - 14);
  u8g2.setFont(F_BOLD);
  if (u8g2.getUTF8Width(it.title) > maxTitleW) {
    u8g2.setFont(F_MED);
    if (u8g2.getUTF8Width(it.title) > maxTitleW) {
      u8g2.setFont(F_SMALL);
    }
  }
  drawTextFit(11, 10, maxTitleW, it.title);

  // Day Badge on top-right (filled rounded capsule)
  u8g2.setFont(F_TINY);
  drawPill(bx, 1, bw, 9, dayBuf, true);

  // Header separator
  u8g2.drawHLine(0, 12, 128);

  // 2. HERO SECTION: Big Worldwide Gross (or Today if WW unavailable)
  bool hasWW = (it.total[0] && strcmp(it.total, "-") != 0 && strcmp(it.total, "0") != 0);
  const char* heroVal = hasWW ? it.total : it.today;
  const char* heroLbl = hasWW ? "WORLDWIDE GROSS" : "TODAY'S COLLECTION";

  u8g2.setFont(F_TINY);
  u8g2.drawStr(2, 21, heroLbl);

  // BIG HERO NUMBER in F_HELV14 (14px tall Helvetica Bold)
  u8g2.setFont(F_HELV14);
  int heroW = u8g2.getUTF8Width(heroVal);
  if (heroW > 124) {
    u8g2.setFont(F_BOLD);
    heroW = u8g2.getUTF8Width(heroVal);
    if (heroW > 124) {
      u8g2.setFont(F_MED);
    }
  }
  drawTextFit(2, 36, 124, heroVal);

  // Subtle separator
  drawDottedHLine(2, 39, 124);

  // 3. SECONDARY METRICS: Today Gross + India/Domestic Net
  char todayBuf[32] = "";
  if (hasWW && it.today[0] && strcmp(it.today, "-") != 0) {
    snprintf(todayBuf, sizeof(todayBuf), "Today: %s", it.today);
  }

  u8g2.setFont(F_MED);
  if (todayBuf[0]) {
    if (u8g2.getUTF8Width(todayBuf) > 74) u8g2.setFont(F_SMALL);
    drawTextFit(2, 49, 74, todayBuf);
  }

  if (it.net[0]) {
    u8g2.setFont(F_SMALL);
    drawTextRightFit(latestBoxOffice.count > 1 ? 114 : 126, 49, 50, it.net);
  }

  // 4. FOOTER: Verdict pill + Page Dots
  if (it.verdict[0]) {
    u8g2.setFont(F_TINY);
    int tw = u8g2.getUTF8Width(it.verdict);
    int pw = constrain(tw + 8, 20, 75);
    drawPill(2, 53, pw, 10, it.verdict, false);
  }

  if (latestBoxOffice.count > 1) {
    drawPageDots(126, 58, latestBoxOffice.count, idx);
  }
}

static bool boxoffice_input(CardCtx& c, uint8_t g) {
  // Always keep global touch gestures for every card!
  // Single-tap (G_TAP) is never consumed so it advances to next card globally.
  if (g == G_DOUBLE && latestBoxOffice.count > 1) {
    c.t0 = millis();
    return true;
  }
  return false;
}

static bool boxoffice_fetch(JsonObject p, void* d) {
  boxofficeSyncFromState();
  String url = p["url"].is<const char*>() ? (p["url"] | "") : "";
  String movie = p["movie"].is<const char*>() ? (p["movie"] | "") : "";

  if (!url.length()) {
    LOCK();
    if (cfg["boxoffice"].is<JsonObject>()) {
      if (cfg["boxoffice"]["url"].is<const char*>()) url = cfg["boxoffice"]["url"] | "";
      if (!movie.length() && cfg["boxoffice"]["movie"].is<const char*>()) {
        movie = cfg["boxoffice"]["movie"] | "";
      }
    } else if (cfg["boxoffice_url"].is<const char*>()) {
      url = cfg["boxoffice_url"] | "";
    }
    UNLOCK();
  }
  if (!url.length()) {
    url = String(DEFAULT_SPORTS_SERVER) + "/api/emo/boxoffice";
  }
  url.trim();
  movie.trim();

  if (movie.length()) {
    if (url.indexOf('?') >= 0) url += "&movie=";
    else url += "?movie=";
    for (size_t i = 0; i < movie.length(); i++) {
      char c = movie[i];
      if (c == ' ') url += "%20";
      else url += c;
    }
  }

  if (url.length()) {
    JsonDocument doc;
    if (httpJson(url, doc)) {
      if (doc.is<JsonObject>()) {
        if (doc["movies"].is<JsonArray>()) {
          JsonArray arr = doc["movies"].as<JsonArray>();
          for (JsonObject m : arr) {
            const char* t = m["title"] | m["movie"] | "";
            if (!isBoxOfficeRemoved(t)) {
              boxofficeAddOrUpdate(m, false);
            }
          }
        } else if (doc["title"].is<const char*>()) {
          const char* t = doc["title"] | "";
          if (!isBoxOfficeRemoved(t)) {
            boxofficeAddOrUpdate(doc.as<JsonObject>(), false);
          }
        }
      } else if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject o : arr) {
          const char* t = o["title"] | o["movie"] | "";
          if (!isBoxOfficeRemoved(t)) {
            boxofficeAddOrUpdate(o, false);
          }
        }
      }
    }
  }
  return true;
}

static void boxoffice_speak(CardCtx& c, String& s) {
  if (latestBoxOffice.count == 0) {
    s = "No movie box office collections available.";
    return;
  }
  int idx = ((millis() - c.t0) / 6000) % latestBoxOffice.count;
  const BoxOfficeItem& it = latestBoxOffice.items[idx];
  s = String("Movie ") + it.title + " has collected " + it.today + " today, with total worldwide collection reaching " + it.total + ".";
}

const CardType CT_boxoffice = {"boxoffice", sizeof(BoxOfficeData), 300, 0, boxoffice_enter, boxoffice_draw, boxoffice_input, boxoffice_fetch, boxoffice_speak, nullptr};
