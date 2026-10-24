#include "cards.h"

static void bigTime(uint32_t ms, int y, bool tenths = false) {
  uint32_t s = ms / 1000;
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  else if (tenths) snprintf(b, sizeof(b), "%02lu:%02lu.%01lu", (unsigned long)(s / 60), (unsigned long)(s % 60), (unsigned long)(ms / 100 % 10));
  else snprintf(b, sizeof(b), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  u8g2.setFont(s >= 3600 || tenths ? F_NUM24 : F_NUM32);
  int w = u8g2.getStrWidth(b);
  u8g2.drawStr((128 - w) / 2, y, b);
}
static bool isVisible(CardCtx& c) { return cardsCurrent() == c.idx; }

// ================================================================== pomodoro
// st: 0 phase(0 focus,1 break,2 long) 1 running 2 end 3 sessions 4 remaining 5 last tick sec
static uint32_t pomoLen(CardCtx& c, int ph) {
  int m = ph == 0 ? (c.p["work"] | 25) : (ph == 1 ? (c.p["brk"] | 5) : (c.p["lbrk"] | 15));
  return max(1, m) * 60000UL;
}
static uint32_t pomoRem(CardCtx& c) {
  if (!c.st[1]) return c.st[4];
  int32_t r = (int32_t)(c.st[2] - millis());
  return r > 0 ? r : 0;
}
static void pomo_enter(CardCtx& c) { if (!c.st[1] && !c.st[4]) c.st[4] = pomoLen(c, c.st[0]); }
static void pomo_draw(CardCtx& c) {
  if (!c.st[1] && !c.st[4]) c.st[4] = pomoLen(c, c.st[0]);
  static const char* const PH[] = {"Focus", "Break", "Long break"};
  char b[24];
  snprintf(b, sizeof(b), "#%lu", (unsigned long)c.st[3] + (c.st[0] == 0 ? 1 : 0));
  drawHeader(PH[c.st[0]], c.st[1] ? b : "paused");
  uint32_t rem = pomoRem(c), len = pomoLen(c, c.st[0]);
  bigTime(rem, 44);
  drawBar(6, 52, 116, 8, 1.0f - (float)rem / len);
  if (c.st[1]) cardHold = c.p["hold"] | true;
}
static bool pomo_input(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE) {
    if (c.st[1]) { c.st[4] = pomoRem(c); c.st[1] = 0; }
    else { if (!c.st[4]) c.st[4] = pomoLen(c, c.st[0]); c.st[2] = millis() + c.st[4]; c.st[1] = 1; }
    audioSfx("click", EV_TIMER);
    return true;
  }
  if (g == G_LONG) {
    if (c.st[1]) c.st[2] = millis();   // skip to end of phase
    else c.st[4] = pomoLen(c, c.st[0]);
    return true;
  }
  return false;
}
static void pomo_tick(CardCtx& c) {
  if (!c.st[1]) { emoHold(-1, 0); return; }
  uint32_t rem = pomoRem(c);
  if ((c.p["tick"] | false) && isVisible(c) && rem / 1000 != c.st[5]) { c.st[5] = rem / 1000; audioSfx("tick", EV_TIMER); }
  
  // Hold the "focused" expression only during the work phase
  if (c.st[0] == 0) emoHold(E_FOCUSED, 2000);
  else emoHold(-1, 0);

  if (rem > 0) return;
  if (c.st[0] == 0) {
    c.st[3]++;
    int every = c.p["every"] | 4;
    c.st[0] = (every > 0 && c.st[3] % every == 0) ? 2 : 1;
    emoHold(-1, 0);
    notify("Focus done", c.st[0] == 2 ? "Take a long break!" : "Time for a short break", E_EXCITED, "sfx:chime", EV_TIMER, true, 10);
  } else {
    c.st[0] = 0;
    notify("Break over", "Back to focus", E_NEUTRAL, "sfx:bell", EV_TIMER, true, 10);
  }
  c.st[4] = pomoLen(c, c.st[0]);
  if (c.p["auto"] | false) c.st[2] = millis() + c.st[4];
  else c.st[1] = 0;
}
static void pomo_speak(CardCtx& c, String& s) {
  uint32_t m = pomoRem(c) / 60000;
  s = numWords(m) + " minutes left" + (c.st[0] ? " of your break." : " to focus.");
}
const CardType CT_pomodoro = {"pomodoro", 0, 0, 0, pomo_enter, pomo_draw, pomo_input, nullptr, pomo_speak, pomo_tick};

