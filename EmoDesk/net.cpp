#include "net.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "display.h"
#include "emo.h"
#include "feeds.h"
#include "cards.h"
#include "services.h"

static DNSServer dns;
static volatile bool portal = false, online = false;
static bool mdnsOk = false;
static uint32_t portalStart = 0, lastRetry = 0;

#include <esp_wifi.h>

static int netActiveProfile = 1;
static volatile uint8_t lastDisconnectReason = 0;
static const wifi_power_t TXP[] = {WIFI_POWER_8_5dBm, WIFI_POWER_11dBm, WIFI_POWER_15dBm, WIFI_POWER_19_5dBm};

static const char* wifiReasonStr(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return "Network not found (out of range or 5GHz only)";
    case WIFI_REASON_AUTH_FAIL: return "Wrong password / Authentication failed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Handshake timeout (Wrong password or weak signal)";
    case WIFI_REASON_BEACON_TIMEOUT: return "Signal lost (Beacon timeout)";
    case WIFI_REASON_ASSOC_EXPIRE: return "Association expired";
    case WIFI_REASON_AUTH_EXPIRE: return "Authentication expired";
    case WIFI_REASON_ASSOC_FAIL: return "Association failed";
    case WIFI_REASON_CONNECTION_FAIL: return "Connection failed";
    case WIFI_REASON_ASSOC_LEAVE: return "Disassociated";
    default: return "Disconnected";
  }
}

static void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      lastDisconnectReason = 0;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      lastDisconnectReason = info.wifi_sta_disconnected.reason;
      if (lastDisconnectReason != WIFI_REASON_ASSOC_LEAVE && lastDisconnectReason != 0) {
        logf("[NET] Disconnected: %s (code %u)", wifiReasonStr(lastDisconnectReason), lastDisconnectReason);
      }
      break;
    default:
      break;
  }
}

void netApplyTxPower() {
  int p = DEFAULT_WIFI_TX_POWER;
  if (!cfg["wifi"]["txPower"].isNull()) {
    p = cfg["wifi"]["txPower"].as<int>();
  } else if (!cfg["wifi"]["txDb"].isNull()) {
    float db = cfg["wifi"]["txDb"].as<float>();
    if (db >= 19.0f) p = 3;
    else if (db >= 14.5f) p = 2;
    else if (db >= 10.5f) p = 1;
    else p = 0;
  } else if (!cfg["wifi"]["txLow"].isNull()) {
    p = (cfg["wifi"]["txLow"] | true) ? 0 : 3;
  }
  p = constrain(p, 0, 3);
  WiFi.setTxPower(TXP[p]);
}

String netSSID(int p) {
  if (cfg["wifi"]["nets"].is<JsonArray>()) {
    JsonArray arr = cfg["wifi"]["nets"].as<JsonArray>();
    if (p >= 1 && p <= (int)arr.size()) {
      const char* s = arr[p - 1]["ssid"] | "";
      if (s && *s) return String(s);
    }
  }
  const char* defSsid = (p == 1) ? DEFAULT_WIFI_SSID : (p == 2 ? DEFAULT_WIFI_SSID2 : DEFAULT_WIFI_SSID3);
  const char* key = (p == 1) ? "ssid" : (p == 2 ? "ssid2" : "ssid3");
  String s = cfg["wifi"][key] | "";
  if (!s.length() && defSsid && *defSsid) s = defSsid;
  return s;
}

