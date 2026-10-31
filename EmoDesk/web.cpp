#include "web.h"
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>
#include "cards.h"
#include "services.h"
#include "power.h"
#include "feeds.h"
#include "web_ui.h"
#include "terminal.h"

static WebServer server(80);
// MASK defined in config.h
static uint32_t restartAt = 0;
static File upFile;
static String upErr, upPath;

// ------------------------------------------------------------------ helpers
// Streams JSON in 512-byte pieces instead of building one big String first.
// The cards and config documents can be 10+ KB; a String copy of that used to
// compete with TLS for RAM at the worst moment.
class ChunkOut : public Print {
  char buf[512];
  size_t n = 0;
 public:
  size_t write(uint8_t c) override {
    buf[n++] = (char)c;
    if (n == sizeof(buf)) done();
    return 1;
  }
  size_t write(const uint8_t* p, size_t len) override {
    for (size_t i = 0; i < len; i++) write(p[i]);
    return len;
  }
  void done() {
    if (n) server.sendContent(buf, n);
    n = 0;
  }
};

static void sendJson(const JsonDocument& d, int code = 200) {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(measureJson(d));
  server.send(code, "application/json", "");
  ChunkOut o;
  serializeJson(d, o);
  o.done();
}
static void ok() { server.send(200, "application/json", "{\"ok\":true}"); }
static void fail(int code, const char* m) { server.send(code, "application/json", String("{\"ok\":false,\"error\":\"") + m + "\"}"); }
static bool body(JsonDocument& d) {
  if (!server.hasArg("plain")) return false;
  return !deserializeJson(d, server.arg("plain"));
}
static bool safePath(const String& p) {
  return (p.startsWith("/img/") || p.startsWith("/anim/") || p.startsWith("/snd/")) && p.indexOf("..") < 0;
}
static String cleanName(const String& in) {
  String o;
  for (unsigned i = 0; i < in.length() && o.length() < 28; i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') o += c;
    else if (c == ' ') o += '_';
  }
  return o.length() ? o : String("file");
}

// ------------------------------------------------------------------ setup page (captive portal)
static const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EmoDesk setup</title><style>body{font-family:ui-rounded,system-ui,sans-serif;background:#e6e3ef;color:#1e2240;margin:0;padding:24px;max-width:420px}
h1{font-size:26px;margin:0 0 4px}p{color:#4b4f6e}button,input,select{font:inherit;width:100%;box-sizing:border-box;padding:12px;border-radius:14px;border:2px solid #1e2240;margin:6px 0;background:#fff}
button{background:#1e2240;color:#fff;font-weight:700}a{color:#1e2240}</style></head><body>
<h1>Hi, I'm Emo</h1><p>Pick your Wi-Fi so I can fetch the time, weather and scores.</p>
<button onclick="scan()" id="sb">Find networks</button><select id="ss" onchange="ssid.value=this.value"><option value="">Networks appear here</option></select>
<input id="ssid" placeholder="Network name"><input id="pass" type="password" placeholder="Password">
<button onclick="save()">Connect and restart</button><p id="msg"></p><p><a href="/?full=1">Open the full dashboard instead</a></p>
<script>async function scan(){sb.textContent='Searching...';let r=await(await fetch('/api/scan')).json();ss.innerHTML='<option value="">Choose a network</option>'+r.map(n=>`<option>${n.s}</option>`).join('');sb.textContent='Search again'}
async function save(){if(!ssid.value){msg.textContent='Enter the network name first.';return}await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid.value,pass:pass.value})});
msg.textContent='Saved. Emo is restarting - reconnect your phone to your normal Wi-Fi and open http://emo.local';}</script></body></html>)HTML";

static void redirectSetup() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/setup");
  server.send(302, "text/plain", "");
}

