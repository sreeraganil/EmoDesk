#include "cards.h"
#include "services.h"

static bool imperial() { LOCK(); bool r = cfg["units"] == "imperial"; UNLOCK(); return r; }
static const char* tempUnit() { return cfg["units"] == "imperial" ? "F" : "C"; }

static bool isCustomLoc(JsonObject p) { return !p["loc"]["lat"].isNull(); }
static const char* locName(JsonObject p) {
  if (isCustomLoc(p)) return p["loc"]["name"] | "Weather";
  return cfg["loc"]["name"] | "Weather";
}

// ================================================================== weather
static bool weather_fetch(JsonObject p, void* d) {
  float lat, lon;
  bool custom;
  if (!locFromParams(p, lat, lon, custom)) return false;
  if (!custom) { gFetchNext = 3600; return true; }   // home weather comes from the shared feed
  return fetchWeather(lat, lon, imperial(), *(WeatherData*)d);
}
static WeatherData* wdata(CardCtx& c) { return isCustomLoc(c.p) ? (WeatherData*)c.d : &homeW; }

static bool weatherGate(CardCtx& c, WeatherData* w) {
  if (!isCustomLoc(c.p) && cfg["loc"]["lat"].isNull()) { drawCenteredMsg("Set your location", "Dashboard > Services"); return false; }
  return dataGate(w->ok, "weather");
}

static void weather_draw(CardCtx& c) {
  WeatherData* w = wdata(c);
  if (!weatherGate(c, w)) return;
  drawHeader(locName(c.p), wmoText(w->code));
  drawWeatherIcon(2 + burnX, 14, 28, w->code, w->isDay);
  char b[32];
  snprintf(b, sizeof(b), "%d", (int)roundf(w->temp));
  u8g2.setFont(F_NUM32);
  int tw = u8g2.getStrWidth(b);
  u8g2.drawStr(34 + burnX, 44, b);
  u8g2.setFont(F_SMALL);
  int ux = 36 + tw + burnX;
  u8g2.drawCircle(ux + 2, 20, 2);
  u8g2.drawUTF8(ux + 7, 26, tempUnit());
  snprintf(b, sizeof(b), "%d/%d", (int)roundf(w->dMax[0]), (int)roundf(w->dMin[0]));
  u8g2.setFont(F_TINY);
  u8g2.drawStr(ux, 38, b);
  int rx = 127;
  u8g2.setFont(F_TINY);
  snprintf(b, sizeof(b), "fls %d%s", (int)roundf(w->feels), tempUnit());
  textRight(rx, 22, b);
  snprintf(b, sizeof(b), "hum %d%%", (int)w->hum);
  textRight(rx, 31, b);
  snprintf(b, sizeof(b), "%d %s", (int)roundf(w->wind), cfg["units"] == "imperial" ? "mph" : "kmh");
  textRight(rx, 40, b);
  drawDottedHLine(0, 48, 128);
  snprintf(b, sizeof(b), "%s  |  High %d%s  Low %d%s", wmoText(w->code),
           (int)roundf(w->dMax[0]), tempUnit(), (int)roundf(w->dMin[0]), tempUnit());
  u8g2.setFont(F_TINY);
  textCenter(58, b);
}

static void weather_speak(CardCtx& c, String& s) {
  WeatherData* w = wdata(c);
  if (!w->ok) { s = "I do not have the weather yet."; return; }
  s = String("In ") + locName(c.p) + " it is " + String((int)roundf(w->temp)) + " degrees and " + wmoText(w->code) +
      ". Feels like " + String((int)roundf(w->feels)) + ". High " + String((int)roundf(w->dMax[0])) + ", low " +
      String((int)roundf(w->dMin[0])) + ".";
}
const CardType CT_weather = {"weather", sizeof(WeatherData), 900, 0, nullptr, weather_draw, nullptr, weather_fetch, weather_speak, nullptr};

static void forecast_draw(CardCtx& c) {
  WeatherData* w = wdata(c);
  if (!weatherGate(c, w)) return;
  char b[24];
  if (!strcmp(c.p["mode"] | "daily", "hourly") && w->hn > 1) {
    drawHeader("Next 12 Hours", locName(c.p));
    float mn = w->hTemp[0], mx = w->hTemp[0];
    for (int i = 1; i < w->hn; i++) { mn = min(mn, w->hTemp[i]); mx = max(mx, w->hTemp[i]); }
    drawSpark(4, 18, 120, 26, w->hTemp, w->hn);
    u8g2.setFont(F_TINY);
    snprintf(b, sizeof(b), "%d", (int)roundf(mx)); u8g2.drawStr(2, 18, b);
    snprintf(b, sizeof(b), "%d", (int)roundf(mn)); u8g2.drawStr(2, 46, b);
    for (int i = 0; i < w->hn; i += 3) {
      int x = 4 + i * 120 / (w->hn - 1);
      snprintf(b, sizeof(b), "%d", w->hHour[i]);
      u8g2.drawStr(x - 3, 62, b);
      u8g2.drawVLine(x, 50, 3);
    }
    return;
  }
  for (int i = 0; i < 3; i++) {
    int colX = i * 43;
    const char* dn = i == 0 ? "Today" : WDAY[w->dWday[i]];
    u8g2.setFont(F_TINY);
    int dw = u8g2.getUTF8Width(dn);
    int px = colX + (42 - dw - 6) / 2;
    u8g2.drawRFrame(px, 1, dw + 6, 9, 2);
    u8g2.drawUTF8(px + 3, 7, dn);
    drawWeatherIcon(colX + 11, 14, 20, w->dCode[i], true);
    u8g2.setFont(F_MED);
    snprintf(b, sizeof(b), "%d%s", (int)roundf(w->dMax[i]), tempUnit());
    int wMax = u8g2.getUTF8Width(b);
    u8g2.drawUTF8(colX + (42 - wMax) / 2, 46, b);
    u8g2.setFont(F_TINY);
    snprintf(b, sizeof(b), "%d%s", (int)roundf(w->dMin[i]), tempUnit());
    int wMin = u8g2.getUTF8Width(b);
    u8g2.drawUTF8(colX + (42 - wMin) / 2, 57, b);
    if (i > 0) {
      for (int y = 4; y < 60; y += 3) u8g2.drawPixel(colX - 1, y);
    }
  }
}
static void forecast_speak(CardCtx& c, String& s) {
  WeatherData* w = wdata(c);
  if (!w->ok) return;
  s = String("Tomorrow: ") + wmoText(w->dCode[1]) + ", high " + String((int)roundf(w->dMax[1])) + ", low " +
      String((int)roundf(w->dMin[1])) + ".";
}
const CardType CT_forecast = {"forecast", sizeof(WeatherData), 900, 0, nullptr, forecast_draw, nullptr, weather_fetch, forecast_speak, nullptr};

// ================================================================== air quality
static const char* aqiText(int a) {
  if (a <= 50) return "Good";
  if (a <= 100) return "Moderate";
  if (a <= 150) return "Sensitive";
  if (a <= 200) return "Unhealthy";
  if (a <= 300) return "Very bad";
  return "Hazardous";
}
static bool aqi_fetch(JsonObject p, void* d) {
  float lat, lon;
  bool custom;
  if (!locFromParams(p, lat, lon, custom)) return false;
  return fetchAir(lat, lon, *(AirData*)d);
}
static void aqi_draw(CardCtx& c) {
  AirData* a = (AirData*)c.d;
  if (!isCustomLoc(c.p) && cfg["loc"]["lat"].isNull()) { drawCenteredMsg("Set your location", "Dashboard > Services"); return; }
  if (!dataGate(a->ok, "air quality")) return;
  drawHeader("Air quality", locName(c.p));
  char b[16];
  snprintf(b, sizeof(b), "%d", a->aqi);
  u8g2.setFont(F_NUM32);
  u8g2.drawStr(2 + burnX, 48, b);
  u8g2.setFont(F_MED);
  u8g2.drawStr(2, 62, aqiText(a->aqi));
  u8g2.setFont(F_SMALL);
  snprintf(b, sizeof(b), "PM2.5 %.0f", a->pm25); u8g2.drawStr(72, 22, b);
  snprintf(b, sizeof(b), "PM10  %.0f", a->pm10); u8g2.drawStr(72, 34, b);
  snprintf(b, sizeof(b), "UV    %.1f", a->uv); u8g2.drawStr(72, 46, b);
  drawBar(72, 52, 54, 8, min(1.0f, a->aqi / 300.0f));
}
static void aqi_speak(CardCtx& c, String& s) {
  AirData* a = (AirData*)c.d;
  if (a->ok) s = "Air quality index is " + String(a->aqi) + ", " + aqiText(a->aqi) + ".";
}
const CardType CT_aqi = {"aqi", sizeof(AirData), 1800, 0, nullptr, aqi_draw, nullptr, aqi_fetch, aqi_speak, nullptr};

