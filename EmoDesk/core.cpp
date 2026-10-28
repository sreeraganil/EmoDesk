#include "core.h"
#include <stdarg.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>

JsonDocument cfg;
JsonDocument cardsDoc;
JsonDocument stateDoc;
SemaphoreHandle_t gLock;
PcStats pcStats;
MsgData lastMsg;
volatile bool otaBusy = false;
volatile bool gNetBusy = false;
bool gSafeMode = false;
UBaseType_t gNetStackFree = 0, gAudioStackFree = 0;

// ---- crash counter that survives resets (not power cycles) ----
#define CRASH_MAGIC 0xE3D0C0DEUL
static RTC_NOINIT_ATTR uint32_t rtcMagic;
static RTC_NOINIT_ATTR uint32_t rtcCrashes;
static esp_reset_reason_t resetWhy = ESP_RST_UNKNOWN;
static bool wdtOn = false;

static bool isCrash(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}
bool lastBootCrashed() { return isCrash(resetWhy); }
int bootCrashCount() { return (int)rtcCrashes; }
const char* resetReasonStr() {
  switch (resetWhy) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog (froze)";
    case ESP_RST_BROWNOUT: return "brownout (weak power)";
    case ESP_RST_DEEPSLEEP: return "woke from sleep";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}

size_t heapLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
bool heapOk(size_t needFree, size_t needBlock) { return ESP.getFreeHeap() >= needFree && heapLargest() >= needBlock; }

void wdtEnable(bool on) {
#if WDT_SECONDS > 0
  if (on == wdtOn) return;
  if (on) {
    esp_task_wdt_config_t c = {};
    c.timeout_ms = WDT_SECONDS * 1000;
    c.idle_core_mask = 0;
    c.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
    wdtOn = esp_task_wdt_add(NULL) == ESP_OK;
  } else {
    esp_task_wdt_delete(NULL);
    wdtOn = false;
  }
#endif
}
void wdtFeed() { if (wdtOn) esp_task_wdt_reset(); }

static QueueHandle_t notifQ, rcQ;
static uint8_t dirty = 0;
static uint32_t dirtyAt = 0;

const char* const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const WDAY_LONG[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const MONTH[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const MONTH_LONG[12] = {"January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December"};
const char* const EV_KEYS[EV_COUNT] = {"ui", "touch", "emo", "chime", "alarm", "timer", "remind",
                                       "sports", "msg", "game", "notify"};

// ------------------------------------------------------------------ files
bool loadJson(const char* path, JsonDocument& d) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) { logf("JSON %s: %s", path, e.c_str()); return false; }
  return true;
}

bool saveJson(const char* path, JsonDocument& d) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t written = serializeJson(d, f);
  f.close();
  if (written == 0) {
    LittleFS.remove(tmp);
    return false;
  }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

static void addCard(JsonArray arr, const char* type, uint16_t dur, bool en = true) {
  JsonObject c = arr.add<JsonObject>();
  char id[10];
  snprintf(id, sizeof(id), "c%06lx", (unsigned long)(esp_random() & 0xFFFFFF));
  c["id"] = id;
  c["type"] = type;
  c["en"] = en;
  c["dur"] = dur;
  c["days"] = 127;
  c["p"].to<JsonObject>();
}

void createDefaultCards() {
  cardsDoc.clear();
  JsonArray a = cardsDoc["cards"].to<JsonArray>();
  addCard(a, "emo", 30);
  addCard(a, "clock", 10);
  addCard(a, "weather", 10);
  addCard(a, "forecast", 10);
  addCard(a, "calendar", 8);
  addCard(a, "progress", 8);
  addCard(a, "quote", 15);
  addCard(a, "joke", 15, false);
  addCard(a, "pomodoro", 10);
  addCard(a, "timer", 10, false);
  addCard(a, "todo", 12);
  JsonArray items = a[a.size() - 1]["p"]["items"].to<JsonArray>();
  JsonObject t1 = items.add<JsonObject>(); t1["t"] = "Plan the day"; t1["d"] = false;
  JsonObject t2 = items.add<JsonObject>(); t2["t"] = "Drink water"; t2["d"] = false;
  addCard(a, "pet", 8);
  addCard(a, "flappy", 0, false);
  addCard(a, "matrix", 10, false);
  addCard(a, "netinfo", 8);
  addCard(a, "ble", 10, false);   // disabled until the user enables Bluetooth
}