// ------------------------------------------------------------------ status
static void hStatus() {
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["name"] = cfg["name"] | "Emo";
  d["host"] = cfg["host"] | DEFAULT_HOST;
  d["ip"] = netIP();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = netRSSI();
  d["online"] = netOnline();
  d["portal"] = netPortal();
  d["heap"] = ESP.getFreeHeap();
  d["minHeap"] = ESP.getMinFreeHeap();
  d["block"] = heapLargest();
  d["cpu"] = ESP.getCpuFreqMHz();
  d["reset"] = resetReasonStr();
  d["crashes"] = bootCrashCount();
  d["safe"] = gSafeMode;
  d["stackNet"] = (unsigned)gNetStackFree;
  d["stackAudio"] = (unsigned)gAudioStackFree;
  d["stackLoop"] = (unsigned)uxTaskGetStackHighWaterMark(NULL);
  JsonObject pw = d["power"].to<JsonObject>();
  pw["bat"] = batteryPercent();
  pw["mv"] = batteryMv();
  pw["saver"] = powerSaver();
  pw["mode"] = powerModeName();
  d["up"] = millis() / 1000;
  d["time"] = (long)time(nullptr);
  d["timeOk"] = timeOK();
  d["fsTotal"] = LittleFS.totalBytes();
  d["fsUsed"] = LittleFS.usedBytes();
  d["chipTemp"] = temperatureRead();
  JsonObject e = d["emo"].to<JsonObject>();
  e["active"] = emoActive();
  e["mood"] = emoActive() ? EMO_NAMES[emoCurrent()] : "disabled";
  e["happy"] = (int)pet.happy;
  e["energy"] = (int)pet.energy;
  e["hunger"] = (int)pet.hunger;
  e["pets"] = pet.interactions;
  e["sleeping"] = emoSleeping();
  d["screenOn"] = screenIsOn();
  LOCK();
  d["card"] = cardsCurrent();
  d["cardId"] = cardsCurrentId();
  d["cardType"] = cardsCurrentType();
  d["profile"] = cfg["profile"] | "Default";
  UNLOCK();
  JsonObject a = d["audio"].to<JsonObject>();
  a["installed"] = audioInstalled();
  a["muted"] = audioMuted();
  a["vol"] = audioVolume();
  a["busy"] = audioBusy();
  a["tts"] = ENABLE_TTS;
  JsonObject c = d["cric"].to<JsonObject>();
  c["hits"] = cric.hits;
  c["limit"] = cric.limit;
  c["err"] = cric.err;
  c["n"] = cric.n;
  d["mqtt"] = mqttConnected();
  d["ble"] = bleStatus();
  d["pc"] = pcStats.ts && millis() - pcStats.ts < 15000;
  if (homeW.ok) { d["w"]["t"] = homeW.temp; d["w"]["c"] = wmoText(homeW.code); }
  d["msg"]["from"] = lastMsg.from;
  d["msg"]["text"] = lastMsg.text;
  sendJson(d);
}

// ------------------------------------------------------------------ config
static void maskInto(JsonDocument& d) {
  d.set(cfg);
  if (!d["wifi"]["pass"].isNull() && strlen(d["wifi"]["pass"] | "")) d["wifi"]["pass"] = MASK;
  if (!d["wifi"]["pass2"].isNull() && strlen(d["wifi"]["pass2"] | "")) d["wifi"]["pass2"] = MASK;
  if (!d["wifi"]["pass3"].isNull() && strlen(d["wifi"]["pass3"] | "")) d["wifi"]["pass3"] = MASK;
  if (d["wifi"]["nets"].is<JsonArray>()) {
    for (JsonObject n : d["wifi"]["nets"].as<JsonArray>()) {
      if (!n["pass"].isNull() && strlen(n["pass"] | "")) n["pass"] = MASK;
    }
  }
  if (!d["mqtt"]["pass"].isNull() && strlen(d["mqtt"]["pass"] | "")) d["mqtt"]["pass"] = MASK;
}

static void hConfigGet() {
  powerActivity();
  JsonDocument d;
  LOCK();
  maskInto(d);
  UNLOCK();
  sendJson(d);
}

