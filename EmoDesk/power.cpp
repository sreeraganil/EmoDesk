#include "power.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include "soc/soc_caps.h"
#include "display.h"
#include "emo.h"
#include "audio.h"
#include "input.h"
#include "net.h"
#include "services.h"
#include "feeds.h"

// Settings are cached once a second so the hot paths (render, LED, fetch) never touch JSON.
static struct {
  char mode[6];
  int at, fps, dim, offMin, crit, nFrom, nTo;
  bool cpu, slow, ble, led, quiet, low, night;
} P = {"off", 25, 10, 40, 2, 5, -1, -1, true, true, true, true, false, true, false};

static volatile bool saver = false;
static bool autoOn = false, lowWarned = false, booted = false;
static uint32_t lastEval = 0, critSince = 0, lastWeb = 0;
static int appliedCpu = 0, appliedPs = -1;

static void readCfg() {
  JsonObject o = cfg["power"];
  safeCopy(P.mode, o["saver"] | "off", sizeof(P.mode));
  P.at = constrain((int)(o["at"] | 25), 5, 80);
  P.fps = constrain((int)(o["fps"] | 10), 2, 30);
  P.dim = constrain((int)(o["dim"] | 40), 1, 255);
  P.offMin = o["offMin"] | 2;
  P.crit = o["critPct"] | 5;
  P.cpu = o["cpu"] | true;
  P.slow = o["slowNet"] | true;
  P.ble = o["ble"] | true;
  P.led = o["led"] | true;
  P.quiet = o["quiet"] | false;
  P.low = o["lowAlert"] | true;
  P.night = o["night"] | false;
  P.nFrom = parseHM(o["nFrom"] | "01:00");
  P.nTo = parseHM(o["nTo"] | "06:30");
}

bool powerSaver() { return saver; }
uint16_t powerFrameMs() { return saver ? 1000 / P.fps : FRAME_MS; }
uint8_t powerBrightCap() { return saver ? P.dim : 255; }
uint32_t powerScale() { return (saver && P.slow) ? 3 : 1; }
bool powerLedOff() { return saver && P.led; }
bool powerQuiet() { return saver && P.quiet; }
bool powerPauseBle() { return saver && P.ble; }
int powerOffIdleMin() { return saver ? P.offMin : 0; }
const char* powerModeName() { return P.mode; }
void powerActivity() { lastWeb = millis(); }

void powerSetMode(const char* m) {
  LOCK();
  cfg["power"]["saver"] = m;
  UNLOCK();
  markDirty(DIRTY_CFG);
  lastEval = 0;   // re-evaluate on the next loop
}

void powerApplyRadio() {
  int cpu = (saver && P.cpu) ? 80 : 160;   // Wi-Fi needs at least 80 MHz
  if (cpu != appliedCpu && setCpuFrequencyMhz(cpu)) {
    if (appliedCpu) logf("[POWER] CPU scaled to %d MHz", cpu);
    appliedCpu = cpu;
  }
  // Modem sleep must stay on while Bluetooth runs (shared radio), so never use WIFI_PS_NONE.
  if (netOnline() && !netPortal()) {
    int ps = saver ? 2 : 1;
    if (ps != appliedPs && WiFi.setSleep(saver ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM)) appliedPs = ps;
  } else appliedPs = -1;   // re-apply after the next connect
}

String powerWakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_GPIO: return "touch";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    default: return "";
  }
}

void powerSleepNow(uint32_t wakeAfterMin, const char* why) {
  logf("[POWER] Deep sleep: %s, wake by touch%s", why, wakeAfterMin ? " or timer" : "");
  audioStop();
  emoSaveState();
  saveNow();
  emoSleepNow();
  displayPower(true);
  u8g2.clearBuffer();
  emoRender(64, 22, 0.6f);
  u8g2.setFont(F_MED);
  textCenter(50, why);
  u8g2.setFont(F_TINY);
  if (wakeAfterMin) {
    char b[32];
    snprintf(b, sizeof(b), "touch to wake (auto in %lum)", (unsigned long)wakeAfterMin);
    textCenter(62, b);
  } else textCenter(62, "touch me to wake up");
  displaySend();
  // wait for the finger to leave the pad, otherwise we wake straight away
  uint32_t t0 = millis();
  while (digitalRead(PIN_TOUCH) && millis() - t0 < 4000) { wdtFeed(); delay(20); }
  delay(1500);
  displayPower(false);
  digitalWrite(PIN_AMP_SD, LOW);
  ledcWrite(PIN_LED, LED_ACTIVE_LOW ? 255 : 0);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TOUCH, ESP_GPIO_WAKEUP_GPIO_HIGH);
