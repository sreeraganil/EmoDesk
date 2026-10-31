#include "cards.h"

static bool needTime(struct tm& t) {
  if (nowTm(t)) return true;
  drawCenteredMsg("Syncing time...", netOnline() ? "waiting for NTP" : "needs Wi-Fi once");
  return false;
}

static void speakTime(CardCtx&, String& s) {
  struct tm t;
  if (!nowTm(t)) { s = "I do not know the time yet."; return; }
  s = "It is " + sayTime(t.tm_hour, t.tm_min) + ". " + WDAY_LONG[t.tm_wday] + ", " + MONTH_LONG[t.tm_mon] + " " +
      numWords(t.tm_mday) + ".";
}

static int daysInMonth(int y, int m) {   // m 0..11
  static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return d[m];
}

// ================================================================== emo
static void emo_draw(CardCtx& c) {
  if (inputHeldMs() > 600) emoPetting();
  emoRender(64, 30, 1.0f);
}
static bool emo_input(CardCtx& c, uint8_t g) {
  if (g == G_TRIPLE) { emoPoke(2); return true; }
  if (g == G_LONG) { emoPlay(); return true; }   // petting happens while holding
  return false;   // G_TAP -> Next Card, G_DOUBLE -> Prev Card
}
static void emo_speak(CardCtx&, String& s) { s = emoSpeech(); }
const CardType CT_emo = {"emo", 0, 0, 0, nullptr, emo_draw, emo_input, nullptr, emo_speak, nullptr};

// ================================================================== pet
static void pet_draw(CardCtx& c) {
  drawTitle(c, "Pet Care", "virtual pet");
  emoRender(20, 36, 0.45f);
  u8g2.setFont(F_TINY);
  static const char* const lab[3] = {"JOY", "NRG", "FED"};
  float v[3] = {pet.happy, pet.energy, 100 - pet.hunger};
  for (int i = 0; i < 3; i++) {
    int y = 14 + i * 13;
    u8g2.drawStr(44, y + 7, lab[i]);
    drawBar(64, y, 60, 8, v[i] / 100.0f);
  }
  u8g2.setFont(F_TINY);
  textCenter(62, "tap: next   |   hold: care");
}
static bool pet_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) { emoFeed(); emoPlay(); return true; }
  return false;
}
const CardType CT_pet = {"pet", 0, 0, 0, nullptr, pet_draw, pet_input, nullptr, emo_speak, nullptr};