static void hConfigPost() {
  JsonDocument d;
  if (!body(d) || !d.is<JsonObject>()) { fail(400, "bad json"); return; }
  LOCK();
  String oldHost = cfg["host"] | DEFAULT_HOST, oldTz = cfg["time"]["tz"] | "UTC0";
  if (!strcmp(d["wifi"]["pass"] | "", MASK)) d["wifi"]["pass"] = cfg["wifi"]["pass"] | "";
  if (!strcmp(d["wifi"]["pass2"] | "", MASK)) d["wifi"]["pass2"] = cfg["wifi"]["pass2"] | "";
  if (!strcmp(d["wifi"]["pass3"] | "", MASK)) d["wifi"]["pass3"] = cfg["wifi"]["pass3"] | "";
  if (d["wifi"]["nets"].is<JsonArray>()) {
    for (JsonObject n : d["wifi"]["nets"].as<JsonArray>()) {
      const char* p = n["pass"] | "";
      if (!strcmp(p, MASK)) {
        const char* s = n["ssid"] | "";
        String orig = "";
        if (cfg["wifi"]["nets"].is<JsonArray>()) {
          for (JsonObject oldN : cfg["wifi"]["nets"].as<JsonArray>()) {
            if (String(s).equalsIgnoreCase(oldN["ssid"] | "")) {
              const char* op = oldN["pass"] | "";
              if (op && strcmp(op, MASK) != 0) { orig = op; break; }
            }
          }
        }
        if (!orig.length()) {
          for (int k = 1; k <= 3; k++) {
            const char* sk = (k == 1) ? "ssid" : (k == 2 ? "ssid2" : "ssid3");
            const char* pk = (k == 1) ? "pass" : (k == 2 ? "pass2" : "pass3");
            if (String(s).equalsIgnoreCase(cfg["wifi"][sk] | "")) {
              const char* op = cfg["wifi"][pk] | "";
              if (op && strcmp(op, MASK) != 0) { orig = op; break; }
            }
          }
        }
        if (!orig.length()) {
          if (String(s) == DEFAULT_WIFI_SSID) orig = DEFAULT_WIFI_PASS;
          else if (String(s) == DEFAULT_WIFI_SSID2) orig = DEFAULT_WIFI_PASS2;
          else if (String(s) == DEFAULT_WIFI_SSID3) orig = DEFAULT_WIFI_PASS3;
        }
        n["pass"] = orig;
      }
    }
  }
  if (!strcmp(d["mqtt"]["pass"] | "", MASK)) d["mqtt"]["pass"] = cfg["mqtt"]["pass"] | "";
  if (d["wifi"].isNull() && !cfg["wifi"].isNull()) d["wifi"] = cfg["wifi"];
  cfg = d;
  String newHost = cfg["host"] | DEFAULT_HOST, newTz = cfg["time"]["tz"] | "UTC0";
  UNLOCK();
  markDirty(DIRTY_CFG);
  displayApply();
  audioApply();
  inputApply();
  emoApply();
  bleApply();
  if (oldTz != newTz) timeApply();
  feedsRefresh();
  mqttCardsChanged();
  server.send(200, "application/json", oldHost != newHost ? "{\"ok\":true,\"reboot\":true}" : "{\"ok\":true}");
}

static void hCardsGet() {
  LOCK();
  sendJson(cardsDoc);
  UNLOCK();
}

static void hCardsPost() {
  JsonDocument d;
  if (!body(d) || !d["cards"].is<JsonArray>()) { fail(400, "expected {cards:[...]}"); return; }
  LOCK();
  cardsDoc = d;
  UNLOCK();
  cardsRebuild();
  markDirty(DIRTY_CARDS);
  ok();
}

// ------------------------------------------------------------------ live control
static void hScreen() {
  powerActivity();
  server.sendHeader("Cache-Control", "no-store");
  LOCK();
  server.send_P(200, "application/octet-stream", (const char*)u8g2.getBufferPtr(), 1024);
  UNLOCK();
}

static void hInput() {
  String g = server.arg("g");
  uint8_t gv = G_NONE;
  for (uint8_t i = G_PRESS; i <= G_VLONG; i++) if (g == gestureName(i)) gv = i;
  if (gv == G_NONE) { fail(400, "unknown gesture"); return; }
  logf("[WEB] Input gesture: %s (src: %s)", g.c_str(), server.arg("src").c_str());
  inputInject(gv, server.arg("src") == "boot" ? SRC_BOOT : SRC_WEB);
  ok();
}

static void hGoto() {
  LOCK();
  if (server.hasArg("i")) {
    logf("[WEB] Goto card #%s", server.arg("i").c_str());
    cardsGoto(server.arg("i").toInt());
  } else if (server.hasArg("dir")) {
    int dir = server.arg("dir").toInt() >= 0 ? 1 : -1;
    logf("[WEB] Card %s", dir > 0 ? "next" : "prev");
    cardsNext(dir);
  }
  UNLOCK();
  ok();
}