String netPass(int p) {
  String s = netSSID(p);
  if (!s.length()) return "";

  // 1. Try from cfg["wifi"]["nets"] array matching profile index
  if (cfg["wifi"]["nets"].is<JsonArray>()) {
    JsonArray arr = cfg["wifi"]["nets"].as<JsonArray>();
    if (p >= 1 && p <= (int)arr.size()) {
      const char* pass = arr[p - 1]["pass"] | "";
      if (pass && *pass && strcmp(pass, MASK) != 0) return String(pass);
    }
    // Also try finding by SSID in nets array
    for (JsonObject obj : arr) {
      if (s.equalsIgnoreCase(obj["ssid"] | "")) {
        const char* pass = obj["pass"] | "";
        if (pass && *pass && strcmp(pass, MASK) != 0) return String(pass);
      }
    }
  }

  // 2. Try from flat cfg["wifi"] keys by SSID
  for (int i = 1; i <= 3; i++) {
    const char* sk = (i == 1) ? "ssid" : (i == 2 ? "ssid2" : "ssid3");
    const char* pk = (i == 1) ? "pass" : (i == 2 ? "pass2" : "pass3");
    if (s.equalsIgnoreCase(cfg["wifi"][sk] | "")) {
      const char* pass = cfg["wifi"][pk] | "";
      if (pass && *pass && strcmp(pass, MASK) != 0) return String(pass);
    }
  }

  // 3. Try from flat cfg["wifi"] keys by index
  const char* key = (p == 1) ? "pass" : (p == 2 ? "pass2" : "pass3");
  const char* pass = cfg["wifi"][key] | "";
  if (pass && *pass && strcmp(pass, MASK) != 0) return String(pass);

  // 4. Try from compile-time defaults by SSID
  if (s == DEFAULT_WIFI_SSID && strlen(DEFAULT_WIFI_PASS) > 0) return DEFAULT_WIFI_PASS;
  if (s == DEFAULT_WIFI_SSID2 && strlen(DEFAULT_WIFI_PASS2) > 0) return DEFAULT_WIFI_PASS2;
  if (s == DEFAULT_WIFI_SSID3 && strlen(DEFAULT_WIFI_PASS3) > 0) return DEFAULT_WIFI_PASS3;

  // 5. Try from compile-time defaults by index
  const char* defPass = (p == 1) ? DEFAULT_WIFI_PASS : (p == 2 ? DEFAULT_WIFI_PASS2 : DEFAULT_WIFI_PASS3);
  if (defPass && *defPass) return defPass;

  return "";
}

bool netHasCreds() {
  return netSSID(1).length() > 0 || netSSID(2).length() > 0 || netSSID(3).length() > 0;
}
bool netOnline() { return online; }
bool netPortal() { return portal; }
String netIP() { return online ? WiFi.localIP().toString() : (portal ? WiFi.softAPIP().toString() : String("-")); }
int netRSSI() { return online ? WiFi.RSSI() : 0; }

void timeApply() {
  LOCK();
  String tz = cfg["time"]["tz"] | "";
  UNLOCK();
  tz.trim();
  if (!tz.length()) tz = "UTC0";
  configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  logf("[TIME] TZ applied: %s", tz.c_str());
}

static void beginSta(int profile = 0) {
  if (profile >= 1 && profile <= 3) netActiveProfile = profile;
  String ssid = netSSID(netActiveProfile);
  String pass = netPass(netActiveProfile);
  if (!ssid.length()) {
    for (int p = 1; p <= 3; p++) {
      ssid = netSSID(p);
      if (ssid.length()) { netActiveProfile = p; pass = netPass(p); break; }
    }
  }
  if (!ssid.length()) return;

  WiFi.disconnect(false, false);
  delay(100);

  if (portal) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.setSleep(false);
  netApplyTxPower();
  lastDisconnectReason = 0;
  logf("[NET] Connecting to '%s' (Profile #%d, pass length: %d)...",
       ssid.c_str(), netActiveProfile, (int)pass.length());
  if (pass.length() == 0) {
    logf("[NET] Notice: Profile #%d has no password set (open network or missing credential)", netActiveProfile);
  }
  WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : nullptr);
  netApplyTxPower();
}

