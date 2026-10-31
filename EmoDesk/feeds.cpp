#include "feeds.h"
#include "net.h"
#include "emo.h"
#include "cards.h"
#include "audio.h"
#include "power.h"
#include "display.h"
#include "services.h"
#include <WiFi.h>
#if ENABLE_MQTT
#include <PubSubClient.h>
#endif

WeatherData homeW;
CricData cric;
MqttVal mqttVals[12];
uint8_t mqttValN = 0;

static uint32_t nextHomeW = 0, nextCric = 0, nextTg = 0;
static int cricHits = 0;
static uint32_t cricDay = 0;
static char tgPending[256] = "";

void feedsRefresh() { nextHomeW = 0; nextCric = 0; }
void cricRefresh() { nextCric = 0; }

static uint8_t wdayOf(const char* date) {
  int y, m, d;
  if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  long days = makeUTC(y, m, d, 0, 0, 0) / 86400;
  return (uint8_t)((days + 4) % 7);   // 1970-01-01 was a Thursday
}

// ------------------------------------------------------------------ weather
bool fetchWeather(float lat, float lon, bool imp, WeatherData& out) {
  char url[460];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
           "&hourly=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
           "&timezone=auto&forecast_days=3&forecast_hours=12%s",
           lat, lon, imp ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  JsonDocument doc;
  if (!httpJson(url, doc)) return false;
  JsonObject c = doc["current"];
  if (c.isNull()) return false;
  WeatherData t;
  memset(&t, 0, sizeof(t));
  t.temp = c["temperature_2m"] | 0.0f;
  t.feels = c["apparent_temperature"] | 0.0f;
  t.hum = c["relative_humidity_2m"] | 0.0f;
  t.wind = c["wind_speed_10m"] | 0.0f;
  t.code = c["weather_code"] | 0;
  t.isDay = (c["is_day"] | 1) == 1;
  JsonArray ht = doc["hourly"]["time"];
  t.hn = min((size_t)12, ht.size());
  for (int i = 0; i < t.hn; i++) {
    const char* ts = ht[i] | "0000-00-00T00:00";
    t.hHour[i] = strlen(ts) > 12 ? atoi(ts + 11) : 0;
    t.hTemp[i] = doc["hourly"]["temperature_2m"][i] | 0.0f;
    t.hCode[i] = doc["hourly"]["weather_code"][i] | 0;
  }
  JsonObject d = doc["daily"];
  for (int i = 0; i < 3; i++) {
    t.dCode[i] = d["weather_code"][i] | 0;
    t.dMax[i] = d["temperature_2m_max"][i] | 0.0f;
    t.dMin[i] = d["temperature_2m_min"][i] | 0.0f;
    t.dWday[i] = wdayOf(d["time"][i] | "");
  }
  const char* sr = d["sunrise"][0] | "";
  const char* ss = d["sunset"][0] | "";
  if (strlen(sr) >= 16) { memcpy(t.sunrise, sr + 11, 5); t.sunrise[5] = 0; }
  if (strlen(ss) >= 16) { memcpy(t.sunset, ss + 11, 5); t.sunset[5] = 0; }
  t.uv = d["uv_index_max"][0] | 0.0f;
  t.ok = true;
  t.ts = time(nullptr);
  LOCK();
  out = t;
  UNLOCK();
  return true;
}

bool fetchAir(float lat, float lon, AirData& out) {
  char url[220];
  snprintf(url, sizeof(url),
           "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f&current=us_aqi,pm2_5,pm10,uv_index",
           lat, lon);
  JsonDocument doc;
  if (!httpJson(url, doc)) return false;
  JsonObject c = doc["current"];
  if (c.isNull()) return false;
  AirData a;
  a.aqi = c["us_aqi"] | 0;
  a.pm25 = c["pm2_5"] | 0.0f;
  a.pm10 = c["pm10"] | 0.0f;
  a.uv = c["uv_index"] | 0.0f;
  a.ok = true;
  a.ts = time(nullptr);
  LOCK();
  out = a;
  UNLOCK();
  return true;
}

// ------------------------------------------------------------------ cricket
bool cricLive(const CricMatch& m) { return m.started && !m.ended; }

static void formatCricOvers(float o, char* out, size_t n) {
  int whole = (int)o;
  int balls = (int)roundf((o - whole) * 10.0f);
  if (balls >= 6) {
    whole += balls / 6;
    balls = balls % 6;
  }
  if (balls == 0) snprintf(out, n, "%d", whole);
  else snprintf(out, n, "%d.%d", whole, balls);
}

void cricScore(const CricMatch& m, int side, char* out, size_t n) {
  const char* code = side ? m.t2 : m.t1;
  out[0] = 0;
  int last = -1;
  for (int i = 0; i < m.ni; i++) if (!strcmp(m.inn[i].team, code)) last = i;
  for (int i = 0; i < m.ni; i++) {
    if (strcmp(m.inn[i].team, code)) continue;
    char b[28], rw[16];
    const CricInn& in = m.inn[i];
    if (in.w >= 10) snprintf(rw, sizeof(rw), "%d", in.r);          // all out
    else snprintf(rw, sizeof(rw), "%d/%d", in.r, in.w);
    if (i != last) snprintf(b, sizeof(b), "%s & ", rw);
    else if (in.o > 0) {
      char ovStr[10];
      formatCricOvers(in.o, ovStr, sizeof(ovStr));
      snprintf(b, sizeof(b), "%s (%s)", rw, ovStr);   // always show overs if > 0
    }
    else snprintf(b, sizeof(b), "%s", rw);
    strncat(out, b, n - strlen(out) - 1);
  }
  if (!out[0]) out[0] = 0;
}