// ================================================================== cricket
static bool cricGate() {
  const char* url = cfg["cric"]["url"] | DEFAULT_SPORTS_SERVER;
  if (!*url) { drawCenteredMsg("Set the match server", "Dashboard > Connect"); return false; }
  if (!cric.ok) return dataGate(false, cric.err[0] ? cric.err : "cricket scores");
  if (!cric.n) { drawCenteredMsg("No current matches"); return false; }
  return true;
}
static const char* cricTeam(CardCtx& c) {
  if (c.p["team"].is<const char*>() && c.p["team"].as<const char*>()[0]) {
    return c.p["team"].as<const char*>();
  }
  if (c.p["teams"].is<const char*>() && c.p["teams"].as<const char*>()[0]) {
    return c.p["teams"].as<const char*>();
  }
  if (cfg["cric"]["team"].is<const char*>() && cfg["cric"]["team"].as<const char*>()[0]) {
    return cfg["cric"]["team"].as<const char*>();
  }
  if (cfg["cric"]["teams"].is<const char*>() && cfg["cric"]["teams"].as<const char*>()[0]) {
    return cfg["cric"]["teams"].as<const char*>();
  }
  return "";
}
static void cricOversStr(float o, char* out, size_t n) {
  int w = (int)o;
  int b = (int)roundf((o - w) * 10.0f);
  if (b >= 6) { w += b / 6; b %= 6; }
  if (b == 0) snprintf(out, n, "%d ov", w);
  else snprintf(out, n, "%d.%d ov", w, b);
}

static int cricPick(CardCtx& c) {
  if (!cric.n) return 0;
  const char* matchId = c.p["matchId"].is<const char*>() ? (c.p["matchId"] | "") : "";
  if (!*matchId && cfg["cric"]["matchId"].is<const char*>()) {
    matchId = cfg["cric"]["matchId"].as<const char*>();
  }
  const char* matchTitle = c.p["match"].is<const char*>() ? (c.p["match"] | "") : "";
  if (!*matchTitle && cfg["cric"]["match"].is<const char*>()) {
    matchTitle = cfg["cric"]["match"].as<const char*>();
  }
  const char* team = cricTeam(c);
  int matchOffset = c.st[1] % cric.n;

  // 1. Direct match by matchId
  if (*matchId) {
    for (int i = 0; i < cric.n; i++) {
      int idx = (i + matchOffset) % cric.n;
      if (cric.m[idx].id[0] && !strcasecmp(cric.m[idx].id, matchId)) {
        return idx;
      }
    }
  }

  // 2. Direct match by matchTitle
  if (*matchTitle) {
    for (int i = 0; i < cric.n; i++) {
      int idx = (i + matchOffset) % cric.n;
      const CricMatch& m = cric.m[idx];
      if ((m.name[0] && (icontains(m.name, matchTitle) || icontains(matchTitle, m.name))) ||
          (m.series[0] && (icontains(m.series, matchTitle) || icontains(matchTitle, m.series)))) {
        return idx;
      }
    }
  }

  if (*team) {
    int best = -1, score = -1;
    for (int i = 0; i < cric.n; i++) {
      int idx = (i + matchOffset) % cric.n;
      const CricMatch& m = cric.m[idx];
      bool isMine = teamMatch(m.name, team) ||
                    matchTeamItem(m.t1n, m.t1, team) ||
                    matchTeamItem(m.t2n, m.t2, team);
      if (isMine) {
        int s = 8;
        if (cricLive(m)) s += 4;
        else if (m.started && !m.ended) s += 3;
        else if (!m.started && !m.ended) s += 2;
        else if (m.ended) s += 1;
        if (s > score) { score = s; best = idx; }
      }
    }
    if (best >= 0) return best;
    return -1;
  }

  if (c.st[1] > 0) return matchOffset;

  int best = -1, score = -1;
  for (int i = 0; i < cric.n; i++) {
    int idx = (i + matchOffset) % cric.n;
    const CricMatch& m = cric.m[idx];

    int s = 0;
    if (cricLive(m)) s += 4;
    else if (m.started && !m.ended) s += 3;
    else if (!m.started && !m.ended) s += 2;
    else if (m.ended) s += 1;

    if (s > score) { score = s; best = idx; }
  }
  return best >= 0 ? best : matchOffset;
}

static void formatTeamScore(const CricMatch& m, int side, int level, char* out, size_t n) {
  const char* code = side ? m.t2 : m.t1;
  out[0] = 0;
  int count = 0, last = -1;
  for (int i = 0; i < m.ni; i++) {
    if (!strcmp(m.inn[i].team, code)) { count++; last = i; }
  }
  if (count == 0) return;

  for (int i = 0; i < m.ni; i++) {
    if (strcmp(m.inn[i].team, code)) continue;
    // level >= 2: only show the latest/active innings
    if (level >= 2 && i != last) continue;

    char rw[16];
    const CricInn& in = m.inn[i];
    if (in.w >= 10) snprintf(rw, sizeof(rw), "%d", in.r);
    else snprintf(rw, sizeof(rw), "%d/%d", in.r, in.w);

    char b[24];
    if (i != last) {
      snprintf(b, sizeof(b), "%s & ", rw);
    } else {
      bool showOvers = (level == 0) || (level == 1 && count == 1) || (level == 2);
      if (showOvers && in.o > 0) {
        int w = (int)in.o;
        int bl = (int)roundf((in.o - (float)w) * 10.0f);
        if (bl >= 6) { w += bl / 6; bl %= 6; }
        if (bl == 0) snprintf(b, sizeof(b), "%s (%d)", rw, w);
        else snprintf(b, sizeof(b), "%s (%d.%d)", rw, w, bl);
      } else {
        safeCopy(b, rw, sizeof(b));
      }
    }
    strncat(out, b, n - strlen(out) - 1);
  }
}

static void buildTeamStrings(const CricMatch& m, const char* sc0, const char* sc1,
                             char* lStr, size_t lSize, char* rStr, size_t rSize) {
  if (sc0[0]) snprintf(lStr, lSize, "%s %s", m.t1, sc0);
  else safeCopy(lStr, m.t1, lSize);

  if (sc1[0]) snprintf(rStr, rSize, "%s %s", m.t2, sc1);
  else snprintf(rStr, rSize, "v %s", m.t2);
}

static void drawOverMatrix(int yTop, int h, const char* recentStr) {
  if (!recentStr || !*recentStr) return;
  const char* p = strrchr(recentStr, '|');
  if (p) p++; else p = recentStr;
  while (*p == ' ') p++;
  if (!*p) return;

  char balls[10][6];
  int count = 0;
  const char* cur = p;
  while (*cur && count < 8) {
    while (*cur == ' ') cur++;
    if (!*cur) break;
    const char* start = cur;
    while (*cur && *cur != ' ') cur++;
    int len = cur - start;
    if (len > 0 && len < 5) {
      memcpy(balls[count], start, len);
      balls[count][len] = '\0';
      count++;
    }
  }
  if (!count) return;

  int cellW = (count <= 6) ? 16 : 13;
  int gap = (count <= 6) ? 3 : 2;
  int totalW = count * cellW + (count - 1) * gap;
  int startX = (128 - totalW) / 2;
  if (startX < 1) startX = 1;

  for (int i = 0; i < count; i++) {
    int bx = startX + i * (cellW + gap);
    const char* b = balls[i];
    bool isWkt = (b[0] == 'W' || b[0] == 'w') && (b[1] != 'd' && b[1] != 'D');
    bool isSix = !strcmp(b, "6");
    bool isFour = !strcmp(b, "4");
    bool isDot = !strcmp(b, "0") || !strcmp(b, ".");

    if (isWkt) {
      u8g2.drawRBox(bx, yTop, cellW, h, 2);
      u8g2.setDrawColor(0);
      u8g2.setFont(F_BOLD);
      int tw = u8g2.getUTF8Width("W");
      u8g2.drawUTF8(bx + (cellW - tw) / 2, yTop + h - 2, "W");
      u8g2.setDrawColor(1);
    } else if (isSix) {
      u8g2.drawRBox(bx, yTop, cellW, h, 2);
      u8g2.setDrawColor(0);
      u8g2.setFont(F_BOLD);
      int tw = u8g2.getUTF8Width("6");
      u8g2.drawUTF8(bx + (cellW - tw) / 2, yTop + h - 2, "6");
      u8g2.setDrawColor(1);
    } else if (isFour) {
      u8g2.drawRFrame(bx, yTop, cellW, h, 2);
      u8g2.drawFrame(bx + 1, yTop + 1, cellW - 2, h - 2);
      u8g2.setFont(F_BOLD);
      int tw = u8g2.getUTF8Width("4");
      u8g2.drawUTF8(bx + (cellW - tw) / 2, yTop + h - 2, "4");
    } else if (isDot) {
      u8g2.drawRFrame(bx, yTop, cellW, h, 2);
      u8g2.drawDisc(bx + cellW / 2, yTop + h / 2, 1);
    } else {
      u8g2.drawRFrame(bx, yTop, cellW, h, 2);
      u8g2.setFont((strlen(b) > 1) ? F_TINY : F_SMALL);
      int tw = u8g2.getUTF8Width(b);
      u8g2.drawUTF8(bx + (cellW - tw) / 2, yTop + h - 2, b);
    }
  }
}