// ================================================================== timer
// st: 0 state(0 idle 1 run 2 pause) 1 end 2 remaining 3 preset index 4 duration
static int timerPresets(CardCtx& c, int* out) {
  String s = c.p["presets"] | "1,3,5,10,15,25,30,60";
  int n = 0, start = 0;
  while (start < (int)s.length() && n < 12) {
    int e = s.indexOf(',', start);
    if (e < 0) e = s.length();
    int v = s.substring(start, e).toInt();
    if (v > 0) out[n++] = v;
    start = e + 1;
  }
  return n;
}
static uint32_t timerRem(CardCtx& c) {
  if (c.st[0] == 1) { int32_t r = (int32_t)(c.st[1] - millis()); return r > 0 ? r : 0; }
  return c.st[0] == 2 ? c.st[2] : c.st[4];
}
static void timer_draw(CardCtx& c) {
  if (!c.st[4]) c.st[4] = max(1, (int)(c.p["min"] | 5)) * 60000UL;
  drawTitle(c, "Timer", c.st[0] == 1 ? "running" : (c.st[0] == 2 ? "paused" : "2x start"));
  uint32_t rem = timerRem(c);
  bigTime(rem, 46);
  if (c.st[0]) drawBar(6, 53, 116, 7, 1.0f - (float)rem / c.st[4]);
  else { u8g2.setFont(F_TINY); textCenter(62, "hold: change minutes"); }
  if (c.st[0] == 1) cardHold = true;
}
static bool timer_input(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE) {
    if (c.st[0] == 0) { c.st[1] = millis() + c.st[4]; c.st[0] = 1; }
    else if (c.st[0] == 1) { c.st[2] = timerRem(c); c.st[0] = 2; }
    else { c.st[1] = millis() + c.st[2]; c.st[0] = 1; }
    audioSfx("click", EV_TIMER);
    return true;
  }
  if (g == G_LONG) {
    if (c.st[0]) c.st[0] = 0;
    else {
      int pr[12];
      int n = timerPresets(c, pr);
      if (n) { c.st[3] = (c.st[3] + 1) % n; c.st[4] = pr[c.st[3]] * 60000UL; }
    }
    return true;
  }
  return false;
}
static void timer_tick(CardCtx& c) {
  if (c.st[0] != 1 || timerRem(c) > 0) return;
  c.st[0] = 0;
  String snd = c.p["sound"] | "sfx:alarm";
  notify("Timer", "Time is up!", E_SURPRISED, snd.c_str(), EV_TIMER, false, 60, NK_ALARM);
}
static void timer_speak(CardCtx& c, String& s) { s = numWords(timerRem(c) / 60000) + " minutes on the timer."; }
const CardType CT_timer = {"timer", 0, 0, 0, nullptr, timer_draw, timer_input, nullptr, timer_speak, timer_tick};

// ================================================================== stopwatch
struct SwData { uint32_t laps[4]; uint8_t n; };
static uint32_t swNow(CardCtx& c) { return c.st[2] + (c.st[0] ? millis() - c.st[1] : 0); }
static void stopwatch_draw(CardCtx& c) {
  SwData* d = (SwData*)c.d;
  drawTitle(c, "Stopwatch", c.st[0] ? "running" : "2x start");
  bigTime(swNow(c), 38, true);
  u8g2.setFont(F_TINY);
  for (int i = 0; i < d->n; i++) {
    char b[24];
    uint32_t s = d->laps[i] / 1000;
    snprintf(b, sizeof(b), "L%d %02lu:%02lu.%lu", d->n - i, (unsigned long)(s / 60), (unsigned long)(s % 60),
             (unsigned long)(d->laps[i] / 100 % 10));
    u8g2.drawStr((i % 2) * 64, 50 + (i / 2) * 8, b);
  }
  if (c.st[0]) cardHold = true;
}
static bool stopwatch_input(CardCtx& c, uint8_t g) {
  SwData* d = (SwData*)c.d;
  if (g == G_DOUBLE) {
    if (c.st[0]) { c.st[2] = swNow(c); c.st[0] = 0; }
    else { c.st[1] = millis(); c.st[0] = 1; }
    audioSfx("click", EV_TIMER);
    return true;
  }
  if (g == G_LONG) {
    if (c.st[0]) { memmove(d->laps + 1, d->laps, 3 * sizeof(uint32_t)); d->laps[0] = swNow(c); if (d->n < 4) d->n++; }
    else { c.st[2] = 0; d->n = 0; }
    return true;
  }
  return false;
}
const CardType CT_stopwatch = {"stopwatch", sizeof(SwData), 0, 0, nullptr, stopwatch_draw, stopwatch_input, nullptr, nullptr, nullptr};

