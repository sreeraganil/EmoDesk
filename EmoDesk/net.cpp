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
  const char* tz = cfg["time"]["tz"] | "UTC0";
  configTzTime(tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
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
}

static void drawConnecting(const char* ssid, uint32_t t0) {
  u8g2.clearBuffer();
  emoRender(64, 24, 0.7f);
  u8g2.setFont(F_SMALL);
  char b[40];
  snprintf(b, sizeof(b), "Connecting to %s", ssid);
  textCenter(52, b);
  int dots = ((millis() - t0) / 300) % 4;
  for (int i = 0; i < dots; i++) u8g2.drawBox(56 + i * 6, 58, 3, 3);
  displaySend();
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

  netActiveProfile = 1;

  // Try each configured or predefined network (profiles 1, 2, 3)
  for (int p = 1; p <= 3; p++) {
    String s = netSSID(p);
    if (!s.length()) continue;
    if (WiFi.status() == WL_CONNECTED) break;
    beginSta(p);
    uint32_t t0 = millis();
    uint32_t waitMs = 9000;
    uint32_t lastDraw = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) {
      if (millis() - lastDraw >= 200) {
        lastDraw = millis();
        drawConnecting(s.c_str(), t0);
      }
      // If we got a definitive authentication failure, log and move to next immediately
      if (lastDisconnectReason == WIFI_REASON_AUTH_FAIL && millis() - t0 > 1500) {
        logf("[NET] '%s' authentication failed (wrong password)", s.c_str());
        break;
      }
      delay(20);
    }
    if (WiFi.status() == WL_CONNECTED) break;
    WiFi.disconnect(false, false);
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
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
    int nextP = (netActiveProfile % 3) + 1;
    for (int i = 0; i < 3; i++) {
      if (netSSID(nextP).length() > 0) break;
      nextP = (nextP % 3) + 1;
    }
    String s = netSSID(nextP);
    logf("[NET] Retrying Wi-Fi with profile #%d (%s) (retry interval: %us)...",
         nextP, s.c_str(), (unsigned)retrySec);
    beginSta(nextP);
  }
}

void netDrawPortal() {
  drawHeader("Wi-Fi Setup", "AP ready");

  u8g2.setFont(F_TINY);
  u8g2.drawStr(4, 21, "1. Connect Wi-Fi:");
  int w1 = u8g2.getUTF8Width("EmoDesk-Setup") + 10;
  int x1 = (128 - w1) / 2;
  u8g2.drawRFrame(x1, 23, w1, 12, 2);
  u8g2.setFont(F_SMALL);
  textCenter(32, "EmoDesk-Setup");

  u8g2.setFont(F_TINY);
  u8g2.drawStr(4, 45, "2. Open in browser:");
  int w2 = u8g2.getUTF8Width("192.168.4.1") + 14;
  int x2 = (128 - w2) / 2;
  u8g2.drawRBox(x2, 48, w2, 14, 2);
  u8g2.setDrawColor(0);
  u8g2.setFont(F_MED);
  textCenter(59, "192.168.4.1");
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
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent(browserUA ? "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"
                                : "EmoDesk/1.0 (ESP32)");
    if (hName && hVal) http.addHeader(hName, hVal);
    code = http.GET();
    if (code == 200 && cb) {
      http.getStream().setTimeout(4000);
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
  httpGet(url, [&](Stream& s) {
    DeserializationError e = filter ? deserializeJson(doc, s, DeserializationOption::Filter(*filter))
                                    : deserializeJson(doc, s);
    if (e) logf("JSON error: %s", e.c_str());
    ok = !e;
    return ok;
  }, hName, hVal, browserUA);
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