void cleanShortName(const char* shortVal, const char* fullVal, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = 0;
  if (shortVal && *shortVal && strcmp(shortVal, "?") != 0) {
    size_t len = strlen(shortVal);
    bool hasSpace = false;
    for (size_t i = 0; i < len; i++) {
      if (isspace((unsigned char)shortVal[i])) { hasSpace = true; break; }
    }
    bool sameAsLongName = fullVal && (len > 4 || !strcasecmp(shortVal, fullVal)) && strlen(fullVal) > 4;
    if (len >= 2 && len <= 4 && !hasSpace && !sameAsLongName) {
      size_t k = 0;
      for (; k < len && k < outLen - 1; k++) out[k] = toupper((unsigned char)shortVal[k]);
      out[k] = 0;
      return;
    }
  }
  const char* src = (fullVal && *fullVal) ? fullVal : (shortVal ? shortVal : "");
  if (!*src) {
    safeCopy(out, "?", outLen);
    return;
  }
  int wordCount = 0;
  bool inWord = false;
  for (const char* p = src; *p; p++) {
    if (isalnum((unsigned char)*p)) {
      if (!inWord) { wordCount++; inWord = true; }
    } else inWord = false;
  }
  if (wordCount >= 2) {
    int k = 0;
    inWord = false;
    for (const char* p = src; *p && k < 4 && k < (int)outLen - 1; p++) {
      if (isalnum((unsigned char)*p)) {
        if (!inWord) {
          out[k++] = toupper((unsigned char)*p);
          inWord = true;
        }
      } else inWord = false;
    }
    out[k] = 0;
  } else {
    int k = 0;
    for (const char* p = src; *p && k < 3 && k < (int)outLen - 1; p++) {
      if (isalnum((unsigned char)*p)) out[k++] = toupper((unsigned char)*p);
    }
    out[k] = 0;
  }
  if (!out[0]) safeCopy(out, "?", outLen);
}

static void parseMatch(JsonObject m, CricMatch& c) {
    memset(&c, 0, sizeof(c));
    safeCopy(c.id, m["id"] | "", sizeof(c.id));
    safeCopy(c.name, m["title"] | "", sizeof(c.name));
    safeCopy(c.status, m["statusText"] | "", sizeof(c.status));
    safeCopy(c.type, m["format"] | "", sizeof(c.type));
    safeCopy(c.venue, m["venue"] | "", sizeof(c.venue));
    safeCopy(c.series, m["series"] | "", sizeof(c.series));
    c.target = m["target"] | 0;
    safeCopy(c.bat1, m["bat1"] | "", sizeof(c.bat1));
    safeCopy(c.bat2, m["bat2"] | "", sizeof(c.bat2));
    safeCopy(c.bowl, m["bowl"] | "", sizeof(c.bowl));
    c.crr = m["crr"] | 0.0f;
    safeCopy(c.recent, m["recent"] | "", sizeof(c.recent));
    c.rrr = m["rrr"] | 0.0f;
    c.remRuns = m["remRuns"] | 0;
    c.remOvers = m["oversRem"] | 0.0f;
    safeCopy(c.partner, m["partner"] | "", sizeof(c.partner));
    c.winProb = m["winProb"] | 50;
    const char* st = m["status"] | "";
    c.started = !strcmp(st, "live") || !strcmp(st, "finished");
    c.ended = !strcmp(st, "finished") || !strcmp(st, "cancelled");
    c.start = parseISO(m["startTime"] | "");
    JsonArray teams = m["teams"];
    safeCopy(c.t1n, teams[0]["name"] | "", sizeof(c.t1n));
    safeCopy(c.t2n, teams[1]["name"] | "", sizeof(c.t2n));
    cleanShortName(teams[0]["short"] | "", c.t1n, c.t1, sizeof(c.t1));
    cleanShortName(teams[1]["short"] | "", c.t2n, c.t2, sizeof(c.t2));
    c.t1Batting = teams[0]["batting"] | false;
    c.t2Batting = teams[1]["batting"] | false;
    for (int i = 0; i < 2; i++) {
      for (JsonObject s : teams[i]["innings"].as<JsonArray>()) {
        if (c.ni >= 4) break;
        CricInn& in = c.inn[c.ni++];
        in.r = s["runs"] | 0;
        in.w = s["wickets"] | 0;
        float rawO = s["overs"] | 0.0f;
        int wO = (int)rawO;
        int bO = (int)roundf((rawO - wO) * 10.0f);
        if (bO >= 6) { wO += bO / 6; bO = bO % 6; }
        in.o = (float)wO + (float)bO / 10.0f;
        safeCopy(in.team, i == 0 ? c.t1 : c.t2, sizeof(in.team));
      }
    }
}