// ================================================================== alarms (managed in Alarms tab)
static time_t atLocal(int dayOff, int minutes) {
  struct tm t;
  if (!nowTm(t)) return 0;
  t.tm_mday += dayOff;
  t.tm_hour = minutes / 60;
  t.tm_min = minutes % 60;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  return mktime(&t);
}
static int nextAlarm(time_t& when) {
  int best = -1;
  when = 0;
  struct tm now;
  if (!nowTm(now)) return -1;
  JsonArray a = cfg["alarms"];
  int i = 0;
  for (JsonObject al : a) {
    int m = parseHM(al["t"] | "");
    if ((al["en"] | true) && m >= 0) {
      for (int d = 0; d < 8; d++) {
        int wd = (now.tm_wday + d) % 7;
        if (!dayAllowed(al["days"] | 127, wd)) continue;
        if (d == 0 && m <= now.tm_hour * 60 + now.tm_min) continue;
        time_t t = atLocal(d, m);
        if (!when || t < when) { when = t; best = i; }
        break;
      }
    }
    i++;
  }
  return best;
}
static void alarms_draw(CardCtx& c) {
  bool on = cfg["alarmsOn"] | true;
  drawTitle(c, "Alarms", on ? "on" : "OFF");
  time_t when;
  int i = on ? nextAlarm(when) : -1;
  if (i < 0) { drawCenteredMsg(on ? "No alarms set" : "Alarms are off", "2x tap: toggle all"); return; }
  JsonObject al = cfg["alarms"][i];
  struct tm t;
  localtime_r(&when, &t);
  char b[32];
  fmtHM(b, t.tm_hour, t.tm_min, cfgH24());
  u8g2.setFont(F_NUM24);
  u8g2.drawStr(4, 40, b);
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(90, 22, WDAY[t.tm_wday]);
  if (!cfgH24()) u8g2.drawStr(90, 32, t.tm_hour < 12 ? "AM" : "PM");
  long diff = (long)(when - time(nullptr));
  snprintf(b, sizeof(b), "in %ldh %02ldm", diff / 3600, diff / 60 % 60);
  u8g2.drawStr(90, 42, b);
  u8g2.setFont(F_MED);
  u8g2.drawUTF8(4, 58, al["label"] | "Alarm");
}
static bool alarms_input(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  bool on = !(cfg["alarmsOn"] | true);
  cfg["alarmsOn"] = on;
  markDirty(DIRTY_CFG);
  audioSfx(on ? "notify" : "pop", EV_UI);
  return true;
}
static void alarms_speak(CardCtx& c, String& s) {
  time_t when;
  if (nextAlarm(when) < 0) { s = "No alarms are set."; return; }
  struct tm t;
  localtime_r(&when, &t);
  s = String("Next alarm, ") + WDAY_LONG[t.tm_wday] + " at " + sayTime(t.tm_hour, t.tm_min) + ".";
}
const CardType CT_alarms = {"alarms", 0, 0, 0, nullptr, alarms_draw, alarms_input, nullptr, alarms_speak, nullptr};

// ================================================================== reminders
static int reminderNextMin(JsonObject r, struct tm& now, int& dayOff) {
  int nm = now.tm_hour * 60 + now.tm_min;
  for (int d = 0; d < 2; d++) {
    if (!dayAllowed(r["days"] | 127, (now.tm_wday + d) % 7)) continue;
    if (!strcmp(r["mode"] | "every", "at")) {
      int m = parseHM(r["at"] | "");
      if (m >= 0 && (d || m > nm)) { dayOff = d; return m; }
      continue;
    }
    int ev = max(5, (int)(r["every"] | 60));
    int f = parseHM(r["from"] | "09:00"), t = parseHM(r["to"] | "21:00");
    if (f < 0) f = 0;
    if (t <= f) t = 1440;
    for (int m = f + ev; m <= t; m += ev)
      if (d || m > nm) { dayOff = d; return m; }
  }
  return -1;
}
static void reminders_draw(CardCtx& c) {
  drawTitle(c, "Reminders");
  JsonArray a = cfg["reminders"];
  struct tm now;
  if (a.isNull() || !a.size()) { drawCenteredMsg("No reminders", "Add them in the dashboard"); return; }
  if (!nowTm(now)) { drawCenteredMsg("Syncing time..."); return; }
  int y = 20, shown = 0;
  for (JsonObject r : a) {
    if (!(r["en"] | true) || shown >= 5) continue;
    int d = 0, m = reminderNextMin(r, now, d);
    char b[12] = "--";
    if (m >= 0) fmtHM(b, m / 60, m % 60, cfgH24());
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(0, y, r["label"] | "Reminder");
    textRight(127, y, d ? (String("tmrw ") + b).c_str() : b);
    y += 10;
    shown++;
  }
}
const CardType CT_reminders = {"reminders", 0, 0, 0, nullptr, reminders_draw, nullptr, nullptr, nullptr, nullptr};

