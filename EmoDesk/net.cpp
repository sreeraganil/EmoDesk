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

static int netActiveProfile = 1;

bool netHasCreds() {
  const char* s1 = cfg["wifi"]["ssid"] | "";
  const char* s2 = cfg["wifi"]["ssid2"] | "";
  return s1[0] != 0 || s2[0] != 0;
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
  if (profile == 1 || profile == 2) netActiveProfile = profile;
  String ssid, pass;
  bool txLow = true;
  LOCK();
  ssid = (netActiveProfile == 2) ? (cfg["wifi"]["ssid2"] | "") : (cfg["wifi"]["ssid"] | "");
  pass = (netActiveProfile == 2) ? (cfg["wifi"]["pass2"] | "") : (cfg["wifi"]["pass"] | "");
  if (ssid.length() == 0 && netActiveProfile == 2) {
    netActiveProfile = 1;
    ssid = cfg["wifi"]["ssid"] | "";
    pass = cfg["wifi"]["pass"] | "";
  }
  txLow = cfg["wifi"]["txLow"] | true;
  UNLOCK();
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  if (txLow) WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

static void startPortal() {
  WiFi.mode(netHasCreds() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP("EmoDesk-Setup");
  dns.start(53, "*", WiFi.softAPIP());
  portal = true;
  portalStart = millis();
  lastRetry = millis();
  if (netHasCreds()) beginSta(1);
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(host.c_str());
  WiFi.setAutoReconnect(true);
  if (!netHasCreds()) { startPortal(); return; }

  String s1 = cfg["wifi"]["ssid"] | "";
  String s2 = cfg["wifi"]["ssid2"] | "";
  netActiveProfile = 1;

  if (s1.length() > 0) {
    beginSta(1);
    uint32_t t0 = millis();
    uint32_t waitMs = (s2.length() > 0) ? 5000 : 8000;
    uint32_t lastDraw = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) {
      if (millis() - lastDraw >= 200) {
        lastDraw = millis();
        drawConnecting(s1.c_str(), t0);
      }
      delay(20);
    }
  }

  // If primary Wi-Fi is unreachable and backup hotspot is set, try backup
  if (WiFi.status() != WL_CONNECTED && s2.length() > 0) {
    logf("[NET] Primary Wi-Fi '%s' not reached, trying backup '%s'...", s1.c_str(), s2.c_str());
    WiFi.disconnect();
    delay(50);
    beginSta(2);
    uint32_t t0 = millis();
    uint32_t lastDraw = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
      if (millis() - lastDraw >= 200) {
        lastDraw = millis();
        drawConnecting(s2.c_str(), t0);
      }
      delay(20);
    }
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
    String s1, s2;
    LOCK();
    s1 = cfg["wifi"]["ssid"] | "";
    s2 = cfg["wifi"]["ssid2"] | "";
    UNLOCK();
    if (s1.length() > 0 && s2.length() > 0) {
      netActiveProfile = (netActiveProfile == 1) ? 2 : 1;
      logf("[NET] Retrying Wi-Fi with profile #%d (%s) (retry interval: %us)...",
           netActiveProfile, netActiveProfile == 2 ? s2.c_str() : s1.c_str(), (unsigned)retrySec);
      beginSta(netActiveProfile);
    } else {
      logf("[NET] Retrying Wi-Fi connection to '%s' (retry interval: %us)...", s1.c_str(), (unsigned)retrySec);
      beginSta(1);
    }
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