static void cricLoop(uint32_t now) {
  if ((int32_t)(now - nextCric) < 0) return;
  LOCK();
  String url = cfg["cric"]["url"] | DEFAULT_SPORTS_SERVER;
  String team = cfg["cric"]["teams"].is<const char*>() ? (cfg["cric"]["teams"] | "") : (cfg["cric"]["team"] | "");
  String targetMatchId = cfg["cric"]["matchId"].is<const char*>() ? (cfg["cric"]["matchId"] | "") : "";
  String targetMatchTitle = cfg["cric"]["match"].is<const char*>() ? (cfg["cric"]["match"] | "") : "";
  String wAnim = cfg["sports"]["animWicket"] | "";
  String rAnim = cfg["sports"]["animResult"] | "";
  bool alW = cfg["cric"]["wicket"] | true, alR = cfg["cric"]["result"] | true, sp = cfg["cric"]["speak"] | false;
  int cur = cardsCurrent();
  if (cur >= 0 && cur < cardsCount()) {
    const char* ct = cardsTypeAt(cur);
    if (!strcmp(ct, "cricket") || !strcmp(ct, "cricfix")) {
      CardCtx cc = cardCtx(cur);
      if (cc.p["matchId"].is<const char*>() && cc.p["matchId"].as<const char*>()[0]) {
        targetMatchId = cc.p["matchId"].as<const char*>();
      }
      if (cc.p["match"].is<const char*>() && cc.p["match"].as<const char*>()[0]) {
        targetMatchTitle = cc.p["match"].as<const char*>();
      }
      const char* t = "";
      if (cc.p["team"].is<const char*>() && cc.p["team"].as<const char*>()[0]) {
        t = cc.p["team"].as<const char*>();
      } else if (cc.p["teams"].is<const char*>() && cc.p["teams"].as<const char*>()[0]) {
        t = cc.p["teams"].as<const char*>();
      }
      if (*t) team = t;
      if (cc.p["alerts"].is<bool>()) { alW = cc.p["alerts"].as<bool>(); alR = alW; }
      if (wAnim.isEmpty() && cc.p["animWicket"].is<const char*>()) wAnim = (const char*)cc.p["animWicket"];
      if (rAnim.isEmpty() && cc.p["animResult"].is<const char*>()) rAnim = (const char*)cc.p["animResult"];
    }
  }
  url.trim();
  int idleMin = cfg["cric"]["idleMin"] | 60;
  int liveSec = 30;
  if (!cfg["cric"]["liveSec"].isNull()) liveSec = cfg["cric"]["liveSec"].as<int>();
  else if (!cfg["cric"]["liveMin"].isNull()) liveSec = cfg["cric"]["liveMin"].as<int>() * 60;
  if (liveSec < 5) liveSec = 5;
  UNLOCK();

  const char* curType = cardsCurrentType();
  bool isCurCric = !strcmp(curType, "cricket") || !strcmp(curType, "cricfix");
  if (!url.length() || !isCurCric) { nextCric = now + 60000; return; }

  while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
  if (url.endsWith("/api/cricket")) url = url.substring(0, url.length() - 12);
  if (url.endsWith("/api/emo/cricket")) url = url.substring(0, url.length() - 16);
  while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
  String reqUrl = url + "/api/emo/cricket/all";
  if (team.length()) {
    String encTeam = "";
    for (size_t i = 0; i < team.length(); i++) {
      char c = team[i];
      if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') encTeam += c;
      else if (c == ' ') encTeam += "%20";
      else if (c == ',') encTeam += ",";
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
        encTeam += buf;
      }
    }
    reqUrl += "?team=" + encTeam;
  }

  JsonDocument filter;
  JsonObject m = filter.add<JsonObject>();
  m["id"] = true;
  m["title"] = true;
  m["status"] = true;
  m["statusText"] = true;
  m["format"] = true;
  m["startTime"] = true;
  m["venue"] = true;
  m["series"] = true;
  m["target"] = true;
  m["bat1"] = true;
  m["bat2"] = true;
  m["bowl"] = true;
  m["crr"] = true;
  m["recent"] = true;
  m["rrr"] = true;
  m["remRuns"] = true;
  m["oversRem"] = true;
  m["partner"] = true;
  m["winProb"] = true;
  JsonObject t = m["teams"].add<JsonObject>();
  t["name"] = true;
  t["short"] = true;
  t["batting"] = true;
  JsonObject inn = t["innings"].add<JsonObject>();
  inn["runs"] = true;
  inn["wickets"] = true;
  inn["overs"] = true;

  JsonDocument doc;
  bool ok = httpJson(reqUrl, doc, &filter);
  if (!ok || !doc.is<JsonArray>()) {
    LOCK();
    safeCopy(cric.err, ok ? "API error" : "request failed", sizeof(cric.err));
    UNLOCK();
    nextCric = now + 15000UL;
    return;
  }
  CricData* nd = (CricData*)calloc(1, sizeof(CricData));
  if (!nd) {
    nextCric = now + 15000UL;
    return;
  }
  JsonArray arr = doc.as<JsonArray>();

  for (int pass = 0; pass < 5; pass++) {
    for (JsonObject m : arr) {
      if (nd->n >= 8) break;
      const char* mid = m["id"] | "";
      const char* mtitle = m["title"] | "";
      bool isTarget = false;
      if (targetMatchId.length() && mid[0] && !strcasecmp(mid, targetMatchId.c_str())) {
        isTarget = true;
      } else if (targetMatchTitle.length() && mtitle[0] &&
                 (icontains(mtitle, targetMatchTitle.c_str()) || icontains(targetMatchTitle.c_str(), mtitle))) {
        isTarget = true;
      }

      bool already = false;
      for (int k = 0; k < nd->n; k++) {
        if ((mid[0] && nd->m[k].id[0] && !strcasecmp(mid, nd->m[k].id)) ||
            (mtitle[0] && nd->m[k].name[0] && !strcasecmp(mtitle, nd->m[k].name))) {
          already = true;
          break;
        }
      }
      if (already) continue;

      const char* st = m["status"] | "";
      bool live = !strcmp(st, "live");
      const char* t1n = m["teams"][0]["name"] | "";
      const char* t2n = m["teams"][1]["name"] | "";
      const char* t1s = m["teams"][0]["short"] | "";
      const char* t2s = m["teams"][1]["short"] | "";
      bool mine = team.length() && (teamMatch(mtitle, team.c_str()) ||
                                    matchTeamItem(t1n, t1s, team.c_str()) ||
                                    matchTeamItem(t2n, t2s, team.c_str()));
      if ((pass == 0 && isTarget) ||
          (pass == 1 && mine && live) ||
          (pass == 2 && mine && !live) ||
          (pass == 3 && !mine && live) ||
          (pass == 4 && !mine && !live)) {
        parseMatch(m, nd->m[nd->n++]);
      }
    }
  }
  nd->hits = 0;
  nd->limit = 0;
  cricHits = max(cricHits, nd->hits);
  nd->ok = true;
  nd->ts = time(nullptr);

  bool anyLive = false;
  bool startingSoon = false;
  time_t curT = time(nullptr);

  LOCK();
  for (int i = 0; i < nd->n; i++) {
    CricMatch& nm = nd->m[i];
    if (cricLive(nm)) anyLive = true;
    else if (nm.start > 0) {
      double diff = difftime(nm.start, curT);
      if (diff >= -900 && diff <= 2400) startingSoon = true;
    }
  }