// ================================================================== digital clock
static void clock_draw(CardCtx& c) {
  struct tm t;
  int bx = burnX, by = burnY;
  if (!nowTm(t)) {
    u8g2.setFont(F_NUM32);
    textCenter(38 + by, "--:--");
    u8g2.setFont(F_MED);
    textCenter(56 + by, netOnline() ? "Getting the time..." : "Waiting for Wi-Fi");
    return;
  }
  const char* style = c.p["style"] | "big";
  bool h24 = cfgH24();
  char hm[8], date[32];
  fmtHM(hm, t.tm_hour, t.tm_min, h24);
  snprintf(date, sizeof(date), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
  const char* ap = t.tm_hour < 12 ? "AM" : "PM";

  if (!strcmp(style, "minimal")) {
    u8g2.setFont(F_NUM42);
    int w = u8g2.getStrWidth(hm);
    u8g2.drawStr((128 - w) / 2 + bx, 48 + by, hm);
  } else if (!strcmp(style, "seconds")) {
    char s[4];
    snprintf(s, sizeof(s), ":%02d", t.tm_sec);
    u8g2.setFont(F_NUM24);
    int w = u8g2.getStrWidth(hm);
    u8g2.setFont(F_NUM16);
    int ws = u8g2.getStrWidth(s);
    int x0 = (128 - w - ws - 2) / 2 + bx;
    u8g2.setFont(F_NUM24);
    u8g2.drawStr(x0, 38 + by, hm);
    u8g2.setFont(F_NUM16);
    u8g2.drawStr(x0 + w + 2, 38 + by, s);
    if (!h24) {
      u8g2.setFont(F_SMALL);
      u8g2.drawStr(x0 + w + ws + 4, 20 + by, t.tm_hour < 12 ? "am" : "pm");
    }
    drawDottedHLine(14, 46 + by, 100);
    u8g2.setFont(F_MED);
    textCenter(59 + by, date);
  } else if (!strcmp(style, "date")) {
    u8g2.setFont(F_NUM24);
    int w = u8g2.getStrWidth(hm);
    u8g2.drawStr((128 - w) / 2 + bx, 28 + by, hm);
    u8g2.setFont(F_HELV);
    textCenter(45 + by, WDAY_LONG[t.tm_wday]);
    char d2[28];
    snprintf(d2, sizeof(d2), "%d %s %d", t.tm_mday, MONTH_LONG[t.tm_mon], t.tm_year + 1900);
    u8g2.setFont(F_MED);
    textCenter(60 + by, d2);
  } else {
    // v3.O Big Clock Style (Sweeping hairline seconds, weather corner, guaranteed zero AM/PM overlap)
    int h = t.tm_hour;
    if (!h24) { h %= 12; if (!h) h = 12; }
    char b[8];
    snprintf(b, sizeof(b), h24 ? "%02d:%02d" : "%d:%02d", h, t.tm_min);

    u8g2.setFont(F_NUM42);
    int w = u8g2.getStrWidth(b);

    if (!h24) {
      const char* apTag = t.tm_hour < 12 ? "am" : "pm";
      u8g2.setFont(F_SMALL);
      int apW = u8g2.getStrWidth(apTag);
      int gap = 5;
      int totalW = w + gap + apW;
      int x = (128 - totalW) / 2 + bx;
      if (x < 2) x = 2;

      // Draw large time digits
      u8g2.setFont(F_NUM42);
      u8g2.drawStr(x, 46 + by, b);

      // Draw am/pm superscript with guaranteed clearance (never overlaps)
      int apX = x + w + gap;
      if (apX + apW > 126) apX = 126 - apW;
      u8g2.setFont(F_SMALL);
      u8g2.drawStr(apX, 16 + by, apTag);
    } else {
      u8g2.drawStr((128 - w) / 2 + bx, 46 + by, b);
    }

    // Hairline seconds bar sweeping under digits (0 to 127 px)
    int sw = (t.tm_sec * 128) / 60;
    if (sw > 0) u8g2.drawHLine(0, 50 + by, sw);

    // Full date string centered at bottom (e.g. "Saturday 31 Oct")
    char fullDate[32];
    snprintf(fullDate, sizeof(fullDate), "%s %d %s", WDAY_LONG[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
    u8g2.setFont(F_MED);
    if (u8g2.getStrWidth(fullDate) > 124) {
      snprintf(fullDate, sizeof(fullDate), "%s, %d %s", WDAY[t.tm_wday], t.tm_mday, MONTH[t.tm_mon]);
    }
    textCenter(62 + by, fullDate);

    // Corner extra: live temperature if weather is synced
    if (homeW.ok) {
      char tb[10];
      snprintf(tb, sizeof(tb), "%d°", (int)roundf(homeW.temp));
      u8g2.setFont(F_TINY);
      u8g2.drawUTF8(1 + bx, 6 + by, tb);
    }
  }
}
const CardType CT_clock = {"clock", 0, 0, 0, nullptr, clock_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== analog clock
static void hand(int cx, int cy, float deg, int len, bool thick) {
  float a = (deg - 90) * PI / 180.0f;
  int x = cx + (int)(cosf(a) * len), y = cy + (int)(sinf(a) * len);
  u8g2.drawLine(cx, cy, x, y);
  if (thick) { u8g2.drawLine(cx + 1, cy, x + 1, y); u8g2.drawLine(cx, cy + 1, x, y + 1); }
}
static void analog_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  int cx = 32 + burnX, cy = 32 + burnY, r = 30;
  u8g2.drawCircle(cx, cy, r);
  for (int i = 0; i < 12; i++) {
    float a = i * 30 * PI / 180.0f;
    int len = i % 3 == 0 ? 5 : 2;
    u8g2.drawLine(cx + sinf(a) * (r - len), cy - cosf(a) * (r - len), cx + sinf(a) * (r - 1), cy - cosf(a) * (r - 1));
  }
  float s = t.tm_sec, m = t.tm_min + s / 60.0f, h = (t.tm_hour % 12) + m / 60.0f;
  hand(cx, cy, h * 30, 15, true);
  hand(cx, cy, m * 6, 23, true);
  hand(cx, cy, s * 6, 27, false);
  u8g2.drawDisc(cx, cy, 2);
  u8g2.setFont(F_HELV14);
  u8g2.drawStr(72, 22, WDAY[t.tm_wday]);
  char b[16];
  snprintf(b, sizeof(b), "%d %s", t.tm_mday, MONTH[t.tm_mon]);
  u8g2.setFont(F_MED);
  u8g2.drawStr(72, 38, b);
  fmtHM(b, t.tm_hour, t.tm_min, cfgH24());
  u8g2.drawStr(72, 54, b);
}
const CardType CT_analog = {"analog", 0, 0, 0, nullptr, analog_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== binary clock
static void binary_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  int digits[6] = {t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10, t.tm_sec / 10, t.tm_sec % 10};
  int bits[6] = {2, 4, 3, 4, 3, 4};
  static const char* const lab = "HHMMSS";
  for (int col = 0; col < 6; col++) {
    int x = 14 + col * 18 + (col / 2) * 6 + burnX;
    for (int b = 0; b < 4; b++) {
      int y = 50 - b * 12 + burnY;
      if (b >= bits[col]) continue;
      if (digits[col] & (1 << b)) u8g2.drawDisc(x, y, 4);
      else u8g2.drawCircle(x, y, 4);
    }
    char l[2] = {lab[col], 0};
    u8g2.setFont(F_TINY);
    u8g2.drawStr(x - 1, 63, l);
  }
}
const CardType CT_binary = {"binary", 0, 0, 0, nullptr, binary_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== word clock
static void wordclock_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  static const char* const H[] = {"twelve", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven"};
  static const char* const M[] = {"", "five past", "ten past", "quarter past", "twenty past", "twenty-five past", "half past",
                                  "twenty-five to", "twenty to", "quarter to", "ten to", "five to"};
  int idx = ((t.tm_min + 2) / 5) % 12;
  int h = t.tm_hour + ((t.tm_min + 2) / 5 >= 12 ? 1 : 0) + (idx > 6 ? 1 : 0);
  char s[64];
  if (idx == 0) snprintf(s, sizeof(s), "It's %s o'clock", H[h % 12]);
  else snprintf(s, sizeof(s), "It's %s %s", M[idx], H[h % 12]);
  u8g2.setFont(F_HELV14);
  int lines = wrapText(0, 0, 120, 17, s, 0);   // count only
  int y0 = 32 - lines * 17 / 2 + 13;
  wrapText(4 + burnX, y0 + burnY, 120, 17, s, 4);
}
const CardType CT_wordclock = {"wordclock", 0, 0, 0, nullptr, wordclock_draw, nullptr, nullptr, speakTime, nullptr};

// ================================================================== calendar
static void calendar_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "%s %d", MONTH_LONG[t.tm_mon], t.tm_year + 1900);
  u8g2.setFont(F_SMALL);
  textCenter(7, hdr);
  bool mon = cfg["time"]["weekMon"] | true;
  const char* dn = mon ? "MoTuWeThFrSaSu" : "SuMoTuWeThFrSa";
  u8g2.setFont(F_TINY);
  for (int i = 0; i < 7; i++) {
    char b[3] = {dn[i * 2], dn[i * 2 + 1], 0};
    u8g2.drawStr(4 + i * 18, 15, b);
  }
  int firstW = ((t.tm_wday - (t.tm_mday - 1)) % 7 + 7) % 7;
  int off = mon ? (firstW + 6) % 7 : firstW;
  int dim = daysInMonth(t.tm_year + 1900, t.tm_mon);
  u8g2.setFont(F_SMALL);
  for (int d = 1; d <= dim; d++) {
    int pos = off + d - 1, row = pos / 7, col = pos % 7;
    int x = 1 + col * 18, y = 23 + row * 8;
    if (y > 63) break;
    char b[3];
    snprintf(b, sizeof(b), "%d", d);
    if (d == t.tm_mday) { u8g2.drawBox(x, y - 7, 16, 8); u8g2.setDrawColor(0); }
    u8g2.drawStr(x + (d < 10 ? 6 : 3), y, b);
    u8g2.setDrawColor(1);
  }
}
static void calendar_speak(CardCtx&, String& s) {
  struct tm t;
  if (!nowTm(t)) return;
  s = "Today is " + String(WDAY_LONG[t.tm_wday]) + ", " + MONTH_LONG[t.tm_mon] + " " + numWords(t.tm_mday) + ", " +
      numWords(t.tm_year + 1900) + ".";
}
const CardType CT_calendar = {"calendar", 0, 0, 0, nullptr, calendar_draw, nullptr, nullptr, calendar_speak, nullptr};

// ================================================================== world clock
static void world_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  JsonArray a = c.p["cities"];
  if (a.isNull() || !a.size()) { drawCenteredMsg("No cities yet", "Add them in the dashboard"); return; }
  static struct tm cache[4];
  static uint32_t calcAt = 0;
  static int calcIdx = -1;
  int n = min((int)a.size(), 4);
  if (millis() - calcAt > 1000 || calcIdx != c.idx) {
    calcAt = millis();
    calcIdx = c.idx;
    time_t now = time(nullptr);
    for (int i = 0; i < n; i++) localIn(a[i]["tz"] | "UTC0", now, cache[i]);
  }
  int rowH = 64 / n;
  bool h24 = cfgH24();
  for (int i = 0; i < n; i++) {
    int y = i * rowH + rowH / 2 + 5;
    u8g2.setFont(F_MED);
    u8g2.drawUTF8(2, y, a[i]["n"] | "?");
    char hm[8];
    fmtHM(hm, cache[i].tm_hour, cache[i].tm_min, h24);
    u8g2.setFont(F_BOLD);
    textRight(126, y + 1, hm);
    int dd = cache[i].tm_yday - t.tm_yday;
    if (dd) {
      u8g2.setFont(F_TINY);
      u8g2.drawStr(126 - u8g2.getStrWidth("+1") - 44, y - 3, dd > 0 ? "+1" : "-1");
    }
    if (i) u8g2.drawHLine(0, i * rowH, 128);
  }
}
const CardType CT_world = {"world", 0, 0, 0, nullptr, world_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== progress
static float progs[4];
static void calcProgress() {
  struct tm t;
  if (!nowTm(t)) return;
  int y = t.tm_year + 1900;
  float min = t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.0f;
  int diy = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
  progs[0] = min / 1440.0f;
  progs[1] = (((t.tm_wday + 6) % 7) * 1440 + min) / (7 * 1440.0f);
  progs[2] = ((t.tm_mday - 1) * 1440 + min) / (daysInMonth(y, t.tm_mon) * 1440.0f);
  progs[3] = (t.tm_yday * 1440 + min) / (diy * 1440.0f);
}
static void progress_draw(CardCtx& c) {
  struct tm t;
  if (!needTime(t)) return;
  calcProgress();
  static const char* const L[] = {"Day", "Week", "Month", "Year"};
  for (int i = 0; i < 4; i++) {
    int y0 = 2 + i * 16;
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(0, y0 + 8, L[i]);
    drawBar(32, y0, 70, 11, progs[i]);
    char b[6];
    snprintf(b, sizeof(b), "%d%%", (int)(progs[i] * 100));
    textRight(127, y0 + 8, b);
  }
}
static void progress_speak(CardCtx&, String& s) {
  calcProgress();
  s = "The day is " + String((int)(progs[0] * 100)) + " percent done. The year is " + String((int)(progs[3] * 100)) +
      " percent done.";
}
const CardType CT_progress = {"progress", 0, 0, 0, nullptr, progress_draw, nullptr, nullptr, progress_speak, nullptr};

// ================================================================== sun & moon
static float moonAge() {
  double days = (time(nullptr) - 947182440.0) / 86400.0;   // since new moon 2000-01-06 18:14 UTC
  double a = fmod(days, 29.530588853);
  return a < 0 ? a + 29.530588853 : a;
}
static const char* moonName(float age) {
  static const char* const N[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous", "Full moon",
                                  "Waning gibbous", "Last quarter", "Waning crescent"};
  return N[(int)((age / 29.530588853f) * 8 + 0.5f) % 8];
}
static void sunmoon_draw(CardCtx& c) {
  float age = moonAge(), p = age / 29.530588853f;
  int cx = 104, cy = 26, r = 14;
  for (int y = -r; y <= r; y++) {
    float w = sqrtf(r * r - y * y);
    float k = cosf(2 * PI * p);
    int x0, x1;
    if (p < 0.5f) { x0 = (int)(w * k); x1 = (int)w; }
    else { x0 = -(int)w; x1 = (int)(-w * k); }
    if (x1 > x0) u8g2.drawHLine(cx + x0, cy + y, x1 - x0 + 1);
  }
  u8g2.drawCircle(cx, cy, r);
  u8g2.setFont(F_TINY);
  const char* nm = moonName(age);
  u8g2.drawStr(127 - u8g2.getStrWidth(nm), 50, nm);
  char b[24];
  snprintf(b, sizeof(b), "%d%% lit", (int)((1 - cosf(2 * PI * p)) * 50));
  u8g2.drawStr(127 - u8g2.getStrWidth(b), 58, b);
  drawWeatherIcon(2, 2, 18, 0, true);
  u8g2.setFont(F_MED);
  if (!homeW.ok) { u8g2.drawStr(2, 36, "Set location"); u8g2.drawStr(2, 48, "for sun times"); return; }
  snprintf(b, sizeof(b), "Rise %s", homeW.sunrise);
  u8g2.drawStr(24, 12, b);
  snprintf(b, sizeof(b), "Set  %s", homeW.sunset);
  u8g2.drawStr(24, 24, b);
  int sr = parseHM(homeW.sunrise), ss = parseHM(homeW.sunset);
  if (sr >= 0 && ss > sr) {
    snprintf(b, sizeof(b), "Day %dh %02dm", (ss - sr) / 60, (ss - sr) % 60);
    u8g2.drawStr(2, 42, b);
  }
  snprintf(b, sizeof(b), "UV max %.0f", homeW.uv);
  u8g2.drawStr(2, 56, b);
}
static void sunmoon_speak(CardCtx&, String& s) {
  s = String("The moon is ") + moonName(moonAge()) + ".";
  if (homeW.ok) s += String(" Sunrise ") + homeW.sunrise + ", sunset " + homeW.sunset + ".";
}
const CardType CT_sunmoon = {"sunmoon", 0, 0, 0, nullptr, sunmoon_draw, nullptr, nullptr, sunmoon_speak, nullptr};

// ================================================================== countdown / days since
static time_t paramDate(JsonObject p, const char* defTime) {
  int y = 0, m = 0, d = 0, hh = 0, mm = 0;
  if (sscanf(p["date"] | "", "%d-%d-%d", &y, &m, &d) != 3) return 0;
  sscanf(p["time"] | defTime, "%d:%d", &hh, &mm);
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = hh; t.tm_min = mm; t.tm_isdst = -1;
  return mktime(&t);
}
static void countdown_draw(CardCtx& c) {
  time_t tgt = paramDate(c.p, "00:00");
  u8g2.setFont(F_MED);
  String title = c.p["title"] | "Countdown";
  textCenter(10, title.c_str());
  if (!tgt || !timeOK()) { drawCenteredMsg(tgt ? "Syncing time..." : "Set a date in the dashboard"); return; }
  long diff = (long)(tgt - time(nullptr));
  char b[24];
  if (diff > 86400) {
    snprintf(b, sizeof(b), "%ld", diff / 86400);
    u8g2.setFont(F_NUM32);
    int w = u8g2.getStrWidth(b);
    u8g2.drawStr((128 - w) / 2 - 12 + burnX, 47, b);
    u8g2.setFont(F_MED);
    u8g2.drawStr((128 + w) / 2 - 8 + burnX, 47, diff / 86400 == 1 ? "day" : "days");
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", (diff % 86400) / 3600, (diff % 3600) / 60, diff % 60);
    textCenter(62, b);
  } else if (diff > 0) {
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", diff / 3600, (diff % 3600) / 60, diff % 60);
    u8g2.setFont(F_NUM24);
    int w = u8g2.getStrWidth(b);
    u8g2.drawStr((128 - w) / 2, 46, b);
  } else {
    u8g2.setFont(F_HELV14);
    textCenter(40, diff > -86400 ? "It's here!" : "Done");
    snprintf(b, sizeof(b), "%ld days ago", -diff / 86400);
    u8g2.setFont(F_SMALL);
    if (diff <= -86400) textCenter(56, b);
  }
}
static void countdown_tick(CardCtx& c) {
  if (millis() - c.st[1] < 1000) return;
  c.st[1] = millis();
  time_t tgt = paramDate(c.p, "00:00");
  if (!tgt || !timeOK() || !(c.p["alert"] | true)) return;
  long diff = (long)(tgt - time(nullptr));
  if (diff <= 0 && diff > -120 && c.st[0] != (uint32_t)tgt) {
    c.st[0] = (uint32_t)tgt;
    String title = c.p["title"] | "Countdown";
    notify(title.c_str(), "The moment is here!", E_EXCITED, "sfx:win", EV_NOTIFY, true, 15);
  }
}
static void countdown_speak(CardCtx& c, String& s) {
  time_t tgt = paramDate(c.p, "00:00");
  if (!tgt) return;
  long d = (long)(tgt - time(nullptr)) / 86400;
  s = numWords(d) + " days until " + String(c.p["title"] | "the countdown") + ".";
}
const CardType CT_countdown = {"countdown", 0, 0, 0, nullptr, countdown_draw, nullptr, nullptr, countdown_speak, countdown_tick};

static void dayssince_draw(CardCtx& c) {
  time_t from = paramDate(c.p, "00:00");
  u8g2.setFont(F_MED);
  String title = c.p["title"] | "Days since";
  textCenter(10, title.c_str());
  if (!from || !timeOK()) { drawCenteredMsg(from ? "Syncing time..." : "Set a date in the dashboard"); return; }
  long d = (long)(time(nullptr) - from) / 86400;
  char b[12];
  snprintf(b, sizeof(b), "%ld", d);
  u8g2.setFont(F_NUM32);
  int w = u8g2.getStrWidth(b);
  u8g2.drawStr((128 - w) / 2 + burnX, 50, b);
  u8g2.setFont(F_SMALL);
  textCenter(62, d == 1 ? "day" : "days");
}
static void dayssince_speak(CardCtx& c, String& s) {
  time_t from = paramDate(c.p, "00:00");
  if (!from) return;
  s = String(c.p["title"] | "Days since") + ": " + numWords((long)(time(nullptr) - from) / 86400) + " days.";
}
const CardType CT_dayssince = {"dayssince", 0, 0, 0, nullptr, dayssince_draw, nullptr, nullptr, dayssince_speak, nullptr};