static void hAction() {
  logf("[WEB] Action: %s", server.arg("a").c_str());
  LOCK();
  doAction(server.arg("a").c_str());
  UNLOCK();
  ok();
}

static void hMessage() {
  JsonDocument d;
  if (!body(d)) { fail(400, "bad json"); return; }
  const char* text = d["text"] | "";
  if (!*text) { fail(400, "empty message"); return; }
  logf("[WEB] Message from '%s': %.40s", (const char*)(d["from"] | "Dashboard"), text);
  if (d["speak"] | false) {
    safeCopy(lastMsg.from, d["from"] | "Dashboard", sizeof(lastMsg.from));
    safeCopy(lastMsg.text, text, sizeof(lastMsg.text));
    lastMsg.t = time(nullptr);
    lastMsg.count++;
    notify(lastMsg.from, text, emoFromName(d["emo"] | "love"), d["sound"] | "sfx:msg", EV_MSG, true, 12);
  } else rcPush(RC_MSG, d["from"] | "Dashboard", text);
  ok();
}

static void hSay() {
  JsonDocument d;
  if (!body(d)) { fail(400, "bad json"); return; }
  if (!audioInstalled()) { fail(409, "speaker is not enabled"); return; }
  logf("[WEB] Say: '%.40s'", (const char*)(d["text"] | ""));
  emoSet(E_HAPPY, 3000);
  audioSay(d["text"] | "", EV_UI);
  ok();
}

static void hSound() {
  JsonDocument d;
  if (!body(d)) { fail(400, "bad json"); return; }
  if (!audioInstalled()) { fail(409, "speaker is not enabled"); return; }
  logf("[WEB] Sound API triggered");
  if (d["stop"] | false) audioStop();
  else if (!d["noise"].isNull()) { audioStop(); audioNoise(d["noise"] | 0, (d["min"] | 0) * 60000UL); }
  else if (!d["metro"].isNull()) { audioStop(); audioMetronome(d["metro"] | 0); }
  else if (!d["chirp"].isNull()) audioChirp(emoFromName(d["chirp"] | "happy"), EV_UI);
  else if (!d["vol"].isNull()) audioSetVolume(d["vol"] | 40);
  else audioPlaySpec(d["spec"] | "sfx:beep", EV_UI);
  ok();
}

static void hSounds() {
  JsonDocument d;
  JsonArray s = d["sfx"].to<JsonArray>();
  for (int i = 0; i < sfxCount(); i++) s.add(sfxName(i));
  JsonArray m = d["mel"].to<JsonArray>();
  LOCK();
  for (int i = 0; i < melodyCount(); i++) m.add(melodyName(i));
  UNLOCK();
  JsonArray f = d["files"].to<JsonArray>();
  File dir = LittleFS.open("/snd");
  if (dir) for (File e = dir.openNextFile(); e; e = dir.openNextFile()) f.add(String(e.name()));
  sendJson(d);
}

static void hEmo() {
  JsonDocument d;
  if (!body(d)) { fail(400, "bad json"); return; }
  int e = emoFromName(d["e"] | "");
  if (e < 0) { fail(400, "unknown emotion"); return; }
  logf("[WEB] Emo override: %s", (const char*)(d["e"] | ""));
  emoTouched();
  emoSet(e, d["ms"] | 5000);
  if (d["chirp"] | true) audioChirp(e, EV_UI);
  ok();
}

static void hPet() {
  String a = server.arg("a");
  logf("[WEB] Pet care: %s", a.c_str());
  if (a == "feed") emoFeed();
  else if (a == "play") emoPlay();
  else if (a == "sleep") emoSleepNow();
  else if (a == "wake") emoTouched();
  else if (a == "reset") emoResetStats();
  else { fail(400, "unknown action"); return; }
  ok();
}

static void hCmd() {
  String c;
  if (server.hasArg("c")) c = server.arg("c");
  else {
    JsonDocument d;
    if (body(d)) c = d["cmd"].as<String>();
  }
  if (!c.length()) { fail(400, "missing cmd"); return; }
  bool ansi = server.hasArg("ansi") && server.arg("ansi") == "1";
  String res = terminalExecute(c, ansi);
  server.send(200, "text/plain", res);
}