static void drawWinProbability(const CricMatch& m, CardCtx& c, int pageNum, int totalPages) {
  char hdrTitle[48];
  const char* defHdr = m.series[0] ? m.series : m.name;
  if (cric.n > 1) {
    int curIdx = cricPick(c);
    snprintf(hdrTitle, sizeof(hdrTitle), "%s %d/%d", defHdr, curIdx + 1, cric.n);
  } else {
    safeCopy(hdrTitle, defHdr, sizeof(hdrTitle));
  }
  drawHeader(hdrTitle, cricLive(m) ? "PROB" : "INFO");

  uint8_t p1 = m.winProb ? m.winProb : 50;
  if (p1 > 98) p1 = 98;
  if (p1 < 2) p1 = 2;
  uint8_t p2 = 100 - p1;

  char probStr1[16], probStr2[16];
  snprintf(probStr1, sizeof(probStr1), "%s %d%%", m.t1, p1);
  snprintf(probStr2, sizeof(probStr2), "%d%% %s", p2, m.t2);

  u8g2.setFont(F_SMALL);
  u8g2.drawUTF8(2, 20, probStr1);
  textRight(126, 20, probStr2);

  int barX = 4, barY = 23, barW = 120, barH = 6;
  u8g2.drawRFrame(barX, barY, barW, barH, 2);
  int fillW = ((barW - 4) * p1) / 100;
  if (fillW > 0) u8g2.drawBox(barX + 2, barY + 2, fillW, barH - 4);
  u8g2.drawVLine(barX + barW / 2, barY - 1, barH + 2);

  drawDottedHLine(0, 32, 128);

  u8g2.setFont(F_TINY);
  if (m.target > 0) {
    char b[48];
    if (m.remRuns > 0 && m.remOvers > 0) {
      int balls = (int)roundf(m.remOvers * 6.0f);
      float rrr = m.rrr > 0 ? m.rrr : (m.remRuns * 6.0f / max(1, balls));
      snprintf(b, sizeof(b), "Need %d in %db (RRR %.1f)", m.remRuns, balls, rrr);
    } else {
      snprintf(b, sizeof(b), "Target: %d runs (CRR %.2f)", m.target, m.crr);
    }
    u8g2.drawUTF8(2, 42, b);

    int meterX = 4, meterY = 46, meterW = 120, meterH = 4;
    u8g2.drawFrame(meterX, meterY, meterW, meterH);
    int curRuns = m.target - m.remRuns;
    if (curRuns > 0 && m.target > 0) {
      int cFill = min(meterW - 2, max(1, (curRuns * (meterW - 2)) / m.target));
      u8g2.drawBox(meterX + 1, meterY + 1, cFill, 2);
    }
  } else if (m.crr > 0) {
    char b[48];
    int proj = (int)roundf(m.crr * 20.0f);
    if (strstr(m.type, "ODI")) proj = (int)roundf(m.crr * 50.0f);
    snprintf(b, sizeof(b), "CRR: %.2f  |  Projected: %d", m.crr, proj);
    u8g2.drawUTF8(2, 42, b);

    int meterX = 4, meterY = 46, meterW = 120, meterH = 4;
    u8g2.drawFrame(meterX, meterY, meterW, meterH);
    int rateFill = min(meterW - 2, max(1, (int)(m.crr * 10)));
    u8g2.drawBox(meterX + 1, meterY + 1, rateFill, 2);
  } else {
    u8g2.drawUTF8(2, 42, m.status[0] ? m.status : "Live update");
  }

  drawDottedHLine(0, 52, 128);
  if (m.partner[0]) {
    char part[32];
    snprintf(part, sizeof(part), "P'ship: %s", m.partner);
    u8g2.drawUTF8(2, 61, part);
    if (m.crr > 0) {
      char crrStr[16];
      snprintf(crrStr, sizeof(crrStr), "CRR %.1f", m.crr);
      textRight(126, 61, crrStr);
    }
  } else if (m.status[0]) {
    marquee(2, 61, 100, m.status, c.t0, 24);
  }

  bool autoCycle = (c.p["cycle"] | true) && !(c.st[2] & 1);
  if (autoCycle && totalPages > 1) {
    char pStr[8];
    snprintf(pStr, sizeof(pStr), "%d/%d", pageNum + 1, totalPages);
    drawBadge(127, 61, pStr);
  }
}