static char alertedWickets[12][48];
static uint8_t alertedWicketIdx = 0;
static char alertedResults[8][48];
static uint8_t alertedResultIdx = 0;

  // Check wicket and result alerts
  if (cric.ok) {
    bool hasTargetId = (targetMatchId.length() > 0);
    bool hasTargetTitle = (targetMatchTitle.length() > 0);
    bool hasTeam = (team.length() > 0);

    for (int i = 0; i < nd->n; i++) {
      CricMatch& nm = nd->m[i];

      // Filter: only alert for the match or team the user has configured
      if (hasTargetId) {
        if (!nm.id[0] || strcmp(nm.id, targetMatchId.c_str()) != 0) continue;
      } else if (hasTargetTitle) {
        if (!nm.name[0] || (!icontains(nm.name, targetMatchTitle.c_str()) && !icontains(targetMatchTitle.c_str(), nm.name))) continue;
      } else if (hasTeam) {
        bool involvesTeam = teamMatch(nm.t1n, team.c_str()) || teamMatch(nm.t1, team.c_str()) ||
                            teamMatch(nm.t2n, team.c_str()) || teamMatch(nm.t2, team.c_str()) ||
                            teamMatch(nm.name, team.c_str());
        if (!involvesTeam) continue;
      }

      // Safe matchOld lookup: match by ID first, then full name with qualifier protection
      int matchOld = -1;
      if (nm.id[0]) {
        for (int j = 0; j < cric.n; j++) {
          if (cric.m[j].id[0] && !strcmp(cric.m[j].id, nm.id)) {
            matchOld = j;
            break;
          }
        }
      }
      if (matchOld < 0 && nm.name[0]) {
        for (int j = 0; j < cric.n; j++) {
          if (cric.m[j].name[0] && !strcasecmp(cric.m[j].name, nm.name)) {
            matchOld = j;
            break;
          }
        }
      }
      if (matchOld < 0 && nm.t1n[0] && nm.t2n[0]) {
        for (int j = 0; j < cric.n; j++) {
          const CricMatch& om = cric.m[j];
          if (!isQualifierMismatch(nm.t1n, om.t1n) && !isQualifierMismatch(nm.t2n, om.t2n) &&
              !isQualifierMismatch(nm.name, om.name)) {
            if ((!strcasecmp(om.t1n, nm.t1n) && !strcasecmp(om.t2n, nm.t2n)) ||
                (!strcasecmp(om.t1n, nm.t2n) && !strcasecmp(om.t2n, nm.t1n))) {
              matchOld = j;
              break;
            }
          }
        }
      }
      if (matchOld < 0) continue;
      CricMatch& om = cric.m[matchOld];

      // 1. Wicket check across all innings
      bool wicketFell = false;
      int wktInn = -1;
      if (alW && cricLive(nm) && nm.ni > 0) {
        for (int k = 0; k < min((int)nm.ni, (int)om.ni); k++) {
          if (nm.inn[k].w > om.inn[k].w) {
            wicketFell = true;
            wktInn = k;
            break;
          }
        }
        if (!wicketFell && nm.ni > om.ni && nm.inn[nm.ni - 1].w > 0) {
          wicketFell = true;
          wktInn = nm.ni - 1;
        }
      }

      if (wicketFell && wktInn >= 0 && wktInn < nm.ni) {
        CricInn& in = nm.inn[wktInn];
        // Deduplication: prevent the exact same wicket in the same inning from alerting more than once
        char wktKey[48];
        snprintf(wktKey, sizeof(wktKey), "%s|%s:i%d:w%d", nm.t1, nm.t2, wktInn, in.w);
        bool alreadyAlerted = false;
        for (int a = 0; a < 12; a++) {
          if (!strcmp(alertedWickets[a], wktKey)) {
            alreadyAlerted = true;
            break;
          }
        }
        if (!alreadyAlerted) {
          safeCopy(alertedWickets[alertedWicketIdx], wktKey, sizeof(alertedWickets[0]));
          alertedWicketIdx = (alertedWicketIdx + 1) % 12;

          bool ours = false;   // did OUR team lose the wicket?
          if (team.length()) {
            if (!strcmp(in.team, nm.t1)) ours = teamMatch(nm.t1n, team.c_str()) || teamMatch(nm.t1, team.c_str());
            else if (!strcmp(in.team, nm.t2)) ours = teamMatch(nm.t2n, team.c_str()) || teamMatch(nm.t2, team.c_str());
          }
          char ovStr[10];
          formatCricOvers(in.o, ovStr, sizeof(ovStr));
          char b[60];
          snprintf(b, sizeof(b), "%s %d/%d (%s)", in.team, in.r, in.w, ovStr);
          logf("[CRIC] Wicket fell in %s: %s", nm.name, b);
          notify("WICKET!", b, ours ? E_SAD : E_EXCITED, ours ? "sfx:error" : "sfx:goal", EV_SPORTS, sp, 12, NK_NORMAL, -1, wAnim.isEmpty() ? nullptr : wAnim.c_str());
          if (cfg["tg"]["cric"] | false) {
            char tb[80]; snprintf(tb, sizeof(tb), "\U0001F3CF Wicket: %s", b);
            safeCopy(tgPending, tb, sizeof(tgPending));
          }
        }
      }

      // 2. Result check
      if (alR && om.started && !om.ended && nm.ended) {
        char resKey[48];
        snprintf(resKey, sizeof(resKey), "%s|%s:ended", nm.t1, nm.t2);
        bool resAlerted = false;
        for (int a = 0; a < 8; a++) {
          if (!strcmp(alertedResults[a], resKey)) {
            resAlerted = true;
            break;
          }
        }
        if (!resAlerted) {
          safeCopy(alertedResults[alertedResultIdx], resKey, sizeof(alertedResults[0]));
          alertedResultIdx = (alertedResultIdx + 1) % 8;

          logf("[CRIC] Match ended: %s (%s)", nm.name, nm.status);
          notify("RESULT", nm.status, E_HAPPY, "sfx:goal", EV_SPORTS, sp, 15, NK_NORMAL, -1, rAnim.isEmpty() ? nullptr : rAnim.c_str());
          if (cfg["tg"]["cric"] | false) {
            char tb[80]; snprintf(tb, sizeof(tb), "\U0001F3C6 Result: %s", nm.status);
            safeCopy(tgPending, tb, sizeof(tgPending));
          }
        }
      }
    }
  }

  memcpy(&cric, nd, sizeof(CricData));
  UNLOCK();
  free(nd);
  uint32_t liveMs = (uint32_t)liveSec * 1000UL;
  uint32_t idleMs = (uint32_t)idleMin * 60000UL;
  uint32_t activeIdleMs = min(idleMs, 60000UL);
  uint32_t interval = anyLive ? liveMs : (startingSoon ? 120000UL : (isCurCric ? activeIdleMs : idleMs));
  nextCric = now + interval * powerScale();
}

static int64_t tgOffset = 0;

static const char TG_KEYBOARD[] =
  "{\"keyboard\":["
    "[{\"text\":\"/status\"},{\"text\":\"/screen\"},{\"text\":\"/brief\"}],"
    "[{\"text\":\"/next\"},{\"text\":\"/prev\"},{\"text\":\"/home\"}],"
    "[{\"text\":\"/feed\"},{\"text\":\"/pet\"},{\"text\":\"/sleep\"}],"
    "[{\"text\":\"/volup\"},{\"text\":\"/voldown\"},{\"text\":\"/mute\"}],"
    "[{\"text\":\"/cards\"},{\"text\":\"/read\"},{\"text\":\"/rotate\"}],"
    "[{\"text\":\"/wake\"},{\"text\":\"/refresh\"},{\"text\":\"/help\"}]"
  "],\"resize_keyboard\":true}";