void coreBegin() {
  resetWhy = esp_reset_reason();
  if (rtcMagic != CRASH_MAGIC || resetWhy == ESP_RST_POWERON) { rtcMagic = CRASH_MAGIC; rtcCrashes = 0; }
  if (isCrash(resetWhy)) rtcCrashes++;
  else rtcCrashes = 0;
  gSafeMode = rtcCrashes >= 3;
  gLock = xSemaphoreCreateRecursiveMutex();
  notifQ = xQueueCreate(8, sizeof(Notif));
  rcQ = xQueueCreate(6, sizeof(RCmd));
  if (!LittleFS.begin(true)) logf("LittleFS mount failed");
  LittleFS.mkdir("/img");
  LittleFS.mkdir("/anim");
  LittleFS.mkdir("/snd");
  if (!loadJson("/config.json", cfg)) { cfg.clear(); cfg["name"] = "Emo"; }
  if (!loadJson("/cards.json", cardsDoc) || !cardsDoc["cards"].is<JsonArray>()) {
    createDefaultCards();
    saveJson("/cards.json", cardsDoc);
  }
  if (!loadJson("/state.json", stateDoc)) stateDoc.clear();
  memset(&pcStats, 0, sizeof(pcStats));
  memset(&lastMsg, 0, sizeof(lastMsg));
  if (isCrash(resetWhy)) logf("Last reset: %s (%u in a row)", resetReasonStr(), (unsigned)rtcCrashes);
  if (gSafeMode) logf("SAFE MODE: Bluetooth, MQTT and the speaker stay off this boot");
}

void markDirty(uint8_t what) { dirty |= what; dirtyAt = millis(); }

void saveNow() {
  LOCK();
  if (dirty & DIRTY_CFG) saveJson("/config.json", cfg);
  if (dirty & DIRTY_CARDS) saveJson("/cards.json", cardsDoc);
  if (dirty & DIRTY_STATE) saveJson("/state.json", stateDoc);
  dirty = 0;
  UNLOCK();
}

void coreLoop() {
  if (dirty && millis() - dirtyAt > 1000) saveNow();
  // 90 s without a crash: the firmware is stable, forget earlier crashes
  if (rtcCrashes && millis() > 90000) rtcCrashes = 0;
}

// ------------------------------------------------------------------ log
#define LOG_LINES 32
#define LOG_LINE_LEN 96
static char logBuf[LOG_LINES][LOG_LINE_LEN];
static uint8_t logHead = 0, logCount = 0;