static void startPortal() {
  WiFi.mode(netHasCreds() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP("EmoDesk-Setup");
  dns.start(53, "*", WiFi.softAPIP());
  portal = true;
  portalStart = millis();
  lastRetry = millis();
  logf("[NET] Setup AP 'EmoDesk-Setup' at %s", WiFi.softAPIP().toString().c_str());
}

static void onConnect() {
  online = true;
  String curSSID = WiFi.SSID();
  logf("[NET] Wi-Fi connected to '%s' (Profile #%d), IP %s (RSSI: %d dBm)",
       curSSID.c_str(), netActiveProfile, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  // Always refresh mDNS on every new connection/IP so emo.local stays reachable
  MDNS.end();
  String host = cfg["host"] | DEFAULT_HOST;
  mdnsOk = MDNS.begin(host.c_str());
  if (mdnsOk) MDNS.addService("http", "tcp", 80);
  timeApply();
  feedsRefresh();
  toastShow("Wi-Fi connected");
}

static void drawWifiWaves(uint32_t t0) {
  uint32_t waveStep = ((millis() - t0) / 180) % 4;

  // Wave 1 (inner, radius ~5)
  if (waveStep == 0 || waveStep == 1 || waveStep == 2) {
    // Left
    u8g2.drawPixel(33, 23);
    u8g2.drawPixel(34, 21); u8g2.drawPixel(34, 25);
    u8g2.drawPixel(35, 19); u8g2.drawPixel(35, 27);
    // Right
    u8g2.drawPixel(95, 23);
    u8g2.drawPixel(94, 21); u8g2.drawPixel(94, 25);
    u8g2.drawPixel(93, 19); u8g2.drawPixel(93, 27);
  }

  // Wave 2 (middle, radius ~10)
  if (waveStep == 1 || waveStep == 2 || waveStep == 3) {
    // Left
    u8g2.drawPixel(27, 23);
    u8g2.drawPixel(28, 20); u8g2.drawPixel(28, 26);
    u8g2.drawPixel(29, 17); u8g2.drawPixel(29, 29);
    u8g2.drawPixel(30, 15); u8g2.drawPixel(30, 31);
    // Right
    u8g2.drawPixel(101, 23);
    u8g2.drawPixel(100, 20); u8g2.drawPixel(100, 26);
    u8g2.drawPixel(99, 17);  u8g2.drawPixel(99, 29);
    u8g2.drawPixel(98, 15);  u8g2.drawPixel(98, 31);
  }

  // Wave 3 (outer, radius ~15)
  if (waveStep == 2 || waveStep == 3) {
    // Left
    u8g2.drawPixel(21, 23);
    u8g2.drawPixel(22, 19); u8g2.drawPixel(22, 27);
    u8g2.drawPixel(23, 16); u8g2.drawPixel(23, 30);
    u8g2.drawPixel(24, 13); u8g2.drawPixel(24, 33);
    // Right
    u8g2.drawPixel(107, 23);
    u8g2.drawPixel(106, 19); u8g2.drawPixel(106, 27);
    u8g2.drawPixel(105, 16); u8g2.drawPixel(105, 30);
    u8g2.drawPixel(104, 13); u8g2.drawPixel(104, 33);
  }
}

static void drawConnecting(const char* ssid, uint32_t t0) {
  u8g2.clearBuffer();
  drawHeader("EmoDesk", "WI-FI");
  emoRender(64, 23, 0.52f);
  drawWifiWaves(t0);

  char b[48];
  uint32_t dotCycle = ((millis() - t0) / 280) % 4;
  const char* dots = (dotCycle == 0 ? "" : (dotCycle == 1 ? "." : (dotCycle == 2 ? ".." : "...")));
  snprintf(b, sizeof(b), "Joining %s%s", ssid ? ssid : "", dots);
  u8g2.setFont(F_SMALL);
  if (u8g2.getUTF8Width(b) <= 122) textCenter(47, b);
  else drawTextFit(3, 47, 122, b);

  int bx = 20, by = 54, bw = 88, bh = 6;
  u8g2.drawRFrame(bx, by, bw, bh, 2);
  int pW = 20;
  int span = bw - 4 - pW;
  int phase = ((millis() - t0) / 22) % (span * 2);
  int pos = phase <= span ? phase : (span * 2 - phase);
  u8g2.drawRBox(bx + 2 + pos, by + 1, pW, bh - 2, 1);
  u8g2.drawPixel(bx - 4, by + 2);
  u8g2.drawPixel(bx + bw + 3, by + 2);

  displaySend();
}

static void drawScanning(uint32_t t0) {
  u8g2.clearBuffer();
  drawHeader("EmoDesk", "WI-FI");
  emoRender(64, 23, 0.52f);
  drawWifiWaves(t0);

  uint32_t dotCycle = ((millis() - t0) / 280) % 4;
  const char* dots = (dotCycle == 0 ? "" : (dotCycle == 1 ? "." : (dotCycle == 2 ? ".." : "...")));
  char b[32];
  snprintf(b, sizeof(b), "Scanning Wi-Fi%s", dots);
  u8g2.setFont(F_SMALL);
  textCenter(47, b);

  int bx = 20, by = 54, bw = 88, bh = 6;
  u8g2.drawRFrame(bx, by, bw, bh, 2);
  int pW = 20;
  int span = bw - 4 - pW;
  int phase = ((millis() - t0) / 22) % (span * 2);
  int pos = phase <= span ? phase : (span * 2 - phase);
  u8g2.drawRBox(bx + 2 + pos, by + 1, pW, bh - 2, 1);
  u8g2.drawPixel(bx - 4, by + 2);
  u8g2.drawPixel(bx + bw + 3, by + 2);

  displaySend();
}

static bool tryConnectProfile(int p, uint32_t waitMs = 9000) {
  String s = netSSID(p);
  if (!s.length()) return false;
  if (WiFi.status() == WL_CONNECTED) return true;

  beginSta(p);
  uint32_t t0 = millis();
  uint32_t lastDraw = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) {
    wdtFeed();
    uint32_t now = millis();
    if (now - lastDraw >= 30) {
      lastDraw = now;
      drawConnecting(s.c_str(), t0);
    }
    // If we got a definitive authentication failure, log and move to next immediately
    if (lastDisconnectReason == WIFI_REASON_AUTH_FAIL && now - t0 > 1500) {
      logf("[NET] '%s' authentication failed (wrong password)", s.c_str());
      break;
    }
    delay(10);
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(false, false);
  delay(100);
  return false;
}

struct ScannedSavedNet {
  int profile;
  int rssi;
};

static int findAvailableSavedProfiles(ScannedSavedNet* matches, int maxMatches, bool animated = false) {
  wdtFeed();
  int n = 0;

  if (animated) {
    uint32_t t0 = millis();
    uint32_t lastDraw = 0;
    if (!lastBootCrashed()) emoSet(E_SUSPICIOUS, 7000);

    int16_t startRes = WiFi.scanNetworks(true, false);
    if (startRes == WIFI_SCAN_FAILED) {
      logf("[NET] Async Wi-Fi scan failed to start, falling back to sync scan");
      drawScanning(t0);
      n = WiFi.scanNetworks(false, false);
    } else {
      int16_t res = WIFI_SCAN_RUNNING;
      while (true) {
        wdtFeed();
        uint32_t now = millis();
        if (now - lastDraw >= 30) {
          lastDraw = now;
          drawScanning(t0);
        }

        if (res == WIFI_SCAN_RUNNING) {
          res = WiFi.scanComplete();
        }

        if (res >= 0) {
          n = res;
          // Ensure the boot animation is shown for at least 1800ms
          if (now - t0 >= 1800) break;
        } else if (res == WIFI_SCAN_FAILED) {
          logf("[NET] Async Wi-Fi scan returned failed");
          n = 0;
          break;
        }

        // Safety timeout (8 seconds)
        if (now - t0 >= 8000) {
          logf("[NET] Wi-Fi scan timed out");
          n = (res >= 0) ? res : 0;
          break;
        }
        delay(10);
      }
    }
  } else {
    n = WiFi.scanNetworks(false, false);
  }
  wdtFeed();

  int count = 0;
  if (n <= 0) {
    WiFi.scanDelete();
    return 0;
  }
  logf("[NET] Wi-Fi scan found %d network(s) in range", n);

  int maxProfiles = 3;
  if (cfg["wifi"]["nets"].is<JsonArray>()) {
    int arrSize = cfg["wifi"]["nets"].as<JsonArray>().size();
    if (arrSize > maxProfiles) maxProfiles = arrSize;
  }

  for (int p = 1; p <= maxProfiles; p++) {
    String saved = netSSID(p);
    if (!saved.length()) continue;

    int bestRssi = -1000;
    bool found = false;
    for (int i = 0; i < n; i++) {
      if (saved.equalsIgnoreCase(WiFi.SSID(i))) {
        found = true;
        if (WiFi.RSSI(i) > bestRssi) bestRssi = WiFi.RSSI(i);
      }
    }

    if (found && count < maxMatches) {
      matches[count].profile = p;
      matches[count].rssi = bestRssi;
      logf("[NET] Saved network '%s' (Profile #%d) available in scan (RSSI: %d dBm)",
           saved.c_str(), p, bestRssi);
      count++;
    }
  }

  WiFi.scanDelete();

  // Sort available saved networks by signal strength (strongest RSSI first)
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (matches[j].rssi > matches[i].rssi) {
        ScannedSavedNet tmp = matches[i];
        matches[i] = matches[j];
        matches[j] = tmp;
      }
    }
  }
  return count;
}