static void cricket_draw(CardCtx& c) {
  if (!cricGate()) return;
  int pickIdx = cricPick(c);
  if (pickIdx < 0 || pickIdx >= cric.n) {
    const char* t = cricTeam(c);
    drawCenteredMsg("No match for", *t ? t : "teams");
    return;
  }
  const CricMatch& m = cric.m[pickIdx];
  bool showOverMatrix = c.p["overMatrix"] | true;
  bool showWinProb = c.p["winProb"] | true;
  const char* layout = c.p["layout"] | "modern";
  bool isCompact = !strcmp(layout, "compact");
  bool autoCycle = (c.p["cycle"] | true) && !(c.st[2] & 1);

  int totalPages = isCompact ? (showWinProb ? 2 : 1) : (showWinProb ? 4 : 3);
  uint8_t page = autoCycle ? (((millis() - c.t0) / 4500) % totalPages) : (c.st[0] % totalPages);
  char b[48];

  char hdrTitle[48];
  const char* defHdr = m.series[0] ? m.series : m.name;
  if (cric.n > 1) {
    snprintf(hdrTitle, sizeof(hdrTitle), "%s %d/%d", defHdr, pickIdx + 1, cric.n);
  } else {
    safeCopy(hdrTitle, defHdr, sizeof(hdrTitle));
  }

  if (page == 0) {
    drawHeader(hdrTitle, cricLive(m) ? "LIVE" : (m.ended ? "FT" : (m.type[0] ? m.type : nullptr)));

    if (isCompact) {
      for (int s = 0; s < 2; s++) {
        int y = 23 + s * 14;
        bool batting = (s == 0 ? m.t1Batting : m.t2Batting);
        const char* tCode = s ? m.t2 : m.t1;
        u8g2.setFont(F_BOLD);
        if (batting && cricLive(m)) {
          u8g2.drawTriangle(1, y - 7, 5, y - 4, 1, y - 1);
          u8g2.drawStr(8, y, tCode);
        } else {
          u8g2.drawStr(2, y, tCode);
        }
        cricScore(m, s, b, sizeof(b));
        if (b[0]) {
          int tx = (batting && cricLive(m)) ? 8 : 2;
          int tWidth = tx + u8g2.getUTF8Width(tCode);
          u8g2.setFont(F_MED);
          if (u8g2.getUTF8Width(b) > 126 - tWidth - 4) u8g2.setFont(F_SMALL);
          textRight(126, y, b);
        }
      }
      drawDottedHLine(0, 50, 128);
      if (showOverMatrix && m.recent[0]) {
        drawOverMatrix(52, 11, m.recent);
      } else if (m.status[0]) {
        u8g2.setFont(F_TINY);
        marquee(0, 61, autoCycle ? 104 : 126, m.status, c.t0, 26);
      }
    } else {
      bool hasLiveDetails = (m.bat1[0] || m.bowl[0]) && cricLive(m);
      if (hasLiveDetails) {
        char sc0[32] = "", sc1[32] = "";
        char lStr[40] = "", rStr[40] = "";
        bool useTiny = false;

        u8g2.setFont(F_SMALL);
        formatTeamScore(m, 0, 0, sc0, sizeof(sc0));
        formatTeamScore(m, 1, 0, sc1, sizeof(sc1));
        buildTeamStrings(m, sc0, sc1, lStr, sizeof(lStr), rStr, sizeof(rStr));

        if (u8g2.getUTF8Width(lStr) + u8g2.getUTF8Width(rStr) + 6 > 124) {
          useTiny = true;
          u8g2.setFont(F_TINY);
          for (int lvl = 0; lvl <= 3; lvl++) {
            formatTeamScore(m, 0, lvl, sc0, sizeof(sc0));
            formatTeamScore(m, 1, lvl, sc1, sizeof(sc1));
            buildTeamStrings(m, sc0, sc1, lStr, sizeof(lStr), rStr, sizeof(rStr));
            if (u8g2.getUTF8Width(lStr) + u8g2.getUTF8Width(rStr) + 4 <= 124) break;
          }
        }

        u8g2.setFont(useTiny ? F_TINY : F_SMALL);
        int rw = u8g2.getUTF8Width(rStr);
        int rx = 126 - rw;
        int maxLw = rx - 4;
        if (maxLw < 8) maxLw = 8;
        while (u8g2.getUTF8Width(lStr) > maxLw && strlen(lStr) > 3) {
          lStr[strlen(lStr) - 1] = '\0';
        }

        u8g2.drawStr(2, 20, lStr);
        u8g2.drawStr(rx, 20, rStr);

        drawDottedHLine(0, 22, 128);

        u8g2.setFont(F_TINY);
        int yPos = (m.bat1[0] && m.bat2[0] && m.bowl[0]) ? 31 : 33;
        int gap = (m.bat1[0] && m.bat2[0] && m.bowl[0]) ? 9 : 10;
        if (m.bat1[0]) {
          u8g2.drawTriangle(2, yPos - 5, 5, yPos - 2, 2, yPos + 1);
          u8g2.drawStr(8, yPos, m.bat1);
          yPos += gap;
        }
        if (m.bat2[0]) {
          u8g2.drawStr(8, yPos, m.bat2);
          yPos += gap;
        }
        if (m.bowl[0]) {
          u8g2.drawDisc(3, yPos - 2, 2);
          u8g2.drawStr(8, yPos, m.bowl);
        }

        drawDottedHLine(0, 51, 128);

        if (showOverMatrix && m.recent[0]) {
          drawOverMatrix(53, 10, m.recent);
        } else {
          u8g2.setFont(F_TINY);
          if (m.target > 0 && m.remRuns > 0) {
            snprintf(b, sizeof(b), "Need %d in %db (RRR %.1f)", m.remRuns, (int)roundf(m.remOvers * 6), m.rrr);
          } else if (m.crr > 0) {
            snprintf(b, sizeof(b), "%s (CRR %.2f)", m.status[0] ? m.status : "Live", m.crr);
          } else {
            safeCopy(b, m.status[0] ? m.status : m.name, sizeof(b));
          }
          marquee(0, 62, autoCycle ? 104 : 126, b, c.t0, 22);
        }
      } else {
        for (int s = 0; s < 2; s++) {
          int y = 23 + s * 14;
          bool batting = (s == 0 ? m.t1Batting : m.t2Batting);
          const char* tCode = s ? m.t2 : m.t1;
          u8g2.setFont(F_BOLD);
          if (batting && cricLive(m)) {
            u8g2.drawTriangle(1, y - 7, 5, y - 4, 1, y - 1);
            u8g2.drawStr(8, y, tCode);
          } else {
            u8g2.drawStr(2, y, tCode);
          }
          cricScore(m, s, b, sizeof(b));
          if (b[0]) {
            int tx = (batting && cricLive(m)) ? 8 : 2;
            int tWidth = tx + u8g2.getUTF8Width(tCode);
            u8g2.setFont(F_MED);
            if (u8g2.getUTF8Width(b) > 126 - tWidth - 4) {
              u8g2.setFont(F_SMALL);
              if (u8g2.getUTF8Width(b) > 126 - tWidth - 4) u8g2.setFont(F_TINY);
            }
            textRight(126, y, b);
          }
        }
        drawDottedHLine(0, 49, 128);
        if (showOverMatrix && m.recent[0]) {
          drawOverMatrix(51, 11, m.recent);
        } else {
          u8g2.setFont(F_TINY);
          if (m.status[0]) marquee(0, 60, autoCycle ? 104 : 126, m.status, c.t0, 26);
          else textCenter(60, m.name);
        }
      }
    }

    if (autoCycle && totalPages > 1) {
      char pStr[8];
      snprintf(pStr, sizeof(pStr), "1/%d", totalPages);
      drawBadge(127, 61, pStr);
    }

  } else if (showWinProb && page == 1) {
    drawWinProbability(m, c, 1, totalPages);
  } else if (page == (showWinProb ? 2 : 1)) {
    drawHeader("Innings & RR", m.type[0] ? m.type : "Cricket");

    if (m.ni == 0) {
      drawCenteredMsg("No Innings Yet", m.started ? "Match underway" : "Upcoming match");
    } else {
      for (int i = 0; i < min(3, (int)m.ni); i++) {
        const CricInn& in = m.inn[i];
        int y = 22 + i * 11;
        u8g2.setFont(F_SMALL);
        u8g2.drawStr(2, y, in.team);

        int wO = (int)in.o;
        int bO = (int)roundf((in.o - wO) * 10.0f);
        int totalBalls = wO * 6 + bO;
        float rr = totalBalls > 0 ? (float)in.r * 6.0f / totalBalls : 0.0f;

        char ovStr[10];
        cricOversStr(in.o, ovStr, sizeof(ovStr));
        snprintf(b, sizeof(b), "%d/%d (%s) R:%.1f", in.r, in.w, ovStr, rr);
        u8g2.setFont(F_TINY);
        textRight(126, y, b);
      }
    }

    drawDottedHLine(0, 52, 128);
    u8g2.setFont(F_TINY);
    if (m.target > 0) {
      snprintf(b, sizeof(b), "Target: %d runs", m.target);
      u8g2.drawStr(2, 61, b);
    } else if (m.status[0]) {
      marquee(0, 61, autoCycle ? 104 : 126, m.status, c.t0, 26);
    }
    if (autoCycle && totalPages > 1) {
      char pStr[8];
      snprintf(pStr, sizeof(pStr), "%d/%d", (showWinProb ? 2 : 1) + 1, totalPages);
      drawBadge(127, 61, pStr);
    }

  } else {
    drawHeader("Match Details", m.type[0] ? m.type : "Info");

    u8g2.setFont(F_TINY);
    u8g2.drawStr(2, 21, "Teams:");
    snprintf(b, sizeof(b), "%s vs %s", m.t1n[0] ? m.t1n : m.t1, m.t2n[0] ? m.t2n : m.t2);
    u8g2.setFont(F_SMALL);
    marquee(36, 21, 90, b, c.t0, 22);

    u8g2.setFont(F_TINY);
    u8g2.drawStr(2, 32, "Series:");
    marquee(36, 32, 90, m.series[0] ? m.series : m.name, c.t0, 22);

    u8g2.setFont(F_TINY);
    u8g2.drawStr(2, 43, "Venue:");
    marquee(36, 43, 90, m.venue[0] ? m.venue : "Location TBD", c.t0, 22);

    drawDottedHLine(0, 48, 128);
    u8g2.setFont(F_TINY);
    if (m.start) {
      struct tm t;
      localtime_r(&m.start, &t);
      snprintf(b, sizeof(b), "%d %s %02d:%02d", t.tm_mday, MONTH[t.tm_mon], t.tm_hour, t.tm_min);
      u8g2.drawStr(2, 60, b);
    } else {
      u8g2.drawStr(2, 60, m.status[0] ? m.status : "Live update");
    }
    if (autoCycle && totalPages > 1) {
      char pStr[8];
      snprintf(pStr, sizeof(pStr), "%d/%d", totalPages, totalPages);
      drawBadge(127, 61, pStr);
    }
  }
}