void logf(const char* fmt, ...) {
  char tmp[LOG_LINE_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  if (Serial && Serial.availableForWrite() >= (int)strlen(tmp) + 2) {
    Serial.println(tmp);
  }
#else
  if (Serial) {
    Serial.println(tmp);
  }
#endif
  struct tm t;
  char ts[10] = "--:--:--";
  if (nowTm(t)) strftime(ts, sizeof(ts), "%H:%M:%S", &t);
  LOCK();
  snprintf(logBuf[logHead], sizeof(logBuf[0]), "%s %s", ts, tmp);
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
  UNLOCK();
}

String logDump() {
  String s;
  s.reserve(logCount * 60);
  for (int i = 0; i < logCount; i++) {
    int idx = (logHead - logCount + i + LOG_LINES) % LOG_LINES;
    s += logBuf[idx];
    s += '\n';
  }
  return s;
}

// ------------------------------------------------------------------ time
bool timeOK() { return time(nullptr) > 1700000000; }

bool nowTm(struct tm& t) {
  time_t n = time(nullptr);
  if (n < 1700000000) return false;
  localtime_r(&n, &t);
  return true;
}

int nowMin() {
  struct tm t;
  if (!nowTm(t)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

int parseHM(const char* s) {
  if (!s || !*s) return -1;
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) < 2) return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

bool inWindow(int m, int from, int to) {
  if (m < 0 || from < 0 || to < 0) return false;
  if (from == to) return true;
  if (from < to) return m >= from && m < to;
  return m >= from || m < to;
}

bool dayAllowed(int mask, int wday) {
  int bit = (wday + 6) % 7;  // Mon=0 .. Sun=6
  return mask & (1 << bit);
}

uint32_t dayKey(const struct tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

static long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

time_t makeUTC(int y, int mo, int d, int h, int mi, int s) {
  return (time_t)(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s);
}

time_t parseISO(const char* s) {
  if (!s) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  int n = sscanf(s, "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
  if (n < 3) return 0;
  time_t t = makeUTC(y, mo, d, h, mi, sec);
  const char* p = s + 10;
  while (*p && *p != 'Z' && *p != '+' && !(*p == '-' && p > s + 12)) p++;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    sscanf(p + 1, "%d:%d", &oh, &om);
    long off = oh * 3600L + om * 60L;
    t += (*p == '+') ? -off : off;
  }
  return t;
}

void localIn(const char* posixTz, time_t t, struct tm& out) {
  String cur = cfg["time"]["tz"] | "UTC0";
  setenv("TZ", posixTz, 1);
  tzset();
  localtime_r(&t, &out);
  setenv("TZ", cur.c_str(), 1);
  tzset();
}

bool cfgH24() { return cfg["time"]["h24"] | false; }

void fmtHM(char* out, int h, int m, bool h24) {
  if (h24) sprintf(out, "%02d:%02d", h, m);
  else sprintf(out, "%d:%02d", (h % 12) ? h % 12 : 12, m);
}

// ------------------------------------------------------------------ speech text
static const char* const ONES[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                   "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                   "seventeen", "eighteen", "nineteen"};
static const char* const TENS[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

static String below1000(int n) {
  String s;
  if (n >= 100) { s += ONES[n / 100]; s += " hundred"; n %= 100; if (n) s += " "; }
  if (n >= 20) { s += TENS[n / 10]; if (n % 10) { s += " "; s += ONES[n % 10]; } }
  else if (n > 0 || s.length() == 0) s += ONES[n];
  return s;
}

String numWords(long n) {
  if (n < 0) return "minus " + numWords(-n);
  if (n < 1000) return below1000((int)n);
  String s;
  if (n >= 1000000000L) { s += below1000(n / 1000000000L) + " billion "; n %= 1000000000L; }
  if (n >= 1000000L) { s += below1000(n / 1000000L) + " million "; n %= 1000000L; }
  if (n >= 1000L) { s += below1000(n / 1000L) + " thousand "; n %= 1000L; }
  if (n) s += below1000((int)n);
  s.trim();
  return s;
}

String sayTime(int h, int m) {
  String s;
  if (cfgH24()) {
    s = numWords(h);
    if (m == 0) s += " hundred hours";
    else if (m < 10) s += " oh " + numWords(m);
    else s += " " + numWords(m);
    return s;
  }
  int h12 = h % 12 ? h % 12 : 12;
  s = numWords(h12);
  if (m == 0) s += " oh clock";
  else if (m < 10) s += " oh " + numWords(m);
  else s += " " + numWords(m);
  s += h < 12 ? " ay em" : " pee em";
  return s;
}

String ttsClean(const String& in) {
  String out;
  int n = in.length();
  for (int i = 0; i < n; i++) {
    char c = in[i];
    if (isdigit((unsigned char)c)) {
      long v = 0;
      int j = i;
      while (j < n && isdigit((unsigned char)in[j]) && j - i < 9) v = v * 10 + (in[j++] - '0');
      out += " " + numWords(v);
      if (j + 1 < n && in[j] == '.' && isdigit((unsigned char)in[j + 1])) {
        out += " point";
        j++;
        while (j < n && isdigit((unsigned char)in[j])) { out += " "; out += ONES[in[j++] - '0']; }
      } else if (j + 1 < n && in[j] == '/' && isdigit((unsigned char)in[j + 1])) {
        out += " for";   // cricket style 145/4 -> "145 for 4"
        j++;
      }
      out += " ";
      i = j - 1;
    } else if (c == '%') out += " percent ";
    else if (c == '&') out += " and ";
    else if (c == '+') out += " plus ";
    else if ((uint8_t)c == 0xC2 && i + 1 < n && (uint8_t)in[i + 1] == 0xB0) { out += " degrees "; i++; }
    else if (isalpha((unsigned char)c) || c == '\'') out += c;
    else if (c == '.' || c == ',' || c == '?' || c == '!') { out += c; out += ' '; }
    else out += ' ';
  }
  String r;
  bool sp = false;
  for (unsigned i = 0; i < out.length(); i++) {
    if (out[i] == ' ') { if (!sp && r.length()) r += ' '; sp = true; }
    else { r += out[i]; sp = false; }
  }
  r.trim();
  return r;
}

// ------------------------------------------------------------------ queues
void notify(const char* title, const char* text, int emo, const char* snd, uint8_t ev, bool speak,
            uint16_t secs, uint8_t kind, int16_t ref, const char* anim) {
  static char lastTitle[24] = "";
  static char lastText[64] = "";
  static uint32_t lastNotifyAt = 0;
  LOCK();
  uint32_t now = millis();
  if (kind != NK_ALARM && (now - lastNotifyAt < 8000) &&
      !strcmp(lastTitle, title ? title : "") &&
      !strcmp(lastText, text ? text : "")) {
    UNLOCK();
    return;
  }
  safeCopy(lastTitle, title ? title : "", sizeof(lastTitle));
  safeCopy(lastText, text ? text : "", sizeof(lastText));
  lastNotifyAt = now;
  UNLOCK();

  Notif n;
  memset(&n, 0, sizeof(n));
  safeCopy(n.title, title ? title : "", sizeof(n.title));
  safeCopy(n.text, text ? text : "", sizeof(n.text));
  safeCopy(n.snd, snd ? snd : "", sizeof(n.snd));
  if (anim && anim[0]) safeCopy(n.anim, anim, sizeof(n.anim));
  n.emo = emo; n.ev = ev; n.speak = speak; n.secs = secs; n.kind = kind; n.ref = ref;
  if (notifQ) xQueueSend(notifQ, &n, 0);
}

bool notifyPop(Notif& n) { return notifQ && xQueueReceive(notifQ, &n, 0) == pdTRUE; }

void rcPush(uint8_t type, const char* from, const char* arg) {
  RCmd c;
  memset(&c, 0, sizeof(c));
  c.type = type;
  safeCopy(c.from, from ? from : "", sizeof(c.from));
  safeCopy(c.arg, arg ? arg : "", sizeof(c.arg));
  if (rcQ) xQueueSend(rcQ, &c, 0);
}

bool rcPop(RCmd& c) { return rcQ && xQueueReceive(rcQ, &c, 0) == pdTRUE; }

// ------------------------------------------------------------------ misc
void safeCopy(char* dst, const char* src, size_t n) {
  if (!n) return;
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
  // never cut a UTF-8 sequence in half
  size_t l = strlen(dst);
  while (l > 0 && ((uint8_t)dst[l - 1] & 0xC0) == 0x80) l--;
  if (l > 0 && ((uint8_t)dst[l - 1] & 0xC0) == 0xC0) l--;
  if (l < strlen(dst) && strlen(src) >= n - 1) dst[l] = 0;
}

bool icontains(const char* hay, const char* needle) {
  if (!needle || !*needle) return true;
  if (!hay) return false;
  size_t nl = strlen(needle);
  for (const char* p = hay; *p; p++)
    if (strncasecmp(p, needle, nl) == 0) return true;
  return false;
}

static bool hasATeamQualifier(const char* s) {
  if (!s || !*s) return false;
  if (icontains(s, " team a") || icontains(s, " lions") || icontains(s, " emerging")) return true;
  for (const char* p = s; *p; p++) {
    if (*p == 'a' || *p == 'A') {
      bool leftB = (p == s || *(p - 1) == ' ' || *(p - 1) == '-');
      bool rightB = (*(p + 1) == '\0' || *(p + 1) == ' ' || *(p + 1) == '-' || *(p + 1) == ',' || *(p + 1) == ')');
      if (leftB && rightB) return true;
    }
  }
  return false;
}

bool isQualifierMismatch(const char* a, const char* b) {
  if (!a || !b) return false;
  bool aU19 = icontains(a, "u19") || icontains(a, "u-19") || icontains(a, "under-19") || icontains(a, "under 19") || icontains(a, "u 19") || icontains(a, "u23") || icontains(a, "under-23") || icontains(a, "youth");
  bool bU19 = icontains(b, "u19") || icontains(b, "u-19") || icontains(b, "under-19") || icontains(b, "under 19") || icontains(b, "u 19") || icontains(b, "u23") || icontains(b, "under-23") || icontains(b, "youth");
  if (aU19 != bU19) return true;

  bool aWom = icontains(a, "women") || icontains(a, "wmn") || icontains(a, " wom") || icontains(a, "woman") || icontains(a, "(w)");
  bool bWom = icontains(b, "women") || icontains(b, "wmn") || icontains(b, " wom") || icontains(b, "woman") || icontains(b, "(w)");
  if (aWom != bWom) return true;

  bool aA = hasATeamQualifier(a);
  bool bA = hasATeamQualifier(b);
  if (aA != bA) return true;

  return false;
}

bool matchTeamItem(const char* longName, const char* shortCode, const char* teamFilter) {
  if (!teamFilter || !*teamFilter) return false;
  if (longName && *longName && teamMatch(longName, teamFilter)) return true;
  if (shortCode && *shortCode) {
    if (longName && *longName && isQualifierMismatch(longName, teamFilter)) return false;
    return teamMatch(shortCode, teamFilter);
  }
  return false;
}

bool teamMatch(const char* target, const char* teamFilter) {
  if (!teamFilter || !*teamFilter) return false;
  if (!target || !*target) return false;
  const char* p = teamFilter;
  while (*p) {
    while (*p == ' ' || *p == ',' || *p == ';') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ',' && *p != ';') p++;
    const char* end = p;
    while (end > start && *(end - 1) == ' ') end--;
    size_t len = end - start;
    if (len > 0 && len < 40) {
      char token[40];
      memcpy(token, start, len);
      token[len] = '\0';
      if (!isQualifierMismatch(target, token)) {
        if (icontains(target, token) || icontains(token, target)) return true;
      }
    }
  }
  return false;
}

String fmtNum(double v) {
  char b[16];
  double a = fabs(v);
  if (a >= 1e9) snprintf(b, sizeof(b), "%.2fB", v / 1e9);
  else if (a >= 1e6) snprintf(b, sizeof(b), "%.2fM", v / 1e6);
  else if (a >= 1e4) snprintf(b, sizeof(b), "%.1fK", v / 1e3);
  else snprintf(b, sizeof(b), "%.0f", v);
  return String(b);
}

String fmtPrice(double v) {
  char b[20];
  double a = fabs(v);
  if (a >= 100000) snprintf(b, sizeof(b), "%.0f", v);
  else if (a >= 100) snprintf(b, sizeof(b), "%.1f", v);
  else if (a >= 1) snprintf(b, sizeof(b), "%.2f", v);
  else snprintf(b, sizeof(b), "%.4f", v);
  return String(b);
}

static int lastBatPct = -1;
static int lastBatRaw = -1;
static int lastBatMv = -1;
static uint32_t lastBatTime = 0;

// Single-cell LiPo: voltage -> charge (the curve is far from linear)
static int lipoPct(int mv) {
  int vMax = cfg["sys"]["batMax"] | 0;
  int vMin = cfg["sys"]["batMin"] | 0;
  if (vMax > 0 || vMin > 0) {
    if (vMax <= 0) vMax = 4200;
    if (vMin <= 0) vMin = 3300;
    if (vMax <= vMin) vMax = vMin + 1;
    if (mv >= vMax) return 100;
    if (mv <= vMin) return 0;
    return (mv - vMin) * 100 / (vMax - vMin);
  }
  static const int16_t V[] = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
  static const int8_t P[] = {0, 5, 10, 22, 32, 44, 54, 63, 78, 90, 100};
  if (mv <= V[0]) return 0;
  for (int i = 1; i < 11; i++)
    if (mv <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (mv - V[i - 1]) / (V[i] - V[i - 1]);
  return 100;
}

void updateBattery() {
#ifdef PIN_BATTERY
  uint32_t now = millis();
  static uint32_t interval = 5000;
  if (lastBatTime != 0 && now - lastBatTime < interval) return;
  int sec = cfg["sys"]["batInterval"] | 5;
  interval = (uint32_t)constrain(sec, 1, 600) * 1000UL;
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BATTERY);
  int mv = (int)(sum / 8) * 2;                 // 1:2 divider
  lastBatRaw = analogRead(PIN_BATTERY);
  if (mv < 2000) {                             // nothing connected
    lastBatPct = -1;
    lastBatMv = -1;
  } else {
    lastBatMv = lastBatMv < 0 ? mv : (mv + lastBatMv * 3) / 4;   // smooth out radio dips
    lastBatPct = lipoPct(lastBatMv);
  }
  lastBatTime = now;
#endif
}
int batteryPercent() {
  updateBattery();
  return lastBatPct;
}

int batteryRaw() {
  updateBattery();
  return lastBatRaw;
}

int batteryMv() {
  updateBattery();
  return lastBatMv;
}
