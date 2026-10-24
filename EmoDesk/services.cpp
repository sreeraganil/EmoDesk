#include "services.h"
#include "cards.h"
#include "power.h"
#include "feeds.h"
#include "emo.h"

static Notif cur;
static bool active = false;
static uint32_t shownAt = 0, snoozeAt = 0, wakeUntil = 0, ledUntil = 0, toastUntil = 0;
static int16_t snoozeRef = -1;
static bool manualOff = false;
static char toast[40];
static int lastMinute = -1;
static uint32_t briefDay = 0;

void screenSetPower(bool on) {
  manualOff = !on;
  if (on) {
    wakeUntil = millis() + 30000;
    displayPower(true);
  } else {
    wakeUntil = 0;
    displayPower(false);
  }
}

bool screenIsOn() {
  return displayIsOn() && !manualOff;
}

void screenToggle() {
  screenSetPower(!screenIsOn());
}

void displayWake() { screenSetPower(true); }
void ledBlink(int n) { ledUntil = millis() + n * 300; }
void toastShow(const char* t) { safeCopy(toast, t, sizeof(toast)); toastUntil = millis() + 1600; }
bool notifActive() { return active; }

void toastDraw() {
  if (millis() > toastUntil) return;
  u8g2.setFont(F_MED);
  int w = u8g2.getUTF8Width(toast) + 10;
  int x = (128 - w) / 2;
  u8g2.setDrawColor(0);
  u8g2.drawRBox(x - 1, 46, w + 2, 17, 4);
  u8g2.setDrawColor(1);
  u8g2.drawRBox(x, 47, w, 15, 4);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(x + 5, 58, toast);
  u8g2.setDrawColor(1);
}

void servicesBegin() {
  ledcAttach(PIN_LED, 5000, 8);
  ledcWrite(PIN_LED, LED_ACTIVE_LOW ? 255 : 0);
}

// ------------------------------------------------------------------ briefing
String briefingText() {
  struct tm t;
  String s;
  if (!nowTm(t)) return "Good morning!";
  s = String(t.tm_hour < 12 ? "Good morning" : (t.tm_hour < 18 ? "Good afternoon" : "Good evening")) + "! It is " +
      sayTime(t.tm_hour, t.tm_min) + ", " + WDAY_LONG[t.tm_wday] + " " + MONTH_LONG[t.tm_mon] + " " + numWords(t.tm_mday) + ". ";
  if (homeW.ok)
    s += "Weather: " + String((int)roundf(homeW.temp)) + " degrees and " + wmoText(homeW.code) + ", high " +
         String((int)roundf(homeW.dMax[0])) + ". ";
  int left = 0;
  for (JsonObject c : cardsDoc["cards"].as<JsonArray>()) {
    if (strcmp(c["type"] | "", "todo")) continue;
    for (JsonObject it : c["p"]["items"].as<JsonArray>()) if (!(it["d"] | false)) left++;
  }
  if (left) s += "You have " + numWords(left) + " tasks to do. ";
  if (lastMsg.text[0] && lastMsg.t && time(nullptr) - lastMsg.t < 43200) s += "You have a message from " + String(lastMsg.from) + ". ";
  s += "Have a great day!";
  return s;
}

void briefingNow() {
  String s = briefingText();
  notify("Briefing", s.c_str(), E_HAPPY, "sfx:chime", EV_NOTIFY, true, 20);
}

// ------------------------------------------------------------------ minute scheduler
static void fireAlarm(int idx, const char* label, const char* tone) {
  char t[40];
  struct tm tm;
  nowTm(tm);
  fmtHM(t, tm.tm_hour, tm.tm_min, cfgH24());
  String txt = String(label && *label ? label : "Wake up!");
  notify(t, txt.c_str(), E_SURPRISED, tone && *tone ? tone : "sfx:alarm", EV_ALARM, false, 300, NK_ALARM, idx);
  if (cfg["tg"]["alarm"] | false) {
    char b[80];
    snprintf(b, sizeof(b), "⏰ Alarm (%s): %s", t, txt.c_str());
    tgQueue(b);
  }
}