static void hPc() {
  JsonDocument d;
  if (!body(d)) { fail(400, "bad json"); return; }
  pcStats.cpu = d["cpu"] | 0.0f;
  pcStats.ram = d["ram"] | 0.0f;
  pcStats.gpu = d["gpu"] | -1.0f;
  pcStats.cpuT = d["cpuT"] | -1.0f;
  pcStats.gpuT = d["gpuT"] | -1.0f;
  pcStats.up = d["up"] | 0.0f;
  pcStats.down = d["down"] | 0.0f;
  safeCopy(pcStats.song, d["song"] | "", sizeof(pcStats.song));
  safeCopy(pcStats.artist, d["artist"] | "", sizeof(pcStats.artist));
  pcStats.playing = d["playing"] | false;
  pcStats.ts = millis();
  ok();
}

// ------------------------------------------------------------------ files
static void hFiles() {
  JsonDocument d;
  d["total"] = LittleFS.totalBytes();
  d["used"] = LittleFS.usedBytes();
  JsonArray arr = d["files"].to<JsonArray>();
  for (const char* dn : {"/img", "/anim", "/snd"}) {
    if (server.hasArg("dir") && server.arg("dir") != dn) continue;
    File dir = LittleFS.open(dn);
    if (!dir) continue;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      JsonObject o = arr.add<JsonObject>();
      o["path"] = String(dn) + "/" + e.name();
      o["dir"] = dn;
      o["name"] = e.name();
      o["size"] = e.size();
    }
  }
  sendJson(d);
}

static void hFileGet() {
  String p = server.arg("path");
  if (!safePath(p) || !LittleFS.exists(p)) { fail(404, "not found"); return; }
  File f = LittleFS.open(p, "r");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void hFileDel() {
  String p = server.arg("path");
  if (!safePath(p) || !LittleFS.remove(p)) { fail(404, "not found"); return; }
  ok();
}

static void hUploadDone() { if (upErr.length()) fail(400, upErr.c_str()); else ok(); }

static void hUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upErr = "";
    String dir = server.arg("dir");
    if (dir != "/img" && dir != "/anim" && dir != "/snd") { upErr = "bad folder"; return; }
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < 8192) { upErr = "storage is full"; return; }
    upPath = dir + "/" + cleanName(server.hasArg("name") ? server.arg("name") : u.filename);
    upFile = LittleFS.open(upPath, "w");
    if (!upFile) upErr = "could not create file";
  } else if (u.status == UPLOAD_FILE_WRITE) {
    wdtFeed();
    if (upFile && !upErr.length() && upFile.write(u.buf, u.currentSize) != u.currentSize) upErr = "storage is full";
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    if (upErr.length() || u.status == UPLOAD_FILE_ABORTED) LittleFS.remove(upPath);
    else logf("Uploaded %s (%u bytes)", upPath.c_str(), (unsigned)u.totalSize);
  }
}

// ------------------------------------------------------------------ OTA / system
static void hOtaDone() {
  bool good = !Update.hasError();
  server.send(200, "application/json", good ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"update failed\"}");
  otaBusy = false;
  wdtEnable(true);
  if (good) restartAt = millis() + 1200;
}

static void hOta() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    otaBusy = true;
    wdtEnable(false);   // a 1.5 MB upload keeps the loop busy for a while
    audioStop();
    saveNow();
    u8g2.clearBuffer();
    u8g2.setFont(F_BOLD);
    textCenter(28, "Updating...");
    u8g2.setFont(F_SMALL);
    textCenter(44, "don't unplug me");
    displaySend();
    logf("OTA start: %s", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) logf("OTA ok, %u bytes", (unsigned)u.totalSize);
    else Update.printError(Serial);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaBusy = false;
    wdtEnable(true);
  }
}

static void hBackup() {
  // streamed piece by piece: no second copy of every document in RAM
  static const char A[] = "{\"config\":", B[] = ",\"cards\":", C[] = ",\"state\":", D[] = ",\"fw\":\"" FW_VERSION "\"}";
  LOCK();
  size_t len = strlen(A) + strlen(B) + strlen(C) + strlen(D) + measureJson(cfg) + measureJson(cardsDoc) + measureJson(stateDoc);
  server.sendHeader("Content-Disposition", "attachment; filename=emodesk-backup.json");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(len);
  server.send(200, "application/json", "");
  ChunkOut o;
  o.print(A); serializeJson(cfg, o);
  o.print(B); serializeJson(cardsDoc, o);
  o.print(C); serializeJson(stateDoc, o);
  o.print(D);
  o.done();
  UNLOCK();
}