// ================================================================== to-do list
static void listRows(CardCtx& c, JsonArray items, int cursor, bool habits) {
  int n = items.size();
  int first = max(0, min(cursor - 2, n - 5));
  struct tm t;
  uint32_t today = nowTm(t) ? dayKey(t) : 0;
  for (int k = 0; k < 5 && first + k < n; k++) {
    JsonObject it = items[first + k];
    int y = 19 + k * 10;
    bool sel = first + k == cursor;
    bool done = habits ? ((it["l"] | 0) == (int)today) : (it["d"] | false);
    if (sel) { u8g2.drawBox(0, y - 8, 128, 10); u8g2.setDrawColor(0); }
    u8g2.drawFrame(2, y - 7, 7, 7);
    if (done) { u8g2.drawLine(3, y - 4, 5, y - 2); u8g2.drawLine(5, y - 2, 8, y - 7); }
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(12, y, it["t"] | "");
    if (done && !habits) u8g2.drawHLine(12, y - 3, min(110, (int)u8g2.getUTF8Width(it["t"] | "")));
    if (habits) {
      char b[8];
      snprintf(b, sizeof(b), "x%d", (int)(it["s"] | 0));
      textRight(126, y, b);
    }
    u8g2.setDrawColor(1);
  }
}
static void todo_draw(CardCtx& c) {
  JsonArray items = c.p["items"];
  int n = items.isNull() ? 0 : items.size(), done = 0;
  for (int i = 0; i < n; i++) if (items[i]["d"] | false) done++;
  char b[12];
  snprintf(b, sizeof(b), "%d/%d", done, n);
  drawTitle(c, "To-do", b);
  if (!n) { drawCenteredMsg("Nothing to do!", "Add tasks in the dashboard"); return; }
  if ((int)c.st[0] >= n) c.st[0] = 0;
  listRows(c, items, c.st[0], false);
}
static bool todo_input(CardCtx& c, uint8_t g) {
  JsonArray items = c.p["items"];
  int n = items.isNull() ? 0 : items.size();
  if (!n) return false;
  if (g == G_DOUBLE) { c.st[0] = (c.st[0] + 1) % n; audioSfx("click", EV_TOUCH); return true; }
  if (g == G_LONG) {
    JsonObject it = items[c.st[0] % n];
    bool d = !(it["d"] | false);
    it["d"] = d;
    markDirty(DIRTY_CARDS);
    int done = 0;
    for (int i = 0; i < n; i++) if (items[i]["d"] | false) done++;
    if (done == n) { emoSet(E_EXCITED, 3000); audioSfx("win", EV_TOUCH); }
    else { if (d) emoSet(E_HAPPY, 1500); audioSfx("pop", EV_TOUCH); }
    return true;
  }
  return false;
}
static void todo_speak(CardCtx& c, String& s) {
  int left = 0;
  String first;
  for (JsonObject it : c.p["items"].as<JsonArray>())
    if (!(it["d"] | false)) { if (!left) first = it["t"] | ""; left++; }
  s = left ? "You have " + numWords(left) + " tasks left. Next: " + first + "." : "All tasks are done. Great job!";
}
const CardType CT_todo = {"todo", 0, 0, 0, nullptr, todo_draw, todo_input, nullptr, todo_speak, nullptr};