static void minuteTick(struct tm& t) {
  int m = t.tm_hour * 60 + t.tm_min;
  // alarms
  if (cfg["alarmsOn"] | true) {
    int i = 0;
    for (JsonObject a : cfg["alarms"].as<JsonArray>()) {
      if ((a["en"] | true) && dayAllowed(a["days"] | 127, t.tm_wday) && parseHM(a["t"] | "") == m) {
        fireAlarm(i, a["label"] | "", a["tone"] | "sfx:alarm");
        if (a["once"] | false) { a["en"] = false; markDirty(DIRTY_CFG); }
      }
      i++;
    }
  }
  // reminders
  for (JsonObject r : cfg["reminders"].as<JsonArray>()) {
    if (!(r["en"] | true) || !dayAllowed(r["days"] | 127, t.tm_wday)) continue;
    bool due;
    if (!strcmp(r["mode"] | "every", "at")) due = parseHM(r["at"] | "") == m;
    else {
      int ev = max(5, (int)(r["every"] | 60));
      int f = parseHM(r["from"] | "09:00"), to = parseHM(r["to"] | "21:00");
      if (f < 0) f = 0;
      if (to <= f) to = 1440;
      due = m > f && m <= to && (m - f) % ev == 0;
    }
    if (due) notify("Reminder", r["label"] | "Reminder", E_SURPRISED, r["tone"] | "sfx:chime", EV_REMIND, r["speak"] | true, 20);
  }
  // hourly chime
  if ((cfg["chime"]["en"] | false) && t.tm_min == 0) {
    int from = cfg["chime"]["from"] | 8, to = cfg["chime"]["to"] | 22;
    if (t.tm_hour >= from && t.tm_hour <= to) {
      const char* st = cfg["chime"]["style"] | "beep";
      int h12 = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
      if (!strcmp(st, "cuckoo")) for (int k = 0; k < min(h12, 6); k++) audioSfx("cuckoo", EV_CHIME);
      else if (!strcmp(st, "westminster")) audioMelody(5, EV_CHIME);
      else if (!strcmp(st, "speak")) audioSay("It is " + sayTime(t.tm_hour, 0) + ".", EV_CHIME);
      else if (!strcmp(st, "bell")) audioSfx("bell", EV_CHIME);
      else { audioSfx("beep", EV_CHIME); audioSfx("beep", EV_CHIME); }
      emoSet(E_SURPRISED, 800);
    }
  }
  // morning briefing
  if (cfg["brief"]["en"] | false) {
    if (parseHM(cfg["brief"]["time"] | "07:30") == m && dayAllowed(cfg["brief"]["days"] | 127, t.tm_wday) && briefDay != dayKey(t)) {
      briefDay = dayKey(t);
      briefingNow();
    }
  }
}

// ------------------------------------------------------------------ notification overlay
static void dismiss(bool snooze) {
  if (cur.kind == NK_ALARM) {
    audioStop();
    if (snooze) {
      int mins = cfg["alarmSnooze"] | 9;
      snoozeAt = millis() + mins * 60000UL;
      snoozeRef = cur.ref;
      char b[24];
      snprintf(b, sizeof(b), "Snooze %d min", mins);
      toastShow(b);
    } else {
      snoozeAt = 0;
      if (cur.ref >= 0 && (cfg["alarms"][cur.ref]["speak"] | true)) audioSay(briefingText(), EV_ALARM);
      emoSet(E_HAPPY, 3000);
    }
  }
  active = false;
}