static File restFile;
static String restErr;

static void hRestoreUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    restErr = "";
    restFile = LittleFS.open("/restore.tmp", "w");
    if (!restFile) restErr = "could not create temp restore file";
  } else if (u.status == UPLOAD_FILE_WRITE) {
    wdtFeed();
    if (restFile && !restErr.length() && restFile.write(u.buf, u.currentSize) != u.currentSize)
      restErr = "storage write failed";
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (restFile) restFile.close();
    if (restErr.length() || u.status == UPLOAD_FILE_ABORTED) LittleFS.remove("/restore.tmp");
  }
}

static void hRestoreDone() {
  if (restErr.length()) {
    LittleFS.remove("/restore.tmp");
    fail(400, restErr.c_str());
    return;
  }
  JsonDocument d;
  bool loaded = false;
  if (LittleFS.exists("/restore.tmp")) {
    File f = LittleFS.open("/restore.tmp", "r");
    if (f) {
      DeserializationError err = deserializeJson(d, f);
      f.close();
      LittleFS.remove("/restore.tmp");
      if (!err) loaded = true;
      else { fail(400, "invalid JSON in backup file"); return; }
    }
  }
  if (!loaded) {
    if (!body(d)) { fail(400, "not an EmoDesk backup"); return; }
  }

  if (!d["config"].is<JsonObject>()) { fail(400, "missing config in backup"); return; }
  bool hasCards = d["cards"]["cards"].is<JsonArray>() || d["cards"].is<JsonArray>();
  if (!hasCards) { fail(400, "missing cards in backup"); return; }

  LOCK();
  // Preserve passwords if masked in the backup
  if (strcmp(d["config"]["wifi"]["pass"] | "", MASK) == 0) {
    d["config"]["wifi"]["pass"] = cfg["wifi"]["pass"] | "";
  }
  if (strcmp(d["config"]["mqtt"]["pass"] | "", MASK) == 0) {
    d["config"]["mqtt"]["pass"] = cfg["mqtt"]["pass"] | "";
  }

  cfg.set(d["config"]);
  if (d["cards"]["cards"].is<JsonArray>()) {
    cardsDoc.set(d["cards"]);
  } else {
    cardsDoc.clear();
    cardsDoc["cards"] = d["cards"];
  }
  if (d["state"].is<JsonObject>()) stateDoc.set(d["state"]);
  UNLOCK();

  markDirty(DIRTY_CFG | DIRTY_CARDS | DIRTY_STATE);
  saveNow();
  ok();
  restartAt = millis() + 1000;
}

static void hPower() {
  String a = server.arg("a");
  if (a == "on" || a == "off" || a == "auto") { powerSetMode(a.c_str()); ok(); }
  else if (a == "sleep") { ok(); delay(200); powerSleepNow(0, "Sleeping"); }
  else fail(400, "unknown action");
}