// ================================================================== habits
static uint32_t yesterdayKey() {
  time_t y = time(nullptr) - 86400;
  struct tm t;
  localtime_r(&y, &t);
  return dayKey(t);
}
static void habits_draw(CardCtx& c) {
  JsonArray items = c.p["items"];
  int n = items.isNull() ? 0 : items.size();
  drawTitle(c, "Habits", "streak");
  if (!n) { drawCenteredMsg("No habits yet", "Add them in the dashboard"); return; }
  if (!timeOK()) { drawCenteredMsg("Syncing time..."); return; }
  uint32_t yk = yesterdayKey();
  struct tm t;
  nowTm(t);
  uint32_t tk = dayKey(t);
  for (JsonObject it : items) {   // broken streaks drop to zero
    uint32_t l = it["l"] | 0;
    if (l && l != tk && l != yk && (it["s"] | 0) != 0) { it["s"] = 0; markDirty(DIRTY_CARDS); }
  }
  if ((int)c.st[0] >= n) c.st[0] = 0;
  listRows(c, items, c.st[0], true);
}
static bool habits_input(CardCtx& c, uint8_t g) {
  JsonArray items = c.p["items"];
  int n = items.isNull() ? 0 : items.size();
  if (!n || !timeOK()) return false;
  if (g == G_DOUBLE) { c.st[0] = (c.st[0] + 1) % n; audioSfx("click", EV_TOUCH); return true; }
  if (g == G_LONG) {
    JsonObject it = items[c.st[0] % n];
    struct tm t;
    nowTm(t);
    uint32_t tk = dayKey(t), l = it["l"] | 0;
    int s = it["s"] | 0;
    if (l == tk) { it["s"] = max(0, s - 1); it["l"] = yesterdayKey(); audioSfx("pop", EV_TOUCH); }
    else {
      it["s"] = (l == yesterdayKey()) ? s + 1 : 1;
      it["l"] = tk;
      emoSet(E_HAPPY, 2000);
      audioSfx("coin", EV_TOUCH);
    }
    markDirty(DIRTY_CARDS);
    return true;
  }
  return false;
}
const CardType CT_habits = {"habits", 0, 0, 0, nullptr, habits_draw, habits_input, nullptr, nullptr, nullptr};

// ================================================================== water
static int waterCount(CardCtx& c) {
  struct tm t;
  if (!nowTm(t)) return c.p["count"] | 0;
  if ((uint32_t)(c.p["day"] | 0) != dayKey(t)) { c.p["day"] = dayKey(t); c.p["count"] = 0; markDirty(DIRTY_CARDS); }
  return c.p["count"] | 0;
}
static void water_draw(CardCtx& c) {
  int goal = max(1, (int)(c.p["goal"] | 8)), n = waterCount(c);
  drawTitle(c, "Water");
  int x0 = 14, x1 = 46, top = 14, bot = 61;
  u8g2.drawLine(x0, top, x0 + 4, bot);
  u8g2.drawLine(x1, top, x1 - 4, bot);
  u8g2.drawLine(x0 + 4, bot, x1 - 4, bot);
  float f = min(1.0f, (float)n / goal);
  int lvl = bot - (int)((bot - top - 2) * f);
  for (int y = lvl; y < bot; y++) {
    float k = (float)(y - top) / (bot - top);
    int a = x0 + 1 + (int)(4 * k), b = x1 - 1 - (int)(4 * k);
    u8g2.drawHLine(a, y, b - a);
  }
  if (f > 0 && f < 1) {
    u8g2.setDrawColor(0);
    for (int x = x0 + 3; x < x1 - 3; x += 4) u8g2.drawPixel(x + (millis() / 200) % 4, lvl);
    u8g2.setDrawColor(1);
  }
  char b[12];
  snprintf(b, sizeof(b), "%d/%d", n, goal);
  u8g2.setFont(F_NUM24);
  u8g2.drawStr(60, 42, b);
  u8g2.setFont(F_SMALL);
  u8g2.drawStr(62, 54, n >= goal ? "goal reached!" : "glasses");
  u8g2.setFont(F_TINY);
  u8g2.drawStr(62, 63, "2x +1  hold -1");
}
static bool water_input(CardCtx& c, uint8_t g) {
  int n = waterCount(c), goal = c.p["goal"] | 8;
  if (g == G_DOUBLE) {
    c.p["count"] = ++n;
    markDirty(DIRTY_CARDS);
    if (n == goal) { emoSet(E_EXCITED, 3000); audioSfx("win", EV_TOUCH); }
    else { emoSet(E_HAPPY, 1200); audioSfx("drop", EV_TOUCH); }
    return true;
  }
  if (g == G_LONG && n > 0) { c.p["count"] = n - 1; markDirty(DIRTY_CARDS); return true; }
  return false;
}
static void water_speak(CardCtx& c, String& s) {
  s = "You drank " + numWords(waterCount(c)) + " of " + numWords(c.p["goal"] | 8) + " glasses today.";
}
const CardType CT_water = {"water", 0, 0, 0, nullptr, water_draw, water_input, nullptr, water_speak, nullptr};