void notifDraw() {
  uint32_t now = millis();
  if (cur.anim[0] && drawAnimFrame(cur.anim)) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(0, 50, 128, 14);
    u8g2.setDrawColor(1);
    u8g2.drawHLine(0, 50, 128);
    u8g2.setFont(F_SMALL);
    char line[64];
    if (cur.text[0]) snprintf(line, sizeof(line), "%s: %s", cur.title, cur.text);
    else safeCopy(line, cur.title, sizeof(line));
    int tw = u8g2.getUTF8Width(line);
    if (tw <= 124) {
      u8g2.drawUTF8((128 - tw) / 2, 61, line);
    } else {
      int off = ((now - shownAt) / 30) % (tw + 40);
      u8g2.drawUTF8(124 - off, 61, line);
    }
    return;
  }
  u8g2.drawRBox(0, 0, 128, 14, 3);
  u8g2.setDrawColor(0);
  u8g2.setFont(F_BOLD);
  u8g2.drawUTF8(5, 11, cur.title);
  u8g2.setFont(F_TINY);
  if (cur.kind == NK_ALARM) textRight(124, 11, "ALARM");
  else textRight(124, 11, "ALERT");
  u8g2.setDrawColor(1);

  if (cur.kind == NK_ALARM) {
    bool inv = (now / 500) % 2;
    if (inv) u8g2.drawRBox(4, 16, 120, 32, 2);
    u8g2.setDrawColor(inv ? 0 : 1);
    emoRender(64, 31, 0.48f);
    u8g2.setDrawColor(1);
    u8g2.setFont(F_MED);
    textCenter(54, cur.text);
    u8g2.setFont(F_TINY);
    textCenter(63, "tap: stop   hold: snooze");
    return;
  }
  emoRender(18, 38, 0.35f);
  drawDottedHLine(36, 17, 92);
  u8g2.setFont(F_SMALL);
  int lines = wrapText(0, 0, 88, 9, cur.text, 0);
  int skip = 0;
  if (lines > 5) {
    skip = ((now - shownAt) / 2000);
    skip = min(skip, lines - 5);
  }
  wrapText(38, 26, 88, 9, cur.text, 4, skip);
}

static void notifLoop() {
  uint32_t now = millis();
  if (snoozeAt && (int32_t)(now - snoozeAt) >= 0) {
    snoozeAt = 0;
    JsonObject a = cfg["alarms"][snoozeRef];
    fireAlarm(snoozeRef, a["label"] | "Snoozed alarm", a["tone"] | "sfx:alarm");
  }
  if (active) {
    uint32_t lim = cur.kind == NK_ALARM ? 300000UL : max(2, (int)cur.secs) * 1000UL;
    if (now - shownAt > lim && !(cur.speak && audioSpeaking())) dismiss(false);
    return;
  }
  if (!notifyPop(cur)) return;
  active = true;
  shownAt = now;
  displayWake();
  if (cur.emo >= 0) emoSet(cur.emo, max(2, (int)cur.secs) * 1000UL);
  if (cur.kind == NK_ALARM) { emoTouched(); audioAlarm(cur.snd); }
  else {
    if (cur.snd[0]) audioPlaySpec(cur.snd, cur.ev);
    if (cur.speak) audioSay(String(cur.title) + ". " + cur.text, cur.ev);
  }
  ledBlink(cur.kind == NK_ALARM ? 1 : 4);
  logf("Notify: %s - %.40s", cur.title, cur.text);
}

// ------------------------------------------------------------------ LED + display
static void ledLoop() {
  uint32_t now = millis();
  int bright = cfg["led"]["bright"] | 60, duty = 0;
  if (cfg["led"]["en"] | true) {
    if (active && cur.kind == NK_ALARM) duty = (now / 150) % 2 ? bright : 0;   // alarms flash even in saver
    else if (powerLedOff()) duty = 0;
    else if (now < ledUntil && (cfg["led"]["notify"] | true)) duty = ((ledUntil - now) / 150) % 2 ? bright : 0;
    else if (emoSleeping() && (cfg["led"]["breathe"] | true)) duty = (int)(bright * (sinf(now / 1500.0f) + 1) / 6);
    else if (cfg["led"]["beat"] | false) { uint32_t p = now % 1400; duty = (p < 80 || (p > 220 && p < 300)) ? bright : 0; }
  }
  static int last = -1;
  if (duty != last) { last = duty; ledcWrite(PIN_LED, LED_ACTIVE_LOW ? 255 - duty : duty); }
}

static void screenLoop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 500) return;
  last = now;
  int bright = cfg["display"]["bright"] | 200;
  int m = nowMin();
  const char* nfStr = cfg["display"]["nightFrom"] | (const char*)(cfg["emo"]["sleepFrom"] | "22:00");
  const char* ntStr = cfg["display"]["nightTo"] | (const char*)(cfg["emo"]["sleepTo"] | "07:00");
  int nFrom = parseHM(nfStr);
  int nTo = parseHM(ntStr);
  bool isNight = (m >= 0 && nFrom >= 0 && nTo >= 0) && inWindow(m, nFrom, nTo);

  if ((cfg["display"]["nightDim"] | true) && isNight)
    bright = cfg["display"]["nightBright"] | 10;
  bright = min(bright, (int)powerBrightCap());
  displaySetContrast(bright);
  bool off = manualOff;
  uint32_t idle = now - inputLastMs();
  if ((cfg["display"]["nightOff"] | false) && (emoSleeping() || isNight) && idle > 300000UL) off = true;
  int offIdle = cfg["display"]["offIdle"] | 0;
  if (offIdle > 0 && idle > offIdle * 60000UL) off = true;
  int saverIdle = powerOffIdleMin();   // battery saver: screen off sooner
  if (saverIdle > 0 && idle > saverIdle * 60000UL) off = true;
  if (active || (int32_t)(wakeUntil - now) > 0) off = false;
  displayPower(!off);
}

