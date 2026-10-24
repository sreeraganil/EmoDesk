#include "services.h"
#include "emo.h"
#include "audio.h"
#include "net.h"
#include "power.h"

#if ENABLE_BLE
// In Arduino ESP32 Core 3.3.11, esp32-hal-alloc-ble-mem.h checks CONFIG_SOC_BLE_SUPPORTED
// (which is undefined on ESP32-C3) instead of SOC_BLE_SUPPORTED. Therefore _bleLibraryInUse
// stays false, causing initArduino() to call btMemRelease(BT_MODE_BLE) before setup() runs.
// Once released, BLEDevice::init() detects the released memory, fails, and returns false.
// Strongly overriding the weak bleInUse()/btInUse() prevents the core from ever releasing
// the BLE controller memory during boot.
extern "C" bool bleInUse(void) { return true; }
extern "C" bool btInUse(void)  { return true; }

#include <BLEDevice.h>
#include <BLEHIDDevice.h>

static char mode[10]   = "off";
static char target[40] = "";
static int  minRssi    = -80;
static volatile bool     present     = false;
static volatile bool     connected   = false;
static volatile uint32_t lastSeen    = 0;
static volatile bool     needReadv   = false;  // set by onDisconnect ISR, cleared by bleLoop
static bool     started  = false;
static bool     failed   = false;
static const char* why   = "";
static uint32_t startedAt = 0;
static BLEServer*        srv   = nullptr;
static BLEHIDDevice*     hid   = nullptr;
static BLECharacteristic* input = nullptr;
static TaskHandle_t      scanTaskHandle = nullptr;

#define BLE_GUARD 0xB1EB0070UL
static RTC_NOINIT_ATTR uint32_t bleGuard;  // set while Bluetooth starts, survives a crash

// Consumer-control HID report: next, prev, stop, play/pause, mute, vol+, vol-, home
static const uint8_t REPORT_MAP[] = {
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
  0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7, 0x09, 0xCD, 0x09, 0xE2, 0x09, 0xE9, 0x09, 0xEA, 0x0A, 0x23, 0x02,
  0x81, 0x02, 0xC0};

class ScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (!target[0] || d.getRSSI() < minRssi) return;
    if (!strcasecmp(d.getAddress().toString().c_str(), target) ||
        (d.haveName() && icontains(d.getName().c_str(), target)))
      lastSeen = millis();
  }
};

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { connected = true;  needReadv = false; }
  void onDisconnect(BLEServer*) override { connected = false; needReadv = true;  }
};

// Scan task stack budget:
//   BLEScan::start() deep-calls esp_gap_ble_apis which run on the BT controller task.
//   Our task itself only manages the loop, waits, and calls two BLE API functions.
//   Measured stack peak is well under 1 KB; 1536 bytes gives a comfortable 50 % margin
//   and saves ~1 KB of RAM versus the previous 2560-byte allocation.
#define SCAN_TASK_STACK 1536

static void scanTask(void* arg) {
  BLEScan* sc = (BLEScan*)arg;
  if (!sc) { scanTaskHandle = nullptr; vTaskDelete(NULL); return; }
  for (;;) {
    // Yield while a network or OTA operation is in progress (up to 30 s).
    uint32_t waited = 0;
    while ((gNetBusy || otaBusy) && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(500)); waited += 500; }

    // Every device heard during a scan is kept on the heap until clearResults().
    // Only scan when there is room for that list.
    // Threshold: 16 KB free total, 4 KB largest contiguous block.
    if (!powerPauseBle() && !otaBusy && heapOk(16000, 4000)) {
      sc->start(3, false);   // 3 s blocking — shorter = smaller result list on the heap
      sc->clearResults();    // release the list immediately
    }
    // Log the scan-task's own stack headroom once after the first scan completes.
    static bool hwmLogged = false;
    if (!hwmLogged) {
      hwmLogged = true;
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
      logf("BLE scan task stack HWM: %u B free of %u B", (unsigned)(hwm * sizeof(StackType_t)), (unsigned)SCAN_TASK_STACK);
    }
    vTaskDelay(pdMS_TO_TICKS(powerSaver() ? 60000 : 15000));
  }
}

static void bleFail(const char* reason) {
  failed = true;
  why    = reason;
  bleGuard = 0;  // a clean failure is not a crash
  logf("BLE: %s (%u KB free)", reason, (unsigned)(ESP.getFreeHeap() / 1024));
}