static bool cricket_input(CardCtx& c, uint8_t g) {
  if (g == G_LONG) {
    c.st[2] ^= 1;
    bool nowCycle = (c.p["cycle"] | true) && !(c.st[2] & 1);
    toastShow(nowCycle ? "Auto-cycle ON" : "Auto-cycle OFF");
    audioSfx("pop", EV_UI);
    cricRefresh();
    return true;
  }
  return false;
}

static void cricket_speak(CardCtx& c, String& s) {
  if (!cric.ok || !cric.n) return;
  const CricMatch& m = cric.m[cricPick(c)];
  char a[40], b[40];
  cricScore(m, 0, a, sizeof(a));
  cricScore(m, 1, b, sizeof(b));
  s = String(m.t1n) + " " + a + ". " + m.t2n + " " + b + ". " + m.status;
}
const CardType CT_cricket = {"cricket", 0, 0, 0, nullptr, cricket_draw, cricket_input, nullptr, cricket_speak, nullptr};

static void cricfix_draw(CardCtx& c) {
  if (!cricGate()) return;
  const char* team = cricTeam(c);
  drawHeader("Cricket", *team ? team : "Matches");
  int shown = 0, sel = ((millis() - c.t0) / 3500);
  int idx[4];
  for (int i = 0; i < cric.n && shown < 4; i++) {
    bool isMine = !*team || teamMatch(cric.m[i].name, team) ||
                  matchTeamItem(cric.m[i].t1n, cric.m[i].t1, team) ||
                  matchTeamItem(cric.m[i].t2n, cric.m[i].t2, team);
    if (isMine) idx[shown++] = i;
  }
  if (!shown) { drawCenteredMsg("No matches for", team); return; }
  sel %= shown;
  for (int k = 0; k < shown; k++) {
    const CricMatch& m = cric.m[idx[k]];
    int y = 19 + k * 10;
    char b[24];
    snprintf(b, sizeof(b), "%s v %s", m.t1, m.t2);
    u8g2.setFont(F_SMALL);
    if (k == sel) { u8g2.drawBox(0, y - 7, 128, 9); u8g2.setDrawColor(0); }
    u8g2.drawStr(2, y, b);
    if (cricLive(m)) strcpy(b, "LIVE");
    else if (m.ended) strcpy(b, "Result");
    else if (m.start) { struct tm t; localtime_r(&m.start, &t); fmtHM(b, t.tm_hour, t.tm_min, cfgH24()); char d[12]; snprintf(d, sizeof(d), "%d %s ", t.tm_mday, MONTH[t.tm_mon]); String s2 = String(d) + b; safeCopy(b, s2.c_str(), sizeof(b)); }
    else strcpy(b, "Soon");
    textRight(126, y, b);
    u8g2.setDrawColor(1);
  }
  u8g2.setFont(F_TINY);
  marquee(0, 63, 128, cric.m[idx[sel]].status, c.t0 + sel * 3500, 30);
}
const CardType CT_cricfix = {"cricfix", 0, 0, 0, nullptr, cricfix_draw, nullptr, nullptr, cricket_speak, nullptr};

// ================================================================== football (ESPN public scoreboard)
struct FMatch {
  char h[8], a[8];
  char hn[24], an[24];
  int8_t hs, as;
  uint8_t state;
  char clock[12];
  char statusText[24];
  char venue[24];
  char league[20];
  char hForm[10], aForm[10];
  char hScorers[64], aScorers[64];
  time_t t;
  uint8_t hPoss, aPoss;
  uint8_t hShots, aShots;
  uint8_t hSot, aSot;
  uint8_t hCorn, aCorn;
  uint8_t hFouls, aFouls;
  bool hasStats;
  int8_t hPos, aPos;
  int8_t hLivePos, aLivePos;
};
struct FootData { bool ok; uint8_t n; FMatch m[6]; };

static int scoreOf(JsonVariant v) { return v.is<const char*>() ? atoi(v.as<const char*>()) : v.as<int>(); }

struct OldMatch {
  char h[8], a[8], hn[24];
  int8_t hs, as;
};

static void parseFMatch(JsonObject ev, FMatch& m) {
  memset(&m, 0, sizeof(m));
  const char* st = ev["status"] | "scheduled";
  m.state = !strcmp(st, "live") ? 1 : (!strcmp(st, "finished") ? 2 : 0);
  m.t = parseISO(ev["startTime"] | "");
  if (m.state == 1) safeCopy(m.clock, ev["clock"] | "", sizeof(m.clock));
  else safeCopy(m.clock, m.state == 2 ? "FT" : "", sizeof(m.clock));
  
  safeCopy(m.hn, ev["home"]["name"] | "", sizeof(m.hn));
  cleanShortName(ev["home"]["short"] | "", m.hn, m.h, sizeof(m.h));
  m.hs = ev["home"]["score"] | 0;

  safeCopy(m.an, ev["away"]["name"] | "", sizeof(m.an));
  cleanShortName(ev["away"]["short"] | "", m.an, m.a, sizeof(m.a));
  m.as = ev["away"]["score"] | 0;

  safeCopy(m.statusText, ev["statusText"] | "", sizeof(m.statusText));
  safeCopy(m.venue, ev["venue"] | "", sizeof(m.venue));
  safeCopy(m.league, ev["league"] | "", sizeof(m.league));
  safeCopy(m.hForm, ev["home"]["form"] | "", sizeof(m.hForm));
  safeCopy(m.aForm, ev["away"]["form"] | "", sizeof(m.aForm));
  safeCopy(m.hScorers, ev["home"]["scorers"] | "", sizeof(m.hScorers));
  safeCopy(m.aScorers, ev["away"]["scorers"] | "", sizeof(m.aScorers));
  if (ev["stats"].is<JsonObject>()) {
    JsonObject st = ev["stats"];
    m.hPoss = st["poss"][0] | 0;
    m.aPoss = st["poss"][1] | 0;
    m.hShots = st["shots"][0] | 0;
    m.aShots = st["shots"][1] | 0;
    m.hSot = st["sot"][0] | 0;
    m.aSot = st["sot"][1] | 0;
    m.hCorn = st["corn"][0] | 0;
    m.aCorn = st["corn"][1] | 0;
    m.hFouls = st["fouls"][0] | 0;
    m.aFouls = st["fouls"][1] | 0;
    m.hasStats = (m.hPoss > 0 || m.aPoss > 0 || m.hShots > 0 || m.aShots > 0);
  }
  if (m.state == 1 && !m.clock[0] && ev["statusText"].is<const char*>()) {
    safeCopy(m.clock, ev["statusText"] | "", sizeof(m.clock));
  }
  m.hPos = ev["home"]["pos"] | 0;
  m.hLivePos = ev["home"]["livePos"] | m.hPos;
  m.aPos = ev["away"]["pos"] | 0;
  m.aLivePos = ev["away"]["livePos"] | m.aPos;
}