// ------------------------------------------------------------------ remote commands
static void playByName(const char* n) {
  if (strchr(n, ':')) { audioPlaySpec(n, EV_MSG); return; }
  int mi = melodyFind(n);
  if (mi >= 0) { audioMelody(mi, EV_MSG); return; }
  for (int i = 0; i < sfxCount(); i++) if (!strcasecmp(n, sfxName(i))) { audioSfx(sfxName(i), EV_MSG); return; }
  String p = String("/snd/") + n;
  if (!p.endsWith(".wav")) p += ".wav";
  if (LittleFS.exists(p)) audioFile(p.c_str(), EV_MSG);
}

static void rcLoop() {
  RCmd c;
  if (!rcPop(c)) return;
  switch (c.type) {
    case RC_MSG:
      safeCopy(lastMsg.from, c.from, sizeof(lastMsg.from));
      safeCopy(lastMsg.text, c.arg, sizeof(lastMsg.text));
      lastMsg.t = time(nullptr);
      lastMsg.count++;
      notify(c.from[0] ? c.from : "Message", c.arg, E_LOVE, "sfx:msg", EV_MSG, cfg["msgSpeak"] | false, 12);
      break;
    case RC_SAY: emoSet(E_HAPPY, 3000); audioSay(c.arg, EV_MSG); break;
    case RC_EMO: {
      int e = emoFromName(c.arg);
      if (e >= 0) { emoTouched(); emoSet(e, 6000); audioChirp(e, EV_MSG); }
      break;
    }
    case RC_PLAY: playByName(c.arg); break;
    case RC_STOP: audioStop(); break;
    case RC_NOTIFY: {
      JsonDocument d;
      if (deserializeJson(d, c.arg)) { notify("Notice", c.arg); break; }
      notify(d["title"] | "Notice", d["text"] | "", emoFromName(d["emo"] | ""), d["sound"] | "sfx:notify", EV_NOTIFY,
             d["speak"] | false, d["secs"] | 10);
      break;
    }
  }
}

// ------------------------------------------------------------------ gestures / actions
static const char* defTouch(uint8_t g) {
  switch (g) {
    case G_TAP: return "next";
    case G_DOUBLE: return "prev";
    case G_TRIPLE: return "home";
    case G_LONG: return "speak";
    case G_VLONG: return "mute";
  }
  return "none";
}
static const char* defBoot(uint8_t g) {
  switch (g) {
    case G_TAP: return "volup";
    case G_DOUBLE: return "voldown";
    case G_TRIPLE: return "profile";
    case G_LONG: return "screen";
    case G_VLONG: return "rotate";
  }
  return "none";
}