void netBegin() {
  String host = cfg["host"] | DEFAULT_HOST;
  WiFi.persistent(false);
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(host.c_str());
  WiFi.setAutoReconnect(false);
  netApplyTxPower();
  if (!netHasCreds()) { startPortal(); return; }

  ScannedSavedNet matches[6];
  int matchCount = findAvailableSavedProfiles(matches, 6, true);

  if (matchCount > 0) {
    logf("[NET] Found %d saved network(s) in scan. Connecting to available network...", matchCount);
    for (int i = 0; i < matchCount; i++) {
      int p = matches[i].profile;
      String s = netSSID(p);
      logf("[NET] Attempting available network '%s' (Profile #%d, RSSI: %d dBm)...",
           s.c_str(), p, matches[i].rssi);
      if (tryConnectProfile(p, 9000)) {
        emoSet(E_HAPPY, 3000);
        break;
      }
    }
  } else {
    logf("[NET] No saved network visible in scan (or hidden SSID). Trying saved networks in order...");
    int maxProfiles = 3;
    if (cfg["wifi"]["nets"].is<JsonArray>()) {
      int arrSize = cfg["wifi"]["nets"].as<JsonArray>().size();
      if (arrSize > maxProfiles) maxProfiles = arrSize;
    }
    for (int p = 1; p <= maxProfiles; p++) {
      if (tryConnectProfile(p, 9000)) {
        emoSet(E_HAPPY, 3000);
        break;
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    emoSet(E_HAPPY, 3000);
    onConnect();
  } else {
    logf("WiFi connection failed - setup AP started, will retry in background");
    startPortal();
    lastRetry = millis();
  }
}

void netLoop() {
  if (portal) dns.processNextRequest();
  bool c = WiFi.status() == WL_CONNECTED;
  if (c && !online) onConnect();

  uint32_t retrySec = 60;
  LOCK();
  if (!cfg["wifi"]["retrySec"].isNull()) retrySec = cfg["wifi"]["retrySec"].as<uint32_t>();
  else if (!cfg["wifi"]["retryMin"].isNull()) retrySec = cfg["wifi"]["retryMin"].as<uint32_t>() * 60;
  UNLOCK();
  if (retrySec < 5) retrySec = 5;
  if (retrySec > 3600) retrySec = 3600;
  uint32_t retryMs = retrySec * 1000UL;

  uint32_t now = millis();

  if (!c && online) {
    online = false;
    logf("[NET] Wi-Fi lost, will retry reconnect every %us", (unsigned)retrySec);
    lastRetry = now;
  }

  // If a phone is actively connected to the setup AP, pause retries so user can configure
  if (portal && WiFi.softAPgetStationNum() > 0) {
    lastRetry = now;
  }

  if (portal && c && netHasCreds() && now - portalStart > 90000) {
    if (WiFi.softAPgetStationNum() == 0) {
      dns.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      portal = false;
      logf("[NET] Setup AP closed");
    }
  }

  if (!c && netHasCreds() && now - lastRetry > retryMs) {
    lastRetry = now;
    ScannedSavedNet matches[6];
    int matchCount = findAvailableSavedProfiles(matches, 6);
    bool connected = false;

    if (matchCount > 0) {
      logf("[NET] Reconnect: %d saved network(s) available in scan. Attempting...", matchCount);
      for (int i = 0; i < matchCount; i++) {
        int p = matches[i].profile;
        if (tryConnectProfile(p, 7000)) {
          connected = true;
          break;
        }
      }
    }

    if (!connected && WiFi.status() != WL_CONNECTED) {
      // Fallback: rotate profile
      int maxProfiles = 3;
      if (cfg["wifi"]["nets"].is<JsonArray>()) {
        int arrSize = cfg["wifi"]["nets"].as<JsonArray>().size();
        if (arrSize > maxProfiles) maxProfiles = arrSize;
      }
      int nextP = (netActiveProfile % maxProfiles) + 1;
      for (int i = 0; i < maxProfiles; i++) {
        if (netSSID(nextP).length() > 0) break;
        nextP = (nextP % maxProfiles) + 1;
      }
      String s = netSSID(nextP);
      logf("[NET] Scan found no saved networks. Retrying profile #%d (%s)...",
           nextP, s.c_str());
      beginSta(nextP);
    }
  }
}

void netDrawPortal() {
  drawHeader("Wi-Fi Setup", "AP");

  u8g2.setFont(F_TINY);
  textCenter(19, "1. JOIN WI-FI NETWORK");
  u8g2.drawRFrame(12, 22, 104, 13, 3);
  u8g2.setFont(F_SMALL);
  textCenter(31, "EmoDesk-Setup");

  u8g2.setFont(F_TINY);
  textCenter(44, "2. OPEN IN BROWSER");
  u8g2.drawRBox(16, 47, 96, 15, 3);
  u8g2.setDrawColor(0);
  u8g2.setFont(F_MED);
  textCenter(58, "192.168.4.1");
  u8g2.setDrawColor(1);
}

// ------------------------------------------------------------------ HTTP
int httpGet(const String& url, std::function<bool(Stream&)> cb, const char* hName, const char* hVal, bool browserUA) {
  bool tls = url.startsWith("https");
  // A TLS session needs mbedtls buffers plus the handshake.
  if (!heapOk(tls ? 32000 : 10000, tls ? 12000 : 3500)) {
    static uint32_t lastWarn = 0;
    if (!lastWarn || millis() - lastWarn > 60000) {
      lastWarn = millis();
      logf("Low memory (%u KB, block %u KB): fetch postponed", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(heapLargest() / 1024));
    }
    return -3;
  }
  gNetBusy = true;
  HTTPClient http;
  WiFiClientSecure* sc = nullptr;
  WiFiClient* pc = nullptr;
  bool ok;
  if (tls) {
    sc = new WiFiClientSecure();
    if (!sc) { gNetBusy = false; return -4; }
    sc->setInsecure();
    ok = http.begin(*sc, url);
  } else {
    pc = new WiFiClient();
    if (!pc) { gNetBusy = false; return -4; }
    ok = http.begin(*pc, url);
  }
  int code = -1;
  if (ok) {
    http.useHTTP10(true);
    http.setTimeout(5000);
    http.setConnectTimeout(4000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent(browserUA ? "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"
                                : "EmoDesk/1.0 (ESP32)");
    if (hName && hVal) http.addHeader(hName, hVal);
    code = http.GET();
    if (code == 200 && cb) {
      http.getStream().setTimeout(8000);
      if (!cb(http.getStream())) code = -2;
    }
    http.end();
  }
  delete sc;
  delete pc;
  gNetBusy = false;
  if (code != 200) {
    int q = url.indexOf('?');
    logf("HTTP %d %s", code, url.substring(0, q > 0 ? min(q, 70) : 70).c_str());
  }
  return code;
}

bool httpJson(const String& url, JsonDocument& doc, JsonDocument* filter, const char* hName, const char* hVal, bool browserUA) {
  bool ok = false;
  DeserializationError lastErr = DeserializationError::Ok;
  int attempts = 0;
  do {
    if (attempts > 0) {
      // Transient truncated read (e.g. Render cold-start): wait briefly then retry once
      logf("JSON IncompleteInput from %s — retrying in 2s", url.substring(0, min((int)url.length(), 60)).c_str());
      delay(2000);
      doc.clear();
    }
    ok = false;
    lastErr = DeserializationError::Ok;
    httpGet(url, [&](Stream& s) {
      s.setTimeout(attempts > 0 ? 12000 : 8000);  // extra time on retry
      lastErr = filter ? deserializeJson(doc, s, DeserializationOption::Filter(*filter))
                       : deserializeJson(doc, s);
      if (lastErr) logf("JSON error: %s", lastErr.c_str());
      ok = !lastErr;
      return ok;
    }, hName, hVal, browserUA);
    attempts++;
  } while (!ok && lastErr == DeserializationError::IncompleteInput && attempts < 2);
  return ok;
}

String urlEncode(const char* s) {
  String o;
  const char* hex = "0123456789ABCDEF";
  for (; *s; s++) {
    uint8_t c = *s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// ------------------------------------------------------------------ task
static void netTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(150));
  for (;;) {
    if (!otaBusy && online) {
      // Prioritize active visible card first
      cardsFetchLoop();
      // Background feeds run after visible card is serviced
      feedsLoop();
    }
    gNetStackFree = uxTaskGetStackHighWaterMark(NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void netStartTask() { 
  xTaskCreate(netTask, "net", NET_STACK, nullptr, 1, nullptr); 
}