static void hScan() {
  wdtFeed();
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n && a.size() < 20; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (JsonObject o : a) if (s == (o["s"] | "")) dup = true;
    if (dup) continue;
    JsonObject o = a.add<JsonObject>();
    o["s"] = s;
    o["r"] = WiFi.RSSI(i);
    o["e"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void hWifi() {
  JsonDocument d;
  if (!body(d)) { fail(400, "invalid json"); return; }
  LOCK();
  if (d["nets"].is<JsonArray>()) {
    JsonArray newNets = d["nets"].as<JsonArray>();
    JsonDocument oldNetsDoc;
    if (cfg["wifi"]["nets"].is<JsonArray>()) {
      oldNetsDoc.set(cfg["wifi"]["nets"]);
    }
    cfg["wifi"]["nets"].clear();
    int idx = 1;
    for (JsonObject n : newNets) {
      const char* s = n["ssid"] | "";
      if (!*s) continue;
      JsonObject netObj = cfg["wifi"]["nets"].add<JsonObject>();
      netObj["ssid"] = s;
      const char* p = n["pass"] | "";
      String finalPass = "";
      if (p && *p && strcmp(p, MASK) != 0) {
        finalPass = p;
      } else {
        // Look up old password for this SSID
        if (oldNetsDoc.is<JsonArray>()) {
          for (JsonObject oldN : oldNetsDoc.as<JsonArray>()) {
            if (String(s).equalsIgnoreCase(oldN["ssid"] | "")) {
              const char* op = oldN["pass"] | "";
              if (op && *op && strcmp(op, MASK) != 0) { finalPass = op; break; }
            }
          }
        }
        if (!finalPass.length()) {
          for (int k = 1; k <= 3; k++) {
            const char* sk = (k == 1) ? "ssid" : (k == 2 ? "ssid2" : "ssid3");
            const char* pk = (k == 1) ? "pass" : (k == 2 ? "pass2" : "pass3");
            if (String(s).equalsIgnoreCase(cfg["wifi"][sk] | "")) {
              const char* op = cfg["wifi"][pk] | "";
              if (op && *op && strcmp(op, MASK) != 0) { finalPass = op; break; }
            }
          }
        }
        if (!finalPass.length()) {
          if (String(s) == DEFAULT_WIFI_SSID) finalPass = DEFAULT_WIFI_PASS;
          else if (String(s) == DEFAULT_WIFI_SSID2) finalPass = DEFAULT_WIFI_PASS2;
          else if (String(s) == DEFAULT_WIFI_SSID3) finalPass = DEFAULT_WIFI_PASS3;
        }
      }
      netObj["pass"] = finalPass;
      if (idx == 1) { cfg["wifi"]["ssid"] = s; cfg["wifi"]["pass"] = finalPass; }
      else if (idx == 2) { cfg["wifi"]["ssid2"] = s; cfg["wifi"]["pass2"] = finalPass; }
      else if (idx == 3) { cfg["wifi"]["ssid3"] = s; cfg["wifi"]["pass3"] = finalPass; }
      idx++;
    }
    if (idx <= 2) { cfg["wifi"]["ssid2"] = ""; cfg["wifi"]["pass2"] = ""; }
    if (idx <= 3) { cfg["wifi"]["ssid3"] = ""; cfg["wifi"]["pass3"] = ""; }
  } else {
    if (d["ssid"].is<const char*>()) {
      cfg["wifi"]["ssid"] = d["ssid"] | "";
      if (d["pass"].is<const char*>() && strcmp(d["pass"] | "", MASK) != 0) cfg["wifi"]["pass"] = d["pass"] | "";
    }
    if (d["ssid2"].is<const char*>()) {
      cfg["wifi"]["ssid2"] = d["ssid2"] | "";
      if (d["pass2"].is<const char*>() && strcmp(d["pass2"] | "", MASK) != 0) cfg["wifi"]["pass2"] = d["pass2"] | "";
    }
    if (d["ssid3"].is<const char*>()) {
      cfg["wifi"]["ssid3"] = d["ssid3"] | "";
      if (d["pass3"].is<const char*>() && strcmp(d["pass3"] | "", MASK) != 0) cfg["wifi"]["pass3"] = d["pass3"] | "";
    }
  }
  if (!d["txPower"].isNull()) {
    cfg["wifi"]["txPower"] = d["txPower"].as<int>();
    cfg["wifi"]["tx"] = d["txPower"].as<int>();
  } else if (!d["tx"].isNull()) {
    cfg["wifi"]["txPower"] = d["tx"].as<int>();
    cfg["wifi"]["tx"] = d["tx"].as<int>();
  }
  UNLOCK();
  markDirty(DIRTY_CFG);
  saveNow();
  ok();
  restartAt = millis() + 1500;
}

static void hFactory() {
  if (cfg["lockReset"] | false) {
    logf("[WEB] Factory reset rejected: reset buttons locked in dashboard");
    fail(403, "Reset functionality is disabled in dashboard settings");
    return;
  }
  LittleFS.remove("/config.json");
  LittleFS.remove("/cards.json");
  LittleFS.remove("/state.json");
  if (server.arg("full") == "1") LittleFS.format();
  ok();
  restartAt = millis() + 1000;
}

// ------------------------------------------------------------------ setup
void webBegin() {
  server.on("/", HTTP_GET, [] {
    if (netPortal() && !netHasCreds() && !server.hasArg("full")) { redirectSetup(); return; }
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html", (const char*)DASH_GZ, DASH_GZ_LEN);
  });
  server.on("/setup", HTTP_GET, [] { server.send_P(200, "text/html", SETUP_HTML); });
  for (const char* p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/fwlink", "/redirect"})
    server.on(p, HTTP_GET, [] { if (netPortal()) redirectSetup(); else server.send(204); });

  server.on("/api/status", HTTP_GET, hStatus);
  server.on("/api/config", HTTP_GET, hConfigGet);
  server.on("/api/config", HTTP_POST, hConfigPost);
  server.on("/api/cards", HTTP_GET, hCardsGet);
  server.on("/api/cards", HTTP_POST, hCardsPost);
  server.on("/api/screen", HTTP_GET, hScreen);
  server.on("/api/input", HTTP_POST, hInput);
  server.on("/api/goto", HTTP_POST, hGoto);
  server.on("/api/action", HTTP_POST, hAction);
  server.on("/api/message", HTTP_POST, hMessage);
  server.on("/api/say", HTTP_POST, hSay);
  server.on("/api/sound", HTTP_POST, hSound);
  server.on("/api/sounds", HTTP_GET, hSounds);
  server.on("/api/emo", HTTP_POST, hEmo);
  server.on("/api/pet", HTTP_POST, hPet);
  server.on("/api/pc", HTTP_POST, hPc);
  server.on("/api/files", HTTP_GET, hFiles);
  server.on("/api/file", HTTP_GET, hFileGet);
  server.on("/api/file", HTTP_DELETE, hFileDel);
  server.on("/api/upload", HTTP_POST, hUploadDone, hUpload);
  server.on("/api/update", HTTP_POST, hOtaDone, hOta);
  server.on("/api/backup", HTTP_GET, hBackup);
  server.on("/api/restore", HTTP_POST, hRestoreDone, hRestoreUpload);
  server.on("/api/scan", HTTP_GET, hScan);
  server.on("/api/wifi", HTTP_POST, hWifi);
  server.on("/api/cmd", HTTP_POST, hCmd);
  server.on("/api/cmd", HTTP_GET, hCmd);
  server.on("/api/log", HTTP_GET, [] { server.send(200, "text/plain", logDump()); });
  server.on("/api/brief", HTTP_POST, [] { briefingNow(); ok(); });
  server.on("/api/telegram", HTTP_POST, [] {
    JsonDocument d;
    body(d);
    tgSendNow(d["text"] | "Hello from EmoDesk!");
    ok();
  });
  server.on("/api/sports/matches", HTTP_GET, [] {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < cric.n; i++) {
      const CricMatch& m = cric.m[i];
      JsonObject o = arr.add<JsonObject>();
      o["id"] = m.id;
      o["name"] = m.name;
      o["title"] = m.name;
      o["t1"] = m.t1;
      o["t2"] = m.t2;
      o["t1n"] = m.t1n;
      o["t2n"] = m.t2n;
      o["series"] = m.series;
      o["status"] = m.status;
      o["live"] = cricLive(m);
      o["started"] = m.started;
      o["ended"] = m.ended;
      o["type"] = m.type;
      char sc1[32] = "", sc2[32] = "";
      cricScore(m, 0, sc1, sizeof(sc1));
      cricScore(m, 1, sc2, sizeof(sc2));
      o["sc1"] = sc1;
      o["sc2"] = sc2;
      o["recent"] = m.recent;
      o["rrr"] = m.rrr;
      o["remRuns"] = m.remRuns;
      o["partner"] = m.partner;
      o["winProb"] = m.winProb;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/refresh", HTTP_POST, [] { cardRefreshNow(server.arg("i").toInt()); feedsRefresh(); ok(); });
  server.on("/api/reboot", HTTP_POST, [] { saveNow(); ok(); restartAt = millis() + 800; });
  server.on("/api/factory", HTTP_POST, hFactory);
  server.on("/api/power", HTTP_POST, hPower);
  server.onNotFound([] {
    if (netPortal()) { redirectSetup(); return; }
    fail(404, "not found");
  });
  server.begin();
  logf("Web server ready");
}

void webLoop() {
  server.handleClient();
  if (restartAt && (int32_t)(millis() - restartAt) > 0) {
    saveNow();
    delay(100);
    ESP.restart();
  }
}