static void tgSendTo(const String& tok, const String& chat, const char* text, const char* replyMarkup = nullptr) {
  String url = "https://api.telegram.org/bot" + tok + "/sendMessage?chat_id=" + chat + "&text=" + urlEncode(text);
  if (replyMarkup && *replyMarkup) {
    url += "&reply_markup=" + urlEncode(replyMarkup);
  }
  httpGet(url, nullptr);
}

void tgQueue(const char* text) { LOCK(); safeCopy(tgPending, text, sizeof(tgPending)); UNLOCK(); }
void tgSendNow(const char* text) { tgQueue(text); nextTg = 0; }

static void tgLoop(uint32_t now) {
  LOCK();
  bool en = cfg["tg"]["en"] | false;
  String tok = cfg["tg"]["token"] | "";
  String chat = cfg["tg"]["chat"] | "";
  int poll = cfg["tg"]["poll"] | 8;
  bool ack = cfg["tg"]["ack"] | true;
  UNLOCK();
  if (!en || !tok.length() || (int32_t)(now - nextTg) < 0) return;
  nextTg = now + max(poll, 3) * 1000UL * powerScale();
  if (tgPending[0] && chat.length()) {
    char t[256];
    LOCK(); safeCopy(t, tgPending, sizeof(t)); tgPending[0] = 0; UNLOCK();
    tgSendTo(tok, chat, t);
  }
  JsonDocument filter;
  JsonObject r = filter["result"].add<JsonObject>();
  r["update_id"] = true;
  r["message"]["text"] = true;
  r["message"]["chat"]["id"] = true;
  r["message"]["from"]["first_name"] = true;
  JsonDocument doc;
  char url[40];
  snprintf(url, sizeof(url), "%lld", (long long)tgOffset);
  if (!httpJson("https://api.telegram.org/bot" + tok + "/getUpdates?limit=5&timeout=0&offset=" + url, doc, &filter)) return;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    tgOffset = u["update_id"].as<int64_t>() + 1;
    String text = u["message"]["text"] | "";
    int64_t cid = u["message"]["chat"]["id"] | (int64_t)0;
    String from = u["message"]["from"]["first_name"] | "Telegram";
    char cids[24];
    snprintf(cids, sizeof(cids), "%lld", (long long)cid);
    if (!cid || !text.length()) continue;
    if (!chat.length()) {
      chat = cids;
      LOCK(); cfg["tg"]["chat"] = chat; UNLOCK();
      markDirty(DIRTY_CFG);
      logf("Telegram paired with chat %s", cids);
      tgSendTo(tok, chat, "🎉 Paired with EmoDesk! Type /help or /menu for dashboard controls.", TG_KEYBOARD);
    }
    if (chat != cids) continue;
    String lc = text;
    lc.trim();
    lc.toLowerCase();
    auto arg = [&](int n) { String a = text.substring(n); a.trim(); return a; };

    // --- Help / Menu ---
    if (lc.startsWith("/start") || lc.startsWith("/help") || lc.startsWith("/menu")) {
      tgSendTo(tok, chat,
        "🎮 EmoDesk Dashboard Controls:\n\n"
        "📊 Status & Screen:\n"
        "/status - Full system, battery, screen & mood stats\n"
        "/screen - Read current screen text\n"
        "/screen off or /screen on - Display off/on\n"
        "/screentoggle - Toggle display power\n"
        "/brief - Run morning briefing\n\n"
        "🎴 Cards & Navigation:\n"
        "/next or /prev - Switch card\n"
        "/home - Go to home card\n"
        "/cards - List all cards\n"
        "/goto <n> - Jump to card number\n"
        "/read - Speak current card text aloud\n"
        "/refresh - Force refresh feeds & card\n"
        "/rotate [on|off] - Toggle card auto-rotation\n"
        "/profile [name] - View/switch profile\n\n"
        "🐾 Pet & Mood:\n"
        "/feed - Feed Emo 🍎\n"
        "/pet or /play - Play with Emo 🎾\n"
        "/sleep - Sleep & turn screen off 😴🌑\n"
        "/wake - Wake up & turn screen on 👀💡\n"
        "/poke - Poke Emo 👉\n"
        "/resetpet - Reset pet stats\n"
        "/emo <mood> - Set mood (happy, sad, love, angry, excited, cool...)\n\n"
        "🔊 Audio:\n"
        "/say <text> - Speak aloud (TTS)\n"
        "/vol [0-100] - Get/set volume\n"
        "/volup or /voldown - Volume +/- 10%\n"
        "/mute or /unmute - Mute sound\n"
        "/play <name> - Play sound or melody\n"
        "/noise [type] - Sleep noise (white, rain...)\n"
        "/stop - Stop sound\n\n"
        "💡 Display & Power:\n"
        "/bright <0-255> - Set screen brightness\n"
        "/screen off or /screen on - Display off/on\n"
        "/screentoggle - Toggle display\n"
        "/saver [on|off|auto] - Battery saver\n"
        "/deepsleep - Deep sleep now\n"
        "/reboot - Restart EmoDesk\n\n"
        "💬 Messages:\n"
        "/notify <title> | <text> - Popup alert\n"
        "Or send any text to display on screen!",
        TG_KEYBOARD
      );
    }
    // --- Status ---
    else if (lc == "/status" || lc == "status") {
      char b[460];
      int bat = batteryPercent();
      char batS[36];
      if (bat >= 0) snprintf(batS, sizeof(batS), "%d%% (%.2fV)%s", bat, batteryMv() / 1000.0f, powerSaver() ? " [Saver]" : "");
      else strcpy(batS, "No battery");
      char petLine[60] = "";
      if (emoActive()) {
        snprintf(petLine, sizeof(petLine), "❤️ Joy: %d%% | ⚡ Energy: %d%% | 🍔 Hunger: %d%%\n",
                 (int)pet.happy, (int)pet.energy, (int)pet.hunger);
      }
      snprintf(b, sizeof(b),
        "🤖 Emo: %s\n"
        "%s"
        "💡 Screen: %s\n"
        "🔋 Battery: %s\n"
        "🎴 Card: %s (%s, #%d/%d)\n"
        "🔊 Volume: %d%%%s\n"
        "🌐 IP: %s (%d dBm)\n"
        "📡 BLE: %s\n"
        "⚙️ Profile: %s | Uptime: %lus",
        emoActive() ? EMO_NAMES[emoCurrent()] : "Disabled (no Emo card)",
        petLine,
        screenIsOn() ? "On 💡" : "Off 🌑",
        batS,
        cardName(cardsCurrent(), "Card").c_str(), cardsCurrentType(), cardsCurrent() + 1, cardsCount(),
        audioVolume(), audioMuted() ? " [Muted]" : "",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(),
        bleStatus().c_str(),
        (const char*)(cfg["profile"] | "Default"),
        (unsigned long)(millis() / 1000)
      );
      tgSendTo(tok, chat, b);
    }
    // --- Screen Reading & Display Power ---
    else if (lc == "/screenoff" || lc == "screenoff" || lc == "/screen off" || lc == "screen off" ||
             lc == "/displayoff" || lc == "displayoff" || lc == "/display off" || lc == "display off") {
      screenSetPower(false);
      tgSendTo(tok, chat, "Screen turned off 🌑");
    }
    else if (lc == "/screenon" || lc == "screenon" || lc == "/screen on" || lc == "screen on" ||
             lc == "/displayon" || lc == "displayon" || lc == "/display on" || lc == "display on") {
      screenSetPower(true);
      tgSendTo(tok, chat, "Screen turned on 💡");
    }
    else if (lc == "/screentoggle" || lc == "screentoggle" || lc == "/screen toggle" || lc == "screen toggle") {
      screenToggle();
      tgSendTo(tok, chat, screenIsOn() ? "Screen turned on 💡" : "Screen turned off 🌑");
    }
    else if (lc == "/screen" || lc == "screen") {
      String t = cardsCurrentText();
      tgSendTo(tok, chat, t.length() ? t.c_str() : "No text available for this screen.");
    }
    // --- Morning Briefing ---
    else if (lc == "/brief" || lc == "brief") {
      briefingNow();
      String bt = briefingText();
      if (bt.length()) tgSendTo(tok, chat, (String("☀️ Morning Briefing:\n") + bt).c_str());
      else tgSendTo(tok, chat, "☀️ Morning briefing played on EmoDesk.");
    }
    // --- Navigation ---
    else if (lc == "/next" || lc == "next") {
      cardsNext(1);
      if (ack) tgSendTo(tok, chat, (String("Next card ➡️ ") + cardName(cardsCurrent(), "Card")).c_str());
    }
    else if (lc == "/prev" || lc == "prev") {
      cardsNext(-1);
      if (ack) tgSendTo(tok, chat, (String("Previous card ⬅️ ") + cardName(cardsCurrent(), "Card")).c_str());
    }
    else if (lc == "/home" || lc == "home") {
      cardsHome();
      if (ack) tgSendTo(tok, chat, "Switched to Home 🏠");
    }
    else if (lc == "/cards" || lc == "cards") {
      String cl = "🎴 Cards list:\n";
      int count = cardsCount();
      int cur = cardsCurrent();
      for (int i = 0; i < count && cl.length() < 350; i++) {
        char line[50];
        snprintf(line, sizeof(line), "%s %d: %s (%s)\n", (i == cur ? "👉" : "  "), i + 1, cardName(i, "Card").c_str(), (i == cur ? "Active" : ""));
        cl += line;
      }
      cl += "\nUse /goto <number> to switch.";
      tgSendTo(tok, chat, cl.c_str());
    }
    else if (lc.startsWith("/goto ")) {
      int idx = arg(6).toInt();
      if (idx >= 1 && idx <= cardsCount()) {
        cardsGoto(idx - 1);
        String rep = String("Switched to card #") + String(idx) + ": " + cardName(idx - 1, "Card");
        tgSendTo(tok, chat, rep.c_str());
      } else {
        tgSendTo(tok, chat, "Invalid card number. Use /cards to see list.");
      }
    }
    else if (lc == "/read" || lc == "/speakcard") {
      cardsSpeak();
      if (ack) tgSendTo(tok, chat, "Reading current card aloud 📢");
    }
    else if (lc == "/refresh" || lc == "refresh") {
      cardRefreshNow(cardsCurrent());
      feedsRefresh();
      if (ack) tgSendTo(tok, chat, "Refreshed card and feeds 🔄");
    }
    else if (lc.startsWith("/rotate")) {
      String a = arg(7);
      bool r;
      if (a == "on" || a == "1" || a == "true") r = true;
      else if (a == "off" || a == "0" || a == "false") r = false;
      else r = !(cfg["rotate"]["auto"] | true);
      cfg["rotate"]["auto"] = r;
      markDirty(DIRTY_CFG);
      tgSendTo(tok, chat, r ? "Auto-rotation turned ON 🔄" : "Auto-rotation turned OFF ⏸️");
    }
    else if (lc.startsWith("/profile")) {
      String a = arg(8);
      if (a.length()) {
        cfg["profile"] = a;
        markDirty(DIRTY_CFG);
        tgSendTo(tok, chat, (String("Profile set to: ") + a).c_str());
      } else {
        doAction("profile");
        tgSendTo(tok, chat, (String("Active profile: ") + (const char*)(cfg["profile"] | "Default")).c_str());
      }
    }
    // --- Pet & Mood ---
    else if (lc == "/feed" || lc == "feed") {
      if (!emoActive()) tgSendTo(tok, chat, "Emo face card is not active.");
      else {
        emoFeed();
        tgSendTo(tok, chat, (String("Fed Emo 🍎 (Hunger: ") + String((int)pet.hunger) + "%)").c_str());
      }
    }
    else if (lc == "/pet" || lc == "/playpet" || lc == "pet" || (lc == "/play" && !arg(5).length())) {
      if (!emoActive()) tgSendTo(tok, chat, "Emo face card is not active.");
      else {
        emoPlay();
        tgSendTo(tok, chat, (String("Played with Emo 🎾 (Joy: ") + String((int)pet.happy) + "%)").c_str());
      }
    }
    else if (lc == "/sleep" || lc == "sleep") {
      if (emoActive()) emoSleepNow();
      screenSetPower(false);
      tgSendTo(tok, chat, emoActive() ? "Emo is now sleeping & screen turned off 😴🌑" : "Screen turned off (sleep) 🌑");
    }
    else if (lc == "/wake" || lc == "wake") {
      if (emoActive()) emoTouched();
      screenSetPower(true);
      tgSendTo(tok, chat, emoActive() ? "Emo woke up! 👀💡" : "Screen turned on 💡");
    }
    else if (lc == "/poke") {
      if (!emoActive()) tgSendTo(tok, chat, "Emo face card is not active.");
      else {
        emoPoke();
        tgSendTo(tok, chat, "Poked Emo! 👉");
      }
    }
    else if (lc == "/resetpet") {
      if (!emoActive()) tgSendTo(tok, chat, "Emo face card is not active.");
      else {
        emoResetStats();
        tgSendTo(tok, chat, "Pet stats reset to 100% ❤️");
      }
    }
    else if (lc.startsWith("/emo ")) {
      if (!emoActive()) tgSendTo(tok, chat, "Emo face card is not active.");
      else {
        String m = arg(5);
        int e = emoFromName(m.c_str());
        if (e >= 0) {
          emoTouched();
          emoSet(e, 8000);
          audioChirp(e, EV_UI);
          tgSendTo(tok, chat, (String("Mood: ") + EMO_NAMES[e] + " ✨").c_str());
        } else {
          tgSendTo(tok, chat, "Unknown mood. Try: happy, sad, angry, love, sleepy, surprised, bored, dizzy, confused, excited, cool");
        }
      }
    }
    // --- Audio ---
    else if (lc.startsWith("/say ")) {
      audioSay(arg(5), EV_UI);
      if (ack) tgSendTo(tok, chat, "Speaking 🗣️");
    }
    else if (lc == "/volup") {
      int v = min(100, audioVolume() + 10);
      audioSetVolume(v);
      tgSendTo(tok, chat, (String("Volume: ") + String(v) + "% 🔊").c_str());
    }
    else if (lc == "/voldown") {
      int v = max(0, audioVolume() - 10);
      audioSetVolume(v);
      tgSendTo(tok, chat, (String("Volume: ") + String(v) + "% 🔉").c_str());
    }
    else if (lc == "/vol") {
      tgSendTo(tok, chat, (String("Current volume: ") + String(audioVolume()) + "%").c_str());
    }
    else if (lc.startsWith("/vol ")) {
      int v = constrain(arg(5).toInt(), 0, 100);
      audioSetVolume(v);
      tgSendTo(tok, chat, (String("Volume set to: ") + String(v) + "% 🔊").c_str());
    }
    else if (lc == "/mute") {
      audioSetMuted(true);
      tgSendTo(tok, chat, "Audio muted 🔇");
    }
    else if (lc == "/unmute") {
      audioSetMuted(false);
      tgSendTo(tok, chat, "Audio unmuted 🔊");
    }
    else if (lc.startsWith("/noise")) {
      String a = arg(6);
      int type = 0;
      if (a.startsWith("pink")) type = 1;
      else if (a.startsWith("brown")) type = 2;
      else if (a.startsWith("rain")) type = 3;
      else if (a.startsWith("ocean") || a.startsWith("stream")) type = 4;
      audioNoise(type, 0);
      tgSendTo(tok, chat, "Playing ambient noise 🌧️");
    }
    else if (lc.startsWith("/play ")) {
      String s = arg(6);
      if (s.indexOf(":") < 0) s = "sfx:" + s;
      audioPlaySpec(s.c_str(), EV_UI);
      if (ack) tgSendTo(tok, chat, (String("Playing: ") + s).c_str());
    }
    else if (lc == "/sounds") {
      tgSendTo(tok, chat, "Sounds:\nsfx: beep, click, pop, msg, error, notify, goal, alarm, chime, startup, win, coin\nmel: 0 to 5\nnoise: white, pink, brown, rain, ocean");
    }
    else if (lc == "/stop") {
      audioStop();
      if (ack) tgSendTo(tok, chat, "Stopped sound ⏹️");
    }
    // --- Display & Power ---
    else if (lc.startsWith("/bright ")) {
      int b = constrain(arg(8).toInt(), 0, 255);
      displaySetContrast(b);
      cfg["display"]["contrast"] = b;
      markDirty(DIRTY_CFG);
      tgSendTo(tok, chat, (String("Brightness set to: ") + String(b)).c_str());
    }
    else if (lc == "/screenoff") {
      screenSetPower(false);
      tgSendTo(tok, chat, "Screen turned off 🌑");
    }
    else if (lc == "/screenon") {
      screenSetPower(true);
      tgSendTo(tok, chat, "Screen turned on 💡");
    }
    else if (lc.startsWith("/saver")) {
      String m = arg(6);
      if (m != "on" && m != "off" && m != "auto") m = powerSaver() ? "off" : "on";
      powerSetMode(m.c_str());
      tgSendTo(tok, chat, (String("Battery saver mode: ") + m).c_str());
    }
    else if (lc == "/reboot" || lc == "/restart") {
      tgSendTo(tok, chat, "Rebooting EmoDesk... 🔄");
      delay(500);
      ESP.restart();
    }
    else if (lc == "/deepsleep") {
      tgSendTo(tok, chat, "Going into deep sleep 💤 (Touch pad to wake)");
      delay(500);
      powerSleepNow(0, "Telegram");
    }
    // --- Custom Notifications & Text ---
    else if (lc.startsWith("/notify ")) {
      String n = arg(8);
      int sep = n.indexOf('|');
      String title = "Telegram", body = n;
      if (sep > 0) {
        title = n.substring(0, sep); title.trim();
        body = n.substring(sep + 1); body.trim();
      }
      notify(title.c_str(), body.c_str(), E_LOVE, "sfx:notify", EV_NOTIFY, true, 12);
      if (ack) tgSendTo(tok, chat, "Notification shown on screen 🔔");
    }
    else {
      rcPush(RC_MSG, from.c_str(), text.c_str());
      if (ack) tgSendTo(tok, chat, "Shown on EmoDesk ✉️");
    }
  }
}