void doAction(const char* a) {
  logf("[ACTION] Run: %s", a);
  char b[32];
  if (!strcmp(a, "next")) { cardsNext(1); audioSfx("click", EV_TOUCH); }
  else if (!strcmp(a, "prev")) { cardsNext(-1); audioSfx("click", EV_TOUCH); }
  else if (!strcmp(a, "home")) { cardsHome(); audioSfx("pop", EV_TOUCH); }
  else if (!strcmp(a, "speak")) { if (audioSpeaking()) audioStop(); else cardsSpeak(); }
  else if (!strcmp(a, "mute")) { bool m = !audioMuted(); audioSetMuted(m); toastShow(m ? "Muted" : "Sound on"); if (!m) audioSfx("pop", EV_UI); }
  else if (!strcmp(a, "volup") || !strcmp(a, "voldown")) {
    int v = audioVolume() + (a[3] == 'u' ? 10 : -10);
    audioSetVolume(v);
    snprintf(b, sizeof(b), "Volume %d%%", audioVolume());
    toastShow(b);
    audioSfx("beep", EV_UI);
  }
  else if (!strcmp(a, "pet")) emoPlay();
  else if (!strcmp(a, "feed")) emoFeed();
  else if (!strcmp(a, "rotate")) {
    bool r = !(cfg["rotate"]["auto"] | true);
    cfg["rotate"]["auto"] = r;
    markDirty(DIRTY_CFG);
    toastShow(r ? "Auto-rotate on" : "Auto-rotate off");
  }
  else if (!strcmp(a, "profile")) {
    JsonArray pr = cfg["profiles"];
    if (pr.isNull() || !pr.size()) { toastShow("No profiles"); return; }
    const char* curP = cfg["profile"] | "Default";
    int idx = 0, n = pr.size();
    for (int i = 0; i < n; i++) if (!strcmp(pr[i] | "", curP)) idx = (i + 1) % n;
    String np = pr[idx] | "Default";
    cfg["profile"] = np;
    markDirty(DIRTY_CFG);
    toastShow(np.c_str());
    if (!cardVisible(cardsCurrent())) cardsHome();
  }
  else if (!strcmp(a, "screen")) { screenToggle(); }
  else if (!strcmp(a, "screen_off") || !strcmp(a, "screenoff")) { screenSetPower(false); }
  else if (!strcmp(a, "screen_on") || !strcmp(a, "screenon")) { screenSetPower(true); }
  else if (!strcmp(a, "sound")) { if (audioBusy() || audioContinuous()) audioStop(); else audioPlaySpec(cfg["audio"]["fav"] | "mel:0", EV_UI); }
  else if (!strncmp(a, "media_", 6)) {
    const char* key = a + 6;
    bleMedia(key);
    Serial.printf("::MEDIA:%s::\r\n", key);
    char mtoast[32];
    snprintf(mtoast, sizeof(mtoast), "PC: %s", key);
    toastShow(mtoast);
  }
  else if (!strcmp(a, "refresh")) {
    cardRefreshNow(cardsCurrent());
    feedsRefresh();
    cricRefresh();
    toastShow("Refreshing...");
    audioSfx("pop", EV_UI);
  }
  else if (!strcmp(a, "brief")) briefingNow();
  else if (!strncmp(a, "media_", 6)) bleMedia(a + 6);
  else if (!strcmp(a, "saver")) {
    bool on = !powerSaver();
    powerSetMode(on ? "on" : "off");
    toastShow(on ? "Battery saver on" : "Battery saver off");
  }
  else if (!strcmp(a, "sleep")) powerSleepNow(0, "Sleeping");
}

void handleGesture(uint8_t g, uint8_t src) {
  if (src == SRC_BOOT && !(cfg["boot"]["en"] | true)) return;
  if (g != G_PRESS && g != G_RELEASE) {
    logf("[INPUT] %s gesture: %s", src == SRC_TOUCH ? "Touch" : (src == SRC_BOOT ? "BOOT" : "Web"), gestureName(g));
  }
  if (src != SRC_WEB) emoTouched();
  if (!screenIsOn()) {
    screenSetPower(true);
    if (g != G_PRESS && g != G_RELEASE) return;
  }
  wakeUntil = millis() + 30000;
  if (active) {
    if (g == G_RELEASE) return;
    dismiss(g == G_LONG || g == G_VLONG);
    return;
  }
  if (g == G_PRESS || g == G_RELEASE) { cardsInput(g); return; }
  if (src == SRC_BOOT) { doAction(cfg["boot"][gestureName(g)] | defBoot(g)); return; }
  if (cardsInput(g)) return;
  doAction(cfg["gest"][gestureName(g)] | defTouch(g));
}

// ------------------------------------------------------------------ loop
void servicesLoop() {
  struct tm t;
  if (nowTm(t)) {
    int m = t.tm_hour * 60 + t.tm_min;
    if (m != lastMinute) {
      bool first = lastMinute < 0;
      lastMinute = m;
      if (!first) { LOCK(); minuteTick(t); UNLOCK(); }
    }
  }
  notifLoop();
  rcLoop();
  ledLoop();
  screenLoop();
  bleLoop();
}