// ================================================================== note
static void note_draw(CardCtx& c) {
  const char* t = c.p["text"] | "Write a note in the dashboard";
  bool big = c.p["big"] | false;
  u8g2.setFont(big ? F_HELV14 : F_MED);
  int lh = big ? 17 : 11, maxL = 64 / lh;
  int lines = wrapText(0, 0, 124, lh, t, 0);
  int skip = 0;
  if (lines > maxL) { skip = ((millis() - c.t0) / 2500) % (lines - maxL + 2); skip = min(skip, lines - maxL); }
  int y0 = lines < maxL ? (64 - lines * lh) / 2 + lh - 3 : lh - 3;
  wrapText(2 + burnX, y0, 124, lh, t, maxL, skip);
}
static void note_speak(CardCtx& c, String& s) { s = c.p["text"] | ""; }
const CardType CT_note = {"note", 0, 0, 0, nullptr, note_draw, nullptr, nullptr, note_speak, nullptr};

// ================================================================== calendar events (iCal)
struct CalEv { char s[44]; time_t t; bool allDay; };
struct CalData { bool ok; uint8_t n; CalEv e[6]; };

static int readLine(Stream& s, char* out, int cap, uint32_t& budget) {
  int n = 0;
  uint32_t idle = millis();
  while (budget > 0 && millis() - idle < 4000) {
    int ch = s.read();
    if (ch < 0) { delay(2); continue; }
    idle = millis();
    budget--;
    if (ch == '\n') { out[n] = 0; return n; }
    if (ch != '\r' && n < cap - 1) out[n++] = ch;
  }
  out[n] = 0;
  return n ? n : -1;
}
static time_t icsTime(const char* line, bool& allDay) {
  const char* v = strrchr(line, ':');
  if (!v) return 0;
  v++;
  int y, mo, d, h = 0, mi = 0, s = 0;
  if (sscanf(v, "%4d%2d%2d", &y, &mo, &d) != 3) return 0;
  const char* T = strchr(v, 'T');
  allDay = !T;
  if (T) sscanf(T + 1, "%2d%2d%2d", &h, &mi, &s);
  if (T && v[strlen(v) - 1] == 'Z') return makeUTC(y, mo, d, h, mi, s);
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; t.tm_isdst = -1;
  return mktime(&t);
}
static bool calevents_fetch(JsonObject p, void* d) {
  String url = p["url"] | "";
  if (!url.length()) return false;
  if (url.startsWith("webcal://")) url = "https://" + url.substring(9);
  CalData nd;
  memset(&nd, 0, sizeof(nd));
  time_t now = time(nullptr);
  int code = httpGet(url, [&](Stream& s) {
    char line[160];
    uint32_t budget = 500000;
    bool in = false;
    CalEv cur;
    while (readLine(s, line, sizeof(line), budget) >= 0) {
      if (!strncmp(line, "BEGIN:VEVENT", 12)) { in = true; memset(&cur, 0, sizeof(cur)); }
      else if (!strncmp(line, "END:VEVENT", 10) && in) {
        in = false;
        if (!cur.t || cur.t < now - (cur.allDay ? 86400 : 0) || cur.t > now + 45 * 86400L) continue;
        int pos = nd.n;
        while (pos > 0 && nd.e[pos - 1].t > cur.t) pos--;
        if (pos >= 6) continue;
        int last = min((int)nd.n, 5);
        for (int k = last; k > pos; k--) nd.e[k] = nd.e[k - 1];
        nd.e[pos] = cur;
        if (nd.n < 6) nd.n++;
      } else if (in && !strncmp(line, "DTSTART", 7)) cur.t = icsTime(line, cur.allDay);
      else if (in && !strncmp(line, "SUMMARY", 7)) {
        const char* v = strchr(line, ':');
        if (v) {
          char* o = cur.s;
          for (const char* q = v + 1; *q && o - cur.s < (int)sizeof(cur.s) - 1; q++) { if (*q == '\\' && q[1]) q++; *o++ = *q; }
          *o = 0;
        }
      }
    }
    return true;
  });
  if (code != 200) return false;
  nd.ok = true;
  *(CalData*)d = nd;
  return true;
}
static void evWhen(const CalEv& e, char* b, size_t n) {
  struct tm t, now;
  localtime_r(&e.t, &t);
  nowTm(now);
  char hm[8] = "";
  if (!e.allDay) fmtHM(hm, t.tm_hour, t.tm_min, cfgH24());
  const char* day = t.tm_yday == now.tm_yday ? "Today" : (t.tm_yday == (now.tm_yday + 1) % 366 ? "Tmrw" : WDAY[t.tm_wday]);
  snprintf(b, n, "%s %s", day, e.allDay ? "all day" : hm);
}
static void calevents_draw(CardCtx& c) {
  CalData* cd = (CalData*)c.d;
  if (!(c.p["url"] | "")[0]) { drawCenteredMsg("Paste an iCal (.ics) link", "in the card settings"); return; }
  if (!dataGate(cd->ok, "calendar")) return;
  drawTitle(c, "Agenda");
  if (!cd->n) { drawCenteredMsg("Nothing coming up"); return; }
  for (int i = 0; i < min((int)cd->n, 4); i++) {
    int y = 20 + i * 12;
    char b[24];
    evWhen(cd->e[i], b, sizeof(b));
    u8g2.setFont(F_TINY);
    u8g2.drawStr(0, y - 1, b);
    u8g2.setFont(F_SMALL);
    marquee(46, y, 82, cd->e[i].s, c.t0, 25);
  }
}
static void calevents_tick(CardCtx& c) {
  CalData* cd = (CalData*)c.d;
  if (!cd->ok || !cd->n) return;
  int am = c.p["alert"] | 10;
  if (am <= 0) return;
  time_t now = time(nullptr);
  for (int i = 0; i < cd->n; i++) {
    CalEv& e = cd->e[i];
    long diff = (long)(e.t - now);
    if (e.allDay || diff <= 0 || diff > am * 60L || c.st[0] == (uint32_t)e.t) continue;
    c.st[0] = (uint32_t)e.t;
    char b[64];
    snprintf(b, sizeof(b), "%s in %ld min", e.s, (diff + 59) / 60);
    notify("Coming up", b, E_SURPRISED, "sfx:chime", EV_REMIND, true, 15);
  }
}
static void calevents_speak(CardCtx& c, String& s) {
  CalData* cd = (CalData*)c.d;
  if (!cd->ok || !cd->n) { s = "Your calendar is clear."; return; }
  struct tm t;
  localtime_r(&cd->e[0].t, &t);
  s = String("Next: ") + cd->e[0].s + (cd->e[0].allDay ? "" : (", at " + sayTime(t.tm_hour, t.tm_min)).c_str()) + ".";
}
const CardType CT_calevents = {"calevents", sizeof(CalData), 1800, 0, nullptr, calevents_draw, nullptr, calevents_fetch, calevents_speak, calevents_tick};