// ------------------------------------------------------------------ MQTT
#if ENABLE_MQTT
static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);
static String mHost, mBase;
static uint32_t mqttTry = 0, mqttState = 0;
static volatile bool subsDirty = true;
struct MPub { char t[64]; char p[64]; };
static QueueHandle_t mpubQ = nullptr;

bool mqttConnected() { return mqtt.connected(); }
void mqttCardsChanged() { subsDirty = true; }

void mqttPublish(const char* topic, const char* payload) {
  if (!mpubQ) return;
  MPub p;
  safeCopy(p.t, topic, sizeof(p.t));
  safeCopy(p.p, payload, sizeof(p.p));
  xQueueSend(mpubQ, &p, 0);
}

const char* mqttGet(const char* topic) {
  for (int i = 0; i < mqttValN; i++) if (!strcmp(mqttVals[i].topic, topic)) return mqttVals[i].val;
  return "";
}

static void mqttCb(char* topic, byte* payload, unsigned int len) {
  char msg[200];
  unsigned n = min(len, (unsigned)199);
  memcpy(msg, payload, n);
  msg[n] = 0;
  String t(topic), pre = mBase + "/cmd/";
  if (t.startsWith(pre)) {
    String c = t.substring(pre.length());
    if (c == "message") rcPush(RC_MSG, "MQTT", msg);
    else if (c == "say") rcPush(RC_SAY, "MQTT", msg);
    else if (c == "emo") rcPush(RC_EMO, "MQTT", msg);
    else if (c == "play" || c == "sound") rcPush(RC_PLAY, "MQTT", msg);
    else if (c == "stop") rcPush(RC_STOP, "MQTT", msg);
    else if (c == "notify") rcPush(RC_NOTIFY, "MQTT", msg);
    return;
  }
  LOCK();
  for (int i = 0; i < mqttValN; i++) if (!strcmp(mqttVals[i].topic, topic)) safeCopy(mqttVals[i].val, msg, sizeof(mqttVals[i].val));
  UNLOCK();
}