static bool football_fetch(JsonObject p, void* d) {
  FootData* fd = (FootData*)d;
  String league = p["league"] | "eng.1";
  String team = p["teams"].is<const char*>() ? (p["teams"] | "") : (p["team"] | "");
  String show = p["show"] | "all";
  bool alerts = p["alerts"] | true;
  String serverUrl = cfg["cric"]["url"] | DEFAULT_SPORTS_SERVER;
  while (serverUrl.endsWith("/")) serverUrl = serverUrl.substring(0, serverUrl.length() - 1);
  if (serverUrl.endsWith("/api/cricket")) serverUrl = serverUrl.substring(0, serverUrl.length() - 12);
  if (serverUrl.endsWith("/api/emo/cricket")) serverUrl = serverUrl.substring(0, serverUrl.length() - 16);
  while (serverUrl.endsWith("/")) serverUrl = serverUrl.substring(0, serverUrl.length() - 1);
  if (!serverUrl.length()) return false;
  String url = serverUrl + "/api/emo/football/" + show + "?league=" + league;
  if (team.length()) url += "&team=" + urlEncode(team.c_str());
  
  JsonDocument filter;
  JsonObject e = filter.add<JsonObject>();
  e["startTime"] = true;
  e["status"] = true;
  e["statusText"] = true;
  e["clock"] = true;
  e["venue"] = true;
  e["league"] = true;
  e["home"]["short"] = true;
  e["home"]["name"] = true;
  e["home"]["score"] = true;
  e["home"]["form"] = true;
  e["home"]["scorers"] = true;
  e["home"]["pos"] = true;
  e["home"]["livePos"] = true;
  e["away"]["short"] = true;
  e["away"]["name"] = true;
  e["away"]["score"] = true;
  e["away"]["form"] = true;
  e["away"]["scorers"] = true;
  e["away"]["pos"] = true;
  e["away"]["livePos"] = true;
  e["stats"]["poss"][0] = true;
  e["stats"]["poss"][1] = true;
  e["stats"]["shots"][0] = true;
  e["stats"]["shots"][1] = true;
  e["stats"]["sot"][0] = true;
  e["stats"]["sot"][1] = true;
  e["stats"]["corn"][0] = true;
  e["stats"]["corn"][1] = true;
  e["stats"]["fouls"][0] = true;
  e["stats"]["fouls"][1] = true;
  JsonDocument doc;
  bool ok = httpJson(url, doc, &filter);
  if (!ok || !doc.is<JsonArray>()) return false;

  // Snapshot old match scores for goal alerting before modifying fd
  OldMatch oldM[6];
  uint8_t oldN = 0;
  bool oldOk = fd->ok;
  if (oldOk && alerts) {
    oldN = fd->n;
    if (oldN > 6) oldN = 6;
    for (int j = 0; j < oldN; j++) {
      safeCopy(oldM[j].h, fd->m[j].h, sizeof(oldM[j].h));
      safeCopy(oldM[j].a, fd->m[j].a, sizeof(oldM[j].a));
      safeCopy(oldM[j].hn, fd->m[j].hn, sizeof(oldM[j].hn));
      oldM[j].hs = fd->m[j].hs;
      oldM[j].as = fd->m[j].as;
    }
  }

  time_t now = time(nullptr);
  bool anyLive = false;
  bool startingSoon = false;
  JsonArray arr = doc.as<JsonArray>();

  if (team.length() && show == "all") {
    FMatch liveM, nextM, lastM;
    bool hasLive = false, hasNext = false, hasLast = false;
    for (JsonObject ev : arr) {
      const char* hn = ev["home"]["name"] | "";
      const char* an = ev["away"]["name"] | "";
      const char* hs = ev["home"]["short"] | "";
      const char* as = ev["away"]["short"] | "";
      if (!(teamMatch(hn, team.c_str()) || teamMatch(an, team.c_str()) ||
            teamMatch(hs, team.c_str()) || teamMatch(as, team.c_str())))
        continue;

      FMatch tmp;
      parseFMatch(ev, tmp);
      if (tmp.state == 1) {
        anyLive = true;
        liveM = tmp;
        hasLive = true;
      } else if (tmp.state == 0) {
        int32_t diff = tmp.t - now;
        if (diff >= -600 && diff <= 1800) startingSoon = true;
        if (tmp.t > now && (!hasNext || tmp.t < nextM.t)) {
          nextM = tmp;
          hasNext = true;
        }
      } else if (tmp.state == 2) {
        if (!hasLast || tmp.t > lastM.t) {
          lastM = tmp;
          hasLast = true;
        }
      }
    }
    fd->n = 0;
    if (hasLive && fd->n < 6) fd->m[fd->n++] = liveM;
    if (hasNext && fd->n < 6) fd->m[fd->n++] = nextM;
    if (hasLast && fd->n < 6) fd->m[fd->n++] = lastM;
  } else {
    fd->n = 0;
    // Pass 1: live matches
    for (JsonObject ev : arr) {
      if (team.length()) {
        const char* hn = ev["home"]["name"] | "";
        const char* an = ev["away"]["name"] | "";
        const char* hs = ev["home"]["short"] | "";
        const char* as = ev["away"]["short"] | "";
        if (!(teamMatch(hn, team.c_str()) || teamMatch(an, team.c_str()) ||
              teamMatch(hs, team.c_str()) || teamMatch(as, team.c_str())))
          continue;
      }
      const char* st = ev["status"] | "scheduled";
      bool isLive = !strcmp(st, "live");
      time_t t = parseISO(ev["startTime"] | "");
      if (isLive) anyLive = true;
      else if (!strcmp(st, "scheduled")) {
        int32_t diff = t - now;
        if (diff >= -600 && diff <= 1800) startingSoon = true;
      }
      if (isLive && fd->n < 6) {
        parseFMatch(ev, fd->m[fd->n++]);
      }
    }
    // Pass 2: upcoming / finished matches
    if (fd->n < 6) {
      for (JsonObject ev : arr) {
        if (team.length()) {
          const char* hn = ev["home"]["name"] | "";
          const char* an = ev["away"]["name"] | "";
          const char* hs = ev["home"]["short"] | "";
          const char* as = ev["away"]["short"] | "";
          if (!(teamMatch(hn, team.c_str()) || teamMatch(an, team.c_str()) ||
                teamMatch(hs, team.c_str()) || teamMatch(as, team.c_str())))
            continue;
        }
        const char* st = ev["status"] | "scheduled";
        if (strcmp(st, "live") != 0) {
          parseFMatch(ev, fd->m[fd->n++]);
          if (fd->n >= 6) break;
        }
      }
    }
  }

  static char alertedGoals[8][48];
  static uint8_t alertedGoalIdx = 0;

  for (int i = 0; i < fd->n; i++) {
    FMatch& m = fd->m[i];
    if (m.state == 1) anyLive = true;
    if (!oldOk || !alerts) continue;
    for (int j = 0; j < oldN; j++) {
      const OldMatch& o = oldM[j];
      if (strcmp(o.h, m.h) || strcmp(o.a, m.a) || strcmp(o.hn, m.hn) || m.state != 1) continue;
      if (m.hs + m.as > o.hs + o.as) {
        char gKey[48];
        snprintf(gKey, sizeof(gKey), "%s|%s:%d-%d", m.h, m.a, m.hs, m.as);
        bool goalAlerted = false;
        for (int a = 0; a < 8; a++) {
          if (!strcmp(alertedGoals[a], gKey)) { goalAlerted = true; break; }
        }
        if (!goalAlerted) {
          safeCopy(alertedGoals[alertedGoalIdx], gKey, sizeof(alertedGoals[0]));
          alertedGoalIdx = (alertedGoalIdx + 1) % 8;

          bool homeScored = m.hs > o.hs;
          const char* scorer = homeScored ? m.hn : m.an;
          bool ours = !team.length() || teamMatch(scorer, team.c_str()) || teamMatch(homeScored ? m.h : m.a, team.c_str());
          char b[64];
          snprintf(b, sizeof(b), "%s %d-%d %s  %s", m.h, m.hs, m.as, m.a, m.clock);
          String gAnim;
          LOCK();
          if (cfg["sports"]["animGoal"].is<const char*>()) gAnim = (const char*)cfg["sports"]["animGoal"];
          UNLOCK();
          if (gAnim.isEmpty() && p["anim"].is<const char*>()) gAnim = (const char*)p["anim"];
          notify("GOAL!", b, ours ? E_EXCITED : E_SAD, ours ? "sfx:goal" : "sfx:notify", EV_SPORTS, false, 12, NK_NORMAL, -1, gAnim.isEmpty() ? nullptr : gAnim.c_str());
        }
      }
      break;
    }
  }
  int liveSec = 30;
  LOCK();
  if (!cfg["cric"]["liveSec"].isNull()) liveSec = cfg["cric"]["liveSec"].as<int>();
  else if (!cfg["cric"]["liveMin"].isNull()) liveSec = cfg["cric"]["liveMin"].as<int>() * 60;
  UNLOCK();
  if (liveSec < 5) liveSec = 5;
  gFetchNext = anyLive ? liveSec : (startingSoon ? 120 : 900);
  fd->ok = true;
  return true;
}

static void fmtKick(time_t t, char* out, size_t n) {
  struct tm lt;
  localtime_r(&t, &lt);
  char hm[8];
  fmtHM(hm, lt.tm_hour, lt.tm_min, cfgH24());
  snprintf(out, n, "%s %s", WDAY[lt.tm_wday], hm);
}