// ================================================================== last message
static void message_draw(CardCtx& c) {
  if (!lastMsg.text[0]) { drawCenteredMsg("No messages yet", "Telegram, MQTT or dashboard"); return; }
  char b[40] = "";
  long ago = timeOK() && lastMsg.t ? (long)(time(nullptr) - lastMsg.t) / 60 : -1;
  if (ago >= 0) snprintf(b, sizeof(b), ago < 60 ? "%ldm ago" : "%ldh ago", ago < 60 ? ago : ago / 60);
  drawHeader(lastMsg.from[0] ? lastMsg.from : "Message", b);
  u8g2.setFont(F_MED);
  int lines = wrapText(0, 0, 124, 11, lastMsg.text, 0);
  int skip = lines > 4 ? min((int)(((millis() - c.t0) / 2500) % (lines - 2)), lines - 4) : 0;
  wrapText(2, 21, 124, 11, lastMsg.text, 4, skip);
}
static bool message_input(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE && lastMsg.text[0]) { audioSay(lastMsg.text, EV_UI); return true; }
  if (g == G_LONG) { lastMsg.text[0] = 0; return true; }
  return false;
}
static void message_speak(CardCtx& c, String& s) { if (lastMsg.text[0]) s = String(lastMsg.from) + " says: " + lastMsg.text; }
const CardType CT_message = {"message", 0, 0, 0, nullptr, message_draw, message_input, nullptr, message_speak, nullptr};