static void subscribeCards() {
  subsDirty = false;
  MqttVal nv[12];
  uint8_t n = 0;
  LOCK();
  for (JsonObject c : cardsDoc["cards"].as<JsonArray>()) {
    if (strcmp(c["type"] | "", "mqtt")) continue;
    for (JsonObject it : c["p"]["items"].as<JsonArray>()) {
      const char* tp = it["t"] | "";
      if (!*tp || n >= 12) continue;
      safeCopy(nv[n].topic, tp, sizeof(nv[n].topic));
      safeCopy(nv[n].val, mqttGet(tp), sizeof(nv[n].val));
      if (!nv[n].val[0]) strcpy(nv[n].val, "--");
      n++;
    }
  }
  memcpy(mqttVals, nv, sizeof(nv));
  mqttValN = n;
  UNLOCK();
  for (int i = 0; i < n; i++) mqtt.subscribe(nv[i].topic);
}

static void mqttLoop(uint32_t now) {
  if (!mpubQ) mpubQ = xQueueCreate(8, sizeof(MPub));
  LOCK();
  bool en = cfg["mqtt"]["en"] | false;
  String host = cfg["mqtt"]["host"] | "";
  int port = cfg["mqtt"]["port"] | 1883;
  String user = cfg["mqtt"]["user"] | "", pass = cfg["mqtt"]["pass"] | "", base = cfg["mqtt"]["base"] | "emodesk";
  UNLOCK();
  if (!en || !host.length() || gSafeMode) { if (mqtt.connected()) mqtt.disconnect(); return; }
  if (!mqtt.connected()) {
    if (now - mqttTry < 15000 && mqttTry) return;
    mqttTry = now;
    mHost = host;
    mBase = base;
    mqtt.setServer(mHost.c_str(), port);
    mqtt.setCallback(mqttCb);
    mqtt.setBufferSize(1024);
    mqtt.setKeepAlive(60);
    String id = "emodesk-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    String will = base + "/status";
    bool ok = user.length() ? mqtt.connect(id.c_str(), user.c_str(), pass.c_str(), will.c_str(), 0, true, "offline")
                            : mqtt.connect(id.c_str(), will.c_str(), 0, true, "offline");
    if (!ok) { logf("MQTT connect failed (%d)", mqtt.state()); return; }
    mqtt.publish(will.c_str(), "online", true);
    mqtt.subscribe((base + "/cmd/#").c_str());
    subsDirty = true;
    logf("MQTT connected to %s", host.c_str());
  }
  mqtt.loop();
  if (subsDirty) subscribeCards();
  MPub p;
  while (xQueueReceive(mpubQ, &p, 0) == pdTRUE) mqtt.publish(p.t, p.p);
  if (now - mqttState > 60000 || !mqttState) {
    mqttState = now;
    char b[200];
    snprintf(b, sizeof(b), "{\"mood\":\"%s\",\"happy\":%d,\"energy\":%d,\"hunger\":%d,\"card\":\"%s\",\"vol\":%d,\"muted\":%s}",
             EMO_NAMES[emoCurrent()], (int)pet.happy, (int)pet.energy, (int)pet.hunger, cardsCurrentType(), audioVolume(),
             audioMuted() ? "true" : "false");
    mqtt.publish((base + "/state").c_str(), b, true);
  }
}
#else
bool mqttConnected() { return false; }
void mqttCardsChanged() {}
void mqttPublish(const char*, const char*) {}
const char* mqttGet(const char*) { return ""; }
static void mqttLoop(uint32_t) {}
#endif

// ------------------------------------------------------------------ loop
void feedsLoop() {
  uint32_t now = millis();
  const char* curType = cardsCurrentType();
  bool isCurWeather = !strcmp(curType, "weather") || !strcmp(curType, "forecast") || !strcmp(curType, "sunmoon");
  if ((int32_t)(now - nextHomeW) >= 0) {
    LOCK();
    bool has = !cfg["loc"]["lat"].isNull();
    float lat = cfg["loc"]["lat"] | 0.0f, lon = cfg["loc"]["lon"] | 0.0f;
    bool imp = cfg["units"] == "imperial";
    UNLOCK();
    if (has && isCurWeather) nextHomeW = now + (fetchWeather(lat, lon, imp, homeW) ? 15 * 60000UL * powerScale() : 60000UL);
    else nextHomeW = now + (isCurWeather ? 30000 : 60000);
  }
  cricLoop(now);
  tgLoop(now);
  mqttLoop(now);
}