static void football_draw(CardCtx& c) {
  FootData* f = (FootData*)c.d;
  if (!dataGate(f->ok, "football scores")) return;
  const char* team = c.p["teams"].is<const char*>() ? (c.p["teams"] | "") : (c.p["team"] | "");
  if (!f->n) {
    drawCenteredMsg(*team ? "No matches found" : "No matches today", c.p["league"] | "Football");
    return;
  }

  int totalToShow = f->n;
  int sel = (c.st[0] + (millis() - c.t0) / 6000) % totalToShow;
  FMatch& m = f->m[sel];

  const char* rBadge = nullptr;
  char kBuf[20];
  if (m.state == 1) {
    rBadge = m.clock[0] ? m.clock : "LIVE";
  } else if (m.state == 2) {
    rBadge = "FT";
  } else {
    fmtKick(m.t, kBuf, sizeof(kBuf));
    rBadge = kBuf;
  }

  bool showStats = m.hasStats;
  int totalPages = showStats ? 3 : 2;
  bool autoCycle = (c.p["cycle"] | true) && !(c.st[2] & 1);
  uint8_t page = (autoCycle && totalPages > 1) ? (((millis() - c.t0) / 4500) % totalPages) : 0;

  const char* defHdr = m.league[0] ? m.league : (*team ? team : (c.p["league"] | "Football"));
  char hdrTitle[48];
  if (totalToShow > 1) {
    snprintf(hdrTitle, sizeof(hdrTitle), "%s %d/%d", defHdr, sel + 1, totalToShow);
  } else {
    safeCopy(hdrTitle, defHdr, sizeof(hdrTitle));
  }

  if (page == 0) {
    drawHeader(hdrTitle, rBadge);

    u8g2.setFont(F_BOLD);
    u8g2.drawStr(2, 26, m.h);
    int hw = u8g2.getUTF8Width(m.h);

    int aw = u8g2.getUTF8Width(m.a);
    textRight(126, 26, m.a);

    char hPosStr[12] = "";
    char aPosStr[12] = "";
    bool isLive = (m.state == 1);

    if (m.hPos > 0) {
      if (isLive && m.hLivePos > 0 && m.hLivePos != m.hPos) {
        snprintf(hPosStr, sizeof(hPosStr), "%d>%d", m.hPos, m.hLivePos);
      } else {
        snprintf(hPosStr, sizeof(hPosStr), "#%d", isLive && m.hLivePos > 0 ? m.hLivePos : m.hPos);
      }
    }
    if (m.aPos > 0) {
      if (isLive && m.aLivePos > 0 && m.aLivePos != m.aPos) {
        snprintf(aPosStr, sizeof(aPosStr), "%d>%d", m.aPos, m.aLivePos);
      } else {
        snprintf(aPosStr, sizeof(aPosStr), "#%d", isLive && m.aLivePos > 0 ? m.aLivePos : m.aPos);
      }
    }

    u8g2.setFont(F_TINY);
    if (hPosStr[0]) {
      u8g2.drawStr(2 + hw + 2, 26, hPosStr);
    }
    if (aPosStr[0]) {
      int pw = u8g2.getUTF8Width(aPosStr);
      u8g2.drawStr(124 - aw - pw, 26, aPosStr);
    }

    char scoreStr[20];
    if (m.state == 0) {
      u8g2.setFont(F_HELV14);
      textCenter(27, "vs");
    } else {
      snprintf(scoreStr, sizeof(scoreStr), "%d - %d", m.hs, m.as);
      u8g2.setFont(F_HELV14);
      textCenter(27, scoreStr);
    }

    char vsNames[64];
    const char* hName = m.hn[0] ? m.hn : m.h;
    const char* aName = m.an[0] ? m.an : m.a;
    snprintf(vsNames, sizeof(vsNames), "%s vs %s", hName, aName);
    u8g2.setFont(F_SMALL);
    int vsW = u8g2.getUTF8Width(vsNames);
    if (vsW <= 126) textCenter(37, vsNames);
    else marquee(0, 37, 128, vsNames, c.t0, 25);

    bool hasScorers = (m.hScorers[0] || m.aScorers[0]);
    if (hasScorers) {
      char scStr[140];
      if (m.hScorers[0] && m.aScorers[0]) {
        snprintf(scStr, sizeof(scStr), "%s: %s  |  %s: %s", m.h, m.hScorers, m.a, m.aScorers);
      } else if (m.hScorers[0]) {
        snprintf(scStr, sizeof(scStr), "%s: %s", m.h, m.hScorers);
      } else {
        snprintf(scStr, sizeof(scStr), "%s: %s", m.a, m.aScorers);
      }
      u8g2.setFont(F_TINY);
      int scW = u8g2.getUTF8Width(scStr);
      if (scW <= 126) textCenter(47, scStr);
      else marquee(0, 47, 128, scStr, c.t0, 22);
      drawDottedHLine(0, 50, 128);
    } else {
      drawDottedHLine(0, 47, 128);
    }

    char detail[100] = "";
    if (m.state == 1) {
      const char* stDesc = m.statusText[0] ? m.statusText : (m.clock[0] ? m.clock : "Live");
      if (m.hPos > 0 && m.aPos > 0) {
        if (m.hLivePos != m.hPos || m.aLivePos != m.aPos) {
          snprintf(detail, sizeof(detail), "%s · Live: %s %d>%d, %s %d>%d%s%s",
                   stDesc, m.h, m.hPos, m.hLivePos, m.a, m.aPos, m.aLivePos,
                   m.venue[0] ? " · " : "", m.venue[0] ? m.venue : "");
        } else {
          snprintf(detail, sizeof(detail), "%s · Table: %s #%d, %s #%d%s%s",
                   stDesc, m.h, m.hPos, m.a, m.aPos,
                   m.venue[0] ? " · " : "", m.venue[0] ? m.venue : "");
        }
      } else if (m.venue[0]) {
        snprintf(detail, sizeof(detail), "%s · %s", stDesc, m.venue);
      } else safeCopy(detail, stDesc, sizeof(detail));
    } else if (m.state == 2) {
      if (m.hPos > 0 && m.aPos > 0) {
        snprintf(detail, sizeof(detail), "FT · %s #%d, %s #%d%s%s",
                 m.h, m.hPos, m.a, m.aPos,
                 m.venue[0] ? " · " : "", m.venue[0] ? m.venue : "");
      } else if (m.venue[0]) {
        snprintf(detail, sizeof(detail), "FT · %s", m.venue);
      } else strcpy(detail, "Full Time · Final Result");
    } else {
      char k2[24];
      fmtKick(m.t, k2, sizeof(k2));
      if (m.hPos > 0 && m.aPos > 0) {
        snprintf(detail, sizeof(detail), "%s · %s #%d vs %s #%d%s%s",
                 k2, m.h, m.hPos, m.a, m.aPos,
                 m.venue[0] ? " · " : "", m.venue[0] ? m.venue : "");
      } else if (m.venue[0]) {
        snprintf(detail, sizeof(detail), "%s · %s", k2, m.venue);
      } else snprintf(detail, sizeof(detail), "Kickoff: %s", k2);
    }

    u8g2.setFont(F_TINY);
    marquee(0, 60, autoCycle ? 104 : 126, detail, c.t0, 22);
    char badgeBuf[8];
    snprintf(badgeBuf, sizeof(badgeBuf), "1/%d", totalPages);
    if (autoCycle) drawBadge(127, 61, badgeBuf);

  } else if (showStats && page == 1) {
    drawHeader("Match Stats", rBadge);

    u8g2.setFont(F_BOLD);
    u8g2.drawStr(2, 21, m.h);
    textRight(126, 21, m.a);

    char scoreStr[16];
    snprintf(scoreStr, sizeof(scoreStr), "%d - %d", m.hs, m.as);
    u8g2.setFont(F_MED);
    textCenter(21, scoreStr);

    u8g2.setFont(F_TINY);
    char pL[8], pR[8];
    snprintf(pL, sizeof(pL), "%d%%", m.hPoss ? m.hPoss : 50);
    snprintf(pR, sizeof(pR), "%d%%", m.aPoss ? m.aPoss : 50);
    u8g2.drawStr(2, 32, "Poss");
    u8g2.drawStr(25, 32, pL);
    textRight(102, 32, pR);
    float frac = (float)(m.hPoss ? m.hPoss : 50) / 100.0f;
    drawBar(44, 28, 38, 5, frac);

    char shotsStr[44];
    snprintf(shotsStr, sizeof(shotsStr), "Shots(OT): %d(%d) - %d(%d)", m.hShots, m.hSot, m.aShots, m.aSot);
    u8g2.setFont(F_SMALL);
    textCenter(43, shotsStr);

    char cfStr[44];
    snprintf(cfStr, sizeof(cfStr), "Corners: %d-%d   Fouls: %d-%d", m.hCorn, m.aCorn, m.hFouls, m.aFouls);
    u8g2.setFont(F_TINY);
    textCenter(53, cfStr);

    drawDottedHLine(0, 56, 128);

    const char* stDesc = m.statusText[0] ? m.statusText : (m.state == 2 ? "Full Time" : "Live Match Stats");
    marquee(2, 63, autoCycle ? 104 : 126, stDesc, c.t0, 22);
    if (autoCycle) drawBadge(127, 63, "2/3");

  } else {
    drawHeader("Match Details", rBadge);

    u8g2.setFont(F_TINY);
    u8g2.drawStr(2, 20, "Venue:");
    marquee(36, 20, 90, m.venue[0] ? m.venue : "Stadium TBD", c.t0, 22);

    u8g2.drawStr(2, 31, "Form:");
    char formStr[40];
    snprintf(formStr, sizeof(formStr), "%s: %s  |  %s: %s",
             m.h, m.hForm[0] ? m.hForm : "N/A",
             m.a, m.aForm[0] ? m.aForm : "N/A");
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(36, 31, formStr);

    if (m.hPos > 0 || m.aPos > 0) {
      u8g2.setFont(F_TINY);
      u8g2.drawStr(2, 43, "Table:");
      char tabStr[56];
      if (m.state == 1 && (m.hLivePos != m.hPos || m.aLivePos != m.aPos)) {
        snprintf(tabStr, sizeof(tabStr), "%s #%d>%d (Live) | %s #%d>%d",
                 m.h, m.hPos, m.hLivePos, m.a, m.aPos, m.aLivePos);
      } else {
        snprintf(tabStr, sizeof(tabStr), "%s #%d  |  %s #%d",
                 m.h, m.hPos, m.a, m.aPos);
      }
      u8g2.setFont(F_SMALL);
      u8g2.drawStr(36, 43, tabStr);
      drawDottedHLine(0, 48, 128);
    } else {
      drawDottedHLine(0, 46, 128);
    }

    u8g2.setFont(F_TINY);
    if (m.state == 0) {
      char k2[24];
      fmtKick(m.t, k2, sizeof(k2));
      char kickStr[32];
      snprintf(kickStr, sizeof(kickStr), "Kickoff: %s", k2);
      u8g2.drawStr(2, 59, kickStr);
    } else {
      const char* stDesc = m.statusText[0] ? m.statusText : (m.state == 2 ? "Full Time" : "Live in match");
      marquee(2, 59, 96, stDesc, c.t0, 24);
    }
    char badgeBuf[8];
    snprintf(badgeBuf, sizeof(badgeBuf), "%d/%d", totalPages, totalPages);
    if (autoCycle) drawBadge(127, 61, badgeBuf);
  }
}