static void bleStart() {
  // Heap budget:
  //   NimBLE controller + host stack: ~45 KB heap
  //   Scan task stack (SCAN_TASK_STACK): ~1.5 KB
  //   BLE result list headroom:        ~4 KB
  //   Safety margin:                   ~14 KB
  // Total guard: 65 KB free / 25 KB largest contiguous block.
  if (!heapOk(65000, 25000)) {
    static uint32_t lastMemWarn = 0;
    if (millis() - lastMemWarn > 15000) {
      lastMemWarn = millis();
      logf("BLE waiting for memory: %u KB free (largest %u KB)",
           (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(heapLargest() / 1024));
    }
    return;  // don't set started=true; bleLoop() retries once memory is available
  }

  started = true;
  bleGuard = BLE_GUARD;
  startedAt = millis();

  String name = cfg["name"] | "Emo";
  if (name.length() > 12) name = name.substring(0, 12);

  wdtFeed();
  if (!BLEDevice::init(String("EmoDesk ") + name)) { bleFail("Bluetooth failed to start"); return; }
  wdtFeed();

  if (!strcmp(mode, "presence")) {
    BLEScan* sc = BLEDevice::getScan();
    if (!sc) { bleFail("Bluetooth scanner unavailable"); return; }
    static ScanCb scanCb;
    sc->setAdvertisedDeviceCallbacks(&scanCb, false);
    sc->setDuplicateFilter(true);  // skip repeat advertisements → fewer heap allocations
    sc->setActiveScan(false);      // passive: listen only, never transmit
    sc->setInterval(320);          // scan interval ms
    sc->setWindow(48);             // scan window ms → ~15% duty; Wi-Fi holds the radio otherwise
    if (xTaskCreate(scanTask, "blescan", SCAN_TASK_STACK, sc, 1, &scanTaskHandle) != pdPASS) {
      bleFail("no memory for the scan task");
      return;
    }
  } else if (!strcmp(mode, "remote")) {
    srv = BLEDevice::createServer();
    if (!srv) { bleFail("Bluetooth server unavailable"); return; }
    static SrvCb srvCb;
    srv->setCallbacks(&srvCb);
    hid = new BLEHIDDevice(srv);
    if (!hid) { bleFail("Bluetooth HID create failed"); return; }
    input = hid->inputReport(1);
    if (!input) { bleFail("Bluetooth HID setup failed"); return; }
    hid->manufacturer(String("EmoDesk"));
    hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
    hid->hidInfo(0x00, 0x01);
    // bond=true, mitm=false ("just works" pairing), SC=true (secure connections)
    BLESecurity::setAuthenticationMode(true, false, true);
    hid->reportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
    hid->startServices();
    BLEAdvertising* adv = srv->getAdvertising();
    if (adv) {
      adv->setAppearance(HID_KEYBOARD);
      adv->addServiceUUID(hid->hidService()->getUUID());
      adv->start();
    }
    int b = batteryPercent();
    hid->setBatteryLevel(b >= 0 ? b : 100);
  }

  bleGuard = 0;  // init succeeded cleanly — clear the crash guard
  logf("BLE %s started, %u KB free", mode, (unsigned)(ESP.getFreeHeap() / 1024));
}

// ------------------------------------------------------------------ public API

void bleBegin() {
  safeCopy(mode,   cfg["ble"]["mode"]   | "off", sizeof(mode));
  safeCopy(target, cfg["ble"]["target"] | "",    sizeof(target));
  minRssi = cfg["ble"]["rssi"] | -80;

  bool crashedLastTime = (bleGuard == BLE_GUARD) && lastBootCrashed();
  bleGuard = 0;

  if (!strcmp(mode, "off")) return;

  if (crashedLastTime) {
    LOCK();
    cfg["ble"]["crashed"] = true;
    UNLOCK();
    markDirty(DIRTY_CFG);
    logf("Bluetooth crashed while starting last time — it is off. Connect > Bluetooth > Try again.");
  }
  if (cfg["ble"]["crashed"] | false) { failed = true; why = "off after a crash (press Try again)"; return; }
  if (gSafeMode)                     { failed = true; why = "off (safe mode)"; return; }
}

void bleApply() {
  safeCopy(target, cfg["ble"]["target"] | "", sizeof(target));
  minRssi = cfg["ble"]["rssi"] | -80;

  const char* newMode = cfg["ble"]["mode"] | "off";

  // If the user re-enables BLE after a crash, clear the crash lock.
  if (strcmp(newMode, "off") != 0 && (cfg["ble"]["crashed"] | false)) {
    LOCK();
    cfg["ble"]["crashed"] = false;
    UNLOCK();
    markDirty(DIRTY_CFG);
  }
  if (!(cfg["ble"]["crashed"] | false)) {
    if (failed && !strcmp(why, "off after a crash (press Try again)")) {
      failed = false;
      why    = "";
    }
  }

  if (started) {
    // BLEDevice cannot be safely torn down and restarted after init(); a reboot is required
    // for mode changes once BLE has started.  We only update live-tunable parameters here.
    return;
  }

  safeCopy(mode, newMode, sizeof(mode));

  // If the user switched mode to "off" before BLE ever started, try to release the BLE
  // controller memory so it is available to the rest of the firmware.
  if (!strcmp(mode, "off") && !failed) {
    failed = true;
    why    = "off";
    // Override bleInUse/btInUse return true unconditionally, so deinit is a best-effort call:
    // on some core versions it is a no-op. Log either way.
    BLEDevice::deinit(true);
    logf("BLE deinit (mode set to off before start)");
  }
}

void bleMedia(const char* key) {
  if (!input || !connected) { toastShow("Remote not paired"); return; }
  static const char* const K[] = {"next","prev","stop","play","mute","volup","voldown","home"};
  uint8_t bit = 0;
  for (int i = 0; i < 8; i++) if (!strcmp(key, K[i])) { bit = 1 << i; break; }
  if (!bit) return;

  // Key-down
  uint8_t r = bit;
  input->setValue(&r, 1);
  input->notify();

  // Key-up: use FreeRTOS delay so the scheduler can run other tasks (including wdtFeed
  // in the main loop) rather than blocking the CPU with a bare delay().
  // 15 ms is enough for any host HID stack to register the key press.
  vTaskDelay(pdMS_TO_TICKS(15));

  r = 0;
  input->setValue(&r, 1);
  input->notify();
}

void bleLoop() {
  if (!strcmp(mode, "off") || failed) return;
  uint32_t now = millis();

  if (!started) {
    // Let Wi-Fi settle and grab its memory first, then start BLE.
    bool netReady = netOnline() || !netHasCreds() || now > 60000;
    if (now > 12000 && netReady && !gNetBusy && !otaBusy) bleStart();
    return;
  }

  // Remote (HID) mode: restart advertising after a disconnect, and refresh battery level.
  if (srv) {
    if (needReadv && now - startedAt > 500) {
      // Confirm enough time has passed since init before re-advertising.
      static uint32_t disconnectedAt = 0;
      if (!disconnectedAt) disconnectedAt = now;
      if (now - disconnectedAt > 500) {
        disconnectedAt = 0;
        needReadv = false;
        srv->startAdvertising();
      }
    }
    static uint32_t lastBat = 0;
    static int sentBat = -1;
    if (connected && hid && now - lastBat > 300000UL) {
      lastBat = now;
      int b = batteryPercent();
      if (b >= 0 && b != sentBat) { hid->setBatteryLevel(b); sentBat = b; }
    }
    return;
  }

  // Presence mode: derive present/away state from lastSeen timestamp.
  if (strcmp(mode, "presence") || powerPauseBle()) return;  // paused: keep last state

  static bool was = false;
  static uint32_t awaySince = 0;

  present = lastSeen && (now - lastSeen < 60000);

  if (present && !was) {
    screenSetPower(true);
    if (awaySince && now - awaySince > 5 * 60000UL && (cfg["ble"]["greet"] | true)) {
      String n = cfg["owner"] | "";
      notify("Welcome back!", n.length() ? ("Hi " + n + "!").c_str() : "I missed you!",
             emoActive() ? E_EXCITED : -1, "sfx:levelup", EV_NOTIFY, true, 8);
    }
  }
  if (!present && was) awaySince = now;
  if (!present && awaySince && (cfg["ble"]["awaySleep"] | false) &&
      now - awaySince > (uint32_t)(cfg["ble"]["awayMin"] | 10) * 60000UL) {
    if (emoActive() && !emoSleeping()) emoSleepNow();
    screenSetPower(false);
  }
  was = present;
}

bool blePresent() { return present; }

const char* bleMode() { return mode; }

void bleStatusCopy(char* buf, size_t n) {
  if (!strcmp(mode, "off"))         { snprintf(buf, n, "off"); return; }
  if (failed)                       { snprintf(buf, n, "%s", why); return; }
  if (!started)                     { snprintf(buf, n, "starting…"); return; }
  if (!strcmp(mode, "remote"))      { snprintf(buf, n, connected ? "connected" : "waiting to pair"); return; }
  if (powerPauseBle())              { snprintf(buf, n, "paused (battery saver)"); return; }
  snprintf(buf, n, present ? "you are here" : "not seen");
}

String bleStatus() {
  if (!strcmp(mode, "off")) return "off";
  if (failed) return why;
  if (!started) return "starting once Wi-Fi is up";
  if (!strcmp(mode, "remote")) return connected ? "remote: connected" : "remote: waiting for pairing";
  if (powerPauseBle()) return "presence: paused by battery saver";
  return present ? "presence: you are here" : "presence: not seen";
}

#else  // ENABLE_BLE == 0 — stub implementations so callers always link cleanly
void bleBegin()  {}
void bleApply()  {}
void bleLoop()   {}
void bleMedia(const char*) { toastShow("Bluetooth is off"); }
bool blePresent()          { return false; }
const char* bleMode()      { return "off"; }
void bleStatusCopy(char* buf, size_t n) { snprintf(buf, n, "not compiled"); }
String bleStatus()         { return "not compiled (ENABLE_BLE 0)"; }
#endif