// ================================================================== MQTT / Home Assistant
static void mqtt_draw(CardCtx& c) {
  JsonArray items = c.p["items"];
  int n = items.isNull() ? 0 : items.size();
  drawTitle(c, "Home", mqttConnected() ? "" : "offline");
  if (!n) { drawCenteredMsg("Add MQTT entities", "in the card settings"); return; }
  if ((int)c.st[0] >= n) c.st[0] = 0;
  int first = max(0, min((int)c.st[0] - 2, n - 5));
  for (int k = 0; k < 5 && first + k < n; k++) {
    JsonObject it = items[first + k];
    int y = 19 + k * 10;
    bool sel = first + k == (int)c.st[0] && (it["c"] | "")[0];
    if (sel) { u8g2.drawFrame(0, y - 8, 128, 10); }
    u8g2.setFont(F_SMALL);
    u8g2.drawUTF8(2, y, it["l"] | (it["t"] | "?"));
    String v = String(mqttGet(it["t"] | "")) + (it["u"] | "");
    textRight(125, y, v.c_str());
  }
}
static bool mqtt_input(CardCtx& c, uint8_t g) {
  JsonArray items = c.p["items"];
  int n = items.isNull() ? 0 : items.size();
  if (!n) return false;
  if (g == G_DOUBLE) { c.st[0] = (c.st[0] + 1) % n; return true; }
  if (g == G_LONG) {
    JsonObject it = items[c.st[0] % n];
    const char* cmd = it["c"] | "";
    if (!*cmd) return false;
    const char* on = it["on"] | "ON";
    const char* off = it["off"] | "OFF";
    bool isOn = !strcasecmp(mqttGet(it["t"] | ""), on);
    mqttPublish(cmd, isOn ? off : on);
    audioSfx("click", EV_UI);
    return true;
  }
  return false;
}
const CardType CT_mqtt = {"mqtt", 0, 0, 0, nullptr, mqtt_draw, mqtt_input, nullptr, nullptr, nullptr};

// ================================================================== PC stats / now playing (pc_companion.py)
static bool pcGate() {
  if (pcStats.ts && millis() - pcStats.ts < 15000) return true;
  drawCenteredMsg("PC not connected", "Run tools/pc_companion.py");
  return false;
}
static void pcstats_draw(CardCtx& c) {
  if (!pcGate()) return;
  const char* L[] = {"CPU", "RAM", "GPU"};
  float V[] = {pcStats.cpu, pcStats.ram, pcStats.gpu};
  float T[] = {pcStats.cpuT, 0, pcStats.gpuT};
  char b[16];
  for (int i = 0; i < 3; i++) {
    int y = 2 + i * 14;
    u8g2.setFont(F_SMALL);
    u8g2.drawStr(0, y + 8, L[i]);
    drawBar(20, y, 70, 10, V[i] / 100);
    snprintf(b, sizeof(b), T[i] > 0 ? "%d%% %dC" : "%d%%", (int)V[i], (int)T[i]);
    textRight(127, y + 8, b);
  }
  u8g2.setFont(F_TINY);
  snprintf(b, sizeof(b), "Up %.1f", pcStats.up);
  u8g2.drawStr(0, 62, b);
  snprintf(b, sizeof(b), "Down %.1f MB/s", pcStats.down);
  textRight(127, 62, b);
}
static void pcstats_speak(CardCtx& c, String& s) {
  if (pcStats.ts) s = "C P U at " + String((int)pcStats.cpu) + " percent. Memory at " + String((int)pcStats.ram) + " percent.";
}
const CardType CT_pcstats = {"pcstats", 0, 0, 0, nullptr, pcstats_draw, nullptr, nullptr, pcstats_speak, nullptr};

static void nowplaying_draw(CardCtx& c) {
  if (!pcGate()) return;
  if (!pcStats.song[0]) { drawCenteredMsg("Nothing playing"); return; }
  for (int i = 0; i < 12; i++) {
    int h = pcStats.playing ? 3 + (int)((sinf(millis() / 140.0f + i * 1.7f) + 1) * 6 + (esp_random() % 3)) : 2;
    u8g2.drawBox(4 + i * 10, 26 - h, 7, h);
  }
  u8g2.setFont(F_BOLD);
  marquee(0, 44, 128, pcStats.song, c.t0, 30);
  u8g2.setFont(F_SMALL);
  marquee(0, 58, 128, pcStats.artist, c.t0, 25);
}
static void nowplaying_speak(CardCtx& c, String& s) {
  if (pcStats.song[0]) s = String("Now playing ") + pcStats.song + " by " + pcStats.artist + ".";
}
const CardType CT_nowplaying = {"nowplaying", 0, 0, 0, nullptr, nowplaying_draw, nullptr, nullptr, nowplaying_speak, nullptr};