static bool football_input(CardCtx& c, uint8_t g) {
  FootData* f = (FootData*)c.d;
  if (!f || !f->n) return false;
  if (g == G_LONG) {
    c.st[2] ^= 1;
    bool nowCycle = (c.p["cycle"] | true) && !(c.st[2] & 1);
    toastShow(nowCycle ? "Auto-cycle ON" : "Auto-cycle OFF");
    audioSfx("pop", EV_UI);
    cardRefreshNow(c.idx);
    return true;
  }
  return false;
}

static void football_speak(CardCtx& c, String& s) {
  FootData* f = (FootData*)c.d;
  if (!f->ok || !f->n) return;
  int totalToShow = f->n;
  int sel = (c.st[0] + (millis() - c.t0) / 6000) % totalToShow;
  FMatch& m = f->m[sel];
  const char* hName = m.hn[0] ? m.hn : m.h;
  const char* aName = m.an[0] ? m.an : m.a;
  if (m.state == 0) s = String("Next match, ") + hName + " versus " + aName + ".";
  else {
    s = String(hName) + " " + String(m.hs) + ", " + aName + " " + String(m.as) + (m.state == 1 ? (String(". Live in the ") + (m.clock[0] ? m.clock : "match") + ".") : ". Full time.");
    if (m.state == 1 && m.hPos > 0 && m.hLivePos > 0 && m.hLivePos != m.hPos) {
      s += String(" As it stands, ") + hName + " moves to position " + String(m.hLivePos) + ".";
    }
  }
}
const CardType CT_football = {"football", sizeof(FootData), 900, 0, nullptr, football_draw, football_input, football_fetch, football_speak, nullptr};

// ================================================================== league table
struct TRow { char t[6]; uint8_t rank, pl; int8_t gd; uint8_t pts; };
struct TableData { bool ok; uint8_t n; TRow r[20]; };

static bool ftable_fetch(JsonObject p, void* d) {
  String league = p["league"] | "eng.1";
  String serverUrl = cfg["cric"]["url"] | DEFAULT_SPORTS_SERVER;
  while (serverUrl.endsWith("/")) serverUrl = serverUrl.substring(0, serverUrl.length() - 1);
  if (serverUrl.endsWith("/api/cricket")) serverUrl = serverUrl.substring(0, serverUrl.length() - 12);
  if (serverUrl.endsWith("/api/emo/cricket")) serverUrl = serverUrl.substring(0, serverUrl.length() - 16);
  while (serverUrl.endsWith("/")) serverUrl = serverUrl.substring(0, serverUrl.length() - 1);
  if (!serverUrl.length()) return false;
  
  JsonDocument filter;
  JsonObject g = filter.add<JsonObject>();
  JsonObject e = g["table"].add<JsonObject>();
  e["rank"] = true;
  e["short"] = true;
  e["played"] = true;
  e["gd"] = true;
  e["points"] = true;
  JsonDocument doc;
  if (!httpJson(serverUrl + "/api/emo/football/standings/" + league, doc, &filter)) return false;
  if (!doc.is<JsonArray>() || doc.size() == 0 || !doc[0]["table"].is<JsonArray>()) return false;
  TableData td;
  memset(&td, 0, sizeof(td));
  JsonArray tableArray = doc[0]["table"].as<JsonArray>();
  for (JsonObject ev : tableArray) {
    if (td.n >= 20) break;
    TRow& r = td.r[td.n++];
    const char* tn = ev["team"] | "";
    cleanShortName(ev["short"] | "", tn, r.t, sizeof(r.t));
    r.rank = ev["rank"] | 0;
    r.pl = ev["played"] | 0;
    r.gd = ev["gd"] | 0;
    r.pts = ev["points"] | 0;
  }
  for (int i = 0; i < td.n; i++)
    for (int j = i + 1; j < td.n; j++)
      if (td.r[j].rank < td.r[i].rank) { TRow t = td.r[i]; td.r[i] = td.r[j]; td.r[j] = t; }
  td.ok = td.n > 0;
  *(TableData*)d = td;
  return td.ok;
}
static void ftable_draw(CardCtx& c) {
  TableData* t = (TableData*)c.d;
  if (!dataGate(t->ok, "league table")) return;
  u8g2.setFont(F_TINY);
  u8g2.drawStr(0, 6, "#  Team");
  u8g2.drawStr(70, 6, "P");
  u8g2.drawStr(88, 6, "GD");
  textRight(127, 6, "Pts");
  u8g2.drawHLine(0, 8, 128);
  int pages = (t->n + 4) / 5, pg = ((millis() - c.t0) / 4000) % pages;
  const char* hl = c.p["teams"].is<const char*>() ? (c.p["teams"] | "") : (c.p["team"] | "");
  u8g2.setFont(F_SMALL);
  for (int k = 0; k < 5; k++) {
    int i = pg * 5 + k;
    if (i >= t->n) break;
    TRow& r = t->r[i];
    int y = 18 + k * 10;
    bool mine = *hl && teamMatch(r.t, hl);
    if (mine) { u8g2.drawBox(0, y - 8, 128, 10); u8g2.setDrawColor(0); }
    char b[8];
    snprintf(b, sizeof(b), "%d", r.rank); u8g2.drawStr(0, y, b);
    u8g2.drawStr(14, y, r.t);
    snprintf(b, sizeof(b), "%d", r.pl); u8g2.drawStr(68, y, b);
    snprintf(b, sizeof(b), "%+d", r.gd); u8g2.drawStr(86, y, b);
    snprintf(b, sizeof(b), "%d", r.pts); textRight(127, y, b);
    u8g2.setDrawColor(1);
  }
}
static void ftable_speak(CardCtx& c, String& s) {
  TableData* t = (TableData*)c.d;
  if (!t->ok) return;
  s = String(t->r[0].t) + " lead with " + String(t->r[0].pts) + " points.";
}
const CardType CT_ftable = {"ftable", sizeof(TableData), 3600, 0, nullptr, ftable_draw, nullptr, ftable_fetch, ftable_speak, nullptr};