#endif
  if (wakeAfterMin) esp_sleep_enable_timer_wakeup((uint64_t)wakeAfterMin * 60ULL * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// Minutes until the next enabled alarm, or -1
static int nextAlarmIn(int m, const struct tm& t) {
  if (!(cfg["alarmsOn"] | true)) return -1;
  int best = -1;
  for (JsonObject a : cfg["alarms"].as<JsonArray>()) {
    if (!(a["en"] | true)) continue;
    int am = parseHM(a["t"] | "");
    if (am < 0) continue;
    int d = (am - m + 1440) % 1440;
    int wd = (t.tm_wday + (am <= m ? 1 : 0)) % 7;
    if (!dayAllowed(a["days"] | 127, wd)) continue;
    if (best < 0 || d < best) best = d;
  }
  return best;
}

static void nightCheck(uint32_t now) {
  if (!P.night || P.nFrom < 0 || P.nTo < 0 || otaBusy) return;
  if (now < 300000UL || now - inputLastMs() < 300000UL) return;    // 5 min awake after boot or a touch
  if (lastWeb && now - lastWeb < 300000UL) return;                  // dashboard in use
  if (notifActive() || audioBusy() || audioContinuous()) return;
  struct tm t;
  if (!nowTm(t)) return;
  int m = t.tm_hour * 60 + t.tm_min;
  if (!inWindow(m, P.nFrom, P.nTo)) return;
  int wake = (P.nTo - m + 1440) % 1440;
  int al = nextAlarmIn(m, t);
  if (al >= 0 && al < wake) wake = al - 2;   // be up and on Wi-Fi before the alarm
  if (wake < 5) return;
  powerSleepNow(wake, "Good night");
}

void powerBegin() {
  readCfg();
  String w = powerWakeReason();
  if (w.length()) logf("Woke from deep sleep (%s)", w.c_str());
}

void powerLoop() {
  uint32_t now = millis();
  if (lastEval && now - lastEval < 1000) return;
  lastEval = now;
  readCfg();
  int bat = batteryPercent();
  if (bat < 0) autoOn = false;
  else if (bat <= P.at) autoOn = true;
  else if (bat >= P.at + 5) autoOn = false;   // hysteresis, no flapping
  bool want = !gSafeMode && (!strcmp(P.mode, "on") || (!strcmp(P.mode, "auto") && autoOn));
  if (want != saver) {
    saver = want;
    logf("[POWER] Battery saver %s", saver ? "on" : "off");
    if (booted) toastShow(saver ? "Battery saver on" : "Battery saver off");
  }
  booted = true;
  powerApplyRadio();

  if (bat >= 0 && P.low) {
    if (!lowWarned && bat <= 15) {
      lowWarned = true;
      char b[48];
      snprintf(b, sizeof(b), "%d%% left. Please plug me in!", bat);
      notify("Battery low", b, E_SLEEPY, "sfx:error", EV_NOTIFY, false, 8);
      if (cfg["tg"]["bat"] | false) {
        char tb[64];
        snprintf(tb, sizeof(tb), "🔋 Low battery warning: %d%% left! Please plug EmoDesk in.", bat);
        tgQueue(tb);
      }
    } else if (lowWarned && bat >= 25) lowWarned = false;
  }
  if (bat >= 0 && P.crit > 0 && bat <= P.crit && now > 120000UL) {
    if (!critSince) critSince = now;
    else if (now - critSince > 60000UL) powerSleepNow(0, "Battery empty");
  } else critSince = 0;

  nightCheck(now);
}
