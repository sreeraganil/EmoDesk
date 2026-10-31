#include "terminal.h"
#include "core.h"
#include "display.h"
#include "input.h"
#include "emo.h"
#include "audio.h"
#include "cards.h"
#include "services.h"
#include "net.h"
#include "power.h"
#include <WiFi.h>

static char lineBuf[192];
static uint8_t lineLen = 0;

void terminalBegin() {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  Serial.setTxTimeoutMs(0);
#endif
  Serial.println();
  Serial.println("\033[1;36m====================================================\033[0m");
  Serial.printf("\033[1;33m       EmoDesk ESP32-C3 Live Console & CLI\033[0m\r\n");
  Serial.printf("\033[1;32m       Firmware v%s - Ready\033[0m\r\n", FW_VERSION);
  Serial.println("\033[1;36m====================================================\033[0m");
  Serial.println("Type \033[1;37mhelp\033[0m for available commands.\r\n");
  Serial.print("\033[1;32memo>\033[0m ");
}

static String formatUptime() {
  uint32_t s = millis() / 1000;
  uint32_t d = s / 86400;
  uint32_t hh = (s / 3600) % 24;
  uint32_t m = (s / 60) % 60;
  uint32_t sec = s % 60;
  char b[32];
  if (d > 0) snprintf(b, sizeof(b), "%ud %uh %um %us", d, hh, m, sec);
  else if (hh > 0) snprintf(b, sizeof(b), "%uh %um %us", hh, m, sec);
  else snprintf(b, sizeof(b), "%um %us", m, sec);
  return String(b);
}

String terminalExecute(const String& cmdLine, bool ansi) {
  String line = cmdLine;
  line.trim();
  if (line.length() == 0) return "";

  // Split command and argument
  int sp = line.indexOf(' ');
  String cmd = (sp >= 0) ? line.substring(0, sp) : line;
  String arg = (sp >= 0) ? line.substring(sp + 1) : "";
  cmd.toLowerCase();
  arg.trim();

  const char* C_OK   = ansi ? "\033[1;32m" : "";
  const char* C_ERR  = ansi ? "\033[1;31m" : "";
  const char* C_CYAN = ansi ? "\033[1;36m" : "";
  const char* C_YEL  = ansi ? "\033[1;33m" : "";
  const char* C_RST  = ansi ? "\033[0m" : "";

  logf("[CLI] Execute: '%s'", line.c_str());

  if (cmd == "help" || cmd == "?") {
    String out;
    out += String(C_CYAN) + "=== EmoDesk Terminal Commands ===" + C_RST + "\r\n";
    out += "  status                 - Device health, heap, Wi-Fi, battery, active card\r\n";
    out += "  heap                   - Detailed RAM breakdown & stack high-water\r\n";
    out += "  cards                  - List all loaded cards and visibility\r\n";
    out += "  card <next|prev|home|goto <N>> - Switch active card\r\n";
    out += "  touch <tap|double|triple|hold|vlong> - Simulate touch sensor gesture\r\n";
    out += "  boot <tap|double|triple|hold|vlong>  - Simulate BOOT button gesture\r\n";
    out += "  emo <mood> [ms]        - Set emotion (happy, love, sad, angry, dizzy, bored...)\r\n";
    out += "  emo <pet|feed|sleep|wake> - Pet interactions\r\n";
    out += "  say <text>             - Speak text aloud (or show alert)\r\n";
    out += "  msg <text>             - Display notification overlay\r\n";
    out += "  play <spec>            - Play audio (sfx:bell, mel:2, /snd/file.wav)\r\n";
    out += "  sound [vol <0-100>|mute|unmute] - Audio volume controls\r\n";
    out += "  screen [on|off|toggle|bright <0-255>] - OLED display controls\r\n";
    out += "  saver [on|off]         - Toggle battery saver mode\r\n";
    out += "  wifi                   - Show Wi-Fi details\r\n";
    out += "  wifi <ssid> <pass>     - Change Wi-Fi credentials and reconnect\r\n";
    out += "  scan                   - Scan for nearby Wi-Fi access points\r\n";
    out += "  action <action>        - Run internal action (pet, feed, brief, refresh...)\r\n";
    out += "  media <play|next|prev|volup|voldown|mute> - Send PC media controls (USB/BLE)\r\n";
    out += "  log                    - Dump recent in-memory log buffer\r\n";
    out += "  clear                  - Clear terminal screen\r\n";
    out += "  reboot                 - Restart ESP32-C3\r\n";
    out += "  factory                - Reset configuration to defaults\r\n";
    return out;
  }

  if (cmd == "clear") {
    return ansi ? "\033[2J\033[H" : "\r\n--- Screen Cleared ---\r\n";
  }

  if (cmd == "status") {
    int bat = batteryPercent();
    int mv = batteryMv();
    String wifiStatus = netOnline() ? ("Connected (" + WiFi.SSID() + ", IP: " + netIP() + ", RSSI: " + String(netRSSI()) + " dBm)") :
                        (netPortal() ? "Setup AP ('EmoDesk-Setup')" : "Disconnected");
    String resetStr = String(resetReasonStr()) + " (Crashes: " + String(bootCrashCount()) + (gSafeMode ? ", SAFE MODE ACTIVE" : "") + ")";

    String out;
    out += String(C_CYAN) + "=== EmoDesk Status ===" + C_RST + "\r\n";
    out += "  Firmware:    " + String(FW_VERSION) + "\r\n";
    out += "  Uptime:      " + formatUptime() + "\r\n";
    out += "  Reset Reason:" + resetStr + "\r\n";
    out += "  CPU / Heap:  " + String(ESP.getCpuFreqMHz()) + " MHz | Free: " + String(ESP.getFreeHeap() / 1024) +
           " KB (Largest block: " + String(heapLargest() / 1024) + " KB)\r\n";
    out += "  Wi-Fi:       " + wifiStatus + "\r\n";
    out += "  Battery:     " + (bat >= 0 ? (String(bat) + "% (" + String(mv) + " mV)") : String("No divider detected")) +
           " | Saver: " + String(powerSaver() ? "ACTIVE" : "OFF") + "\r\n";
    out += "  Display:     " + String(screenIsOn() ? "ON" : "OFF") + "\r\n";
    out += "  Audio:       Volume " + String(audioVolume()) + "% | Muted: " + String(audioMuted() ? "YES" : "NO") +
           " | Speaker: " + String(audioInstalled() ? "Installed" : "Disabled") + "\r\n";
    out += "  Active Card: #" + String(cardsCurrent()) + " [" + String(cardsCurrentType()) + "] (" +
           cardName(cardsCurrent(), cardsCurrentType()) + ")\r\n";
    out += "  Pet Stats:   Happy " + String((int)pet.happy) + " | Energy " + String((int)pet.energy) +
           " | Hunger " + String((int)pet.hunger) + " | Total Interactions " + String(pet.interactions) + "\r\n";
    return out;
  }

  if (cmd == "heap") {
    String out;
    out += String(C_CYAN) + "=== Memory Breakdown ===" + C_RST + "\r\n";
    out += "  Free Heap:          " + String(ESP.getFreeHeap()) + " B (" + String(ESP.getFreeHeap() / 1024) + " KB)\r\n";
    out += "  Min Free Ever:      " + String(ESP.getMinFreeHeap()) + " B (" + String(ESP.getMinFreeHeap() / 1024) + " KB)\r\n";
    out += "  Largest Free Block: " + String(heapLargest()) + " B (" + String(heapLargest() / 1024) + " KB)\r\n";
    out += "  Net Stack Free:     " + String(gNetStackFree) + " B\r\n";
    out += "  Audio Stack Free:   " + String(gAudioStackFree) + " B\r\n";
    return out;
  }

  if (cmd == "cards") {
    int count = cardsCount();
    String out;
    out += String(C_CYAN) + "=== Configured Cards (Total: " + String(count) + ") ===" + C_RST + "\r\n";
    for (int i = 0; i < count; i++) {
      bool active = (i == cardsCurrent());
      bool vis = cardVisible(i);
      out += (active ? String(C_YEL) + "> " : "  ") + "[" + String(i) + "] " +
             String(cardsTypeAt(i)) + " (\"" + cardName(i, cardsTypeAt(i)) + "\") - " +
             String(vis ? "visible" : "hidden/inactive") + String(active ? C_RST : "") + "\r\n";
    }
    return out;
  }

  if (cmd == "card") {
    if (arg == "next") { cardsNext(1); return String(C_OK) + "[OK] Switched to next card" + C_RST + "\r\n"; }
    if (arg == "prev") { cardsNext(-1); return String(C_OK) + "[OK] Switched to previous card" + C_RST + "\r\n"; }
    if (arg == "home") { cardsHome(); return String(C_OK) + "[OK] Returned to Home card" + C_RST + "\r\n"; }
    if (arg.startsWith("goto ")) {
      int idx = arg.substring(5).toInt();
      if (idx >= 0 && idx < cardsCount()) {
        cardsGoto(idx);
        return String(C_OK) + "[OK] Switched to card #" + String(idx) + C_RST + "\r\n";
      }
      return String(C_ERR) + "[ERR] Invalid card index" + C_RST + "\r\n";
    }
    return String(C_ERR) + "Usage: card <next|prev|home|goto <N>>" + C_RST + "\r\n";
  }

  if (cmd == "touch" || cmd == "boot") {
    uint8_t src = (cmd == "boot") ? SRC_BOOT : SRC_TOUCH;
    uint8_t g = G_NONE;
    if (arg == "tap" || arg == "") g = G_TAP;
    else if (arg == "double") g = G_DOUBLE;
    else if (arg == "triple") g = G_TRIPLE;
    else if (arg == "hold" || arg == "long") g = G_LONG;
    else if (arg == "vlong") g = G_VLONG;
    else return String(C_ERR) + "Unknown gesture. Options: tap, double, triple, hold, vlong" + C_RST + "\r\n";

    inputInject(g, src);
    return String(C_OK) + "[OK] Injected " + cmd + " gesture: " + gestureName(g) + C_RST + "\r\n";
  }

  if (cmd == "emo") {
    if (arg == "pet" || arg == "play") { emoPlay(); return String(C_OK) + "[OK] Petted Emo!" + C_RST + "\r\n"; }
    if (arg == "feed") { emoFeed(); return String(C_OK) + "[OK] Fed Emo!" + C_RST + "\r\n"; }
    if (arg == "sleep") { emoSleepNow(); return String(C_OK) + "[OK] Emo put to sleep." + C_RST + "\r\n"; }
    if (arg == "wake") { emoTouched(); return String(C_OK) + "[OK] Woke Emo up." + C_RST + "\r\n"; }

    int sp2 = arg.indexOf(' ');
    String name = (sp2 >= 0) ? arg.substring(0, sp2) : arg;
    uint32_t ms = (sp2 >= 0) ? arg.substring(sp2 + 1).toInt() : 5000;
    if (ms == 0) ms = 5000;

    int e = emoFromName(name.c_str());
    if (e >= 0) {
      emoTouched();
      emoSet(e, ms);
      audioChirp(e, EV_UI);
      return String(C_OK) + "[OK] Emo expression set to '" + name + "' for " + String(ms) + " ms" + C_RST + "\r\n";
    }
    String valid = "";
    for (int i = 0; i < E_COUNT; i++) { valid += EMO_NAMES[i]; if (i < E_COUNT - 1) valid += ", "; }
    return String(C_ERR) + "[ERR] Unknown emotion. Available: " + valid + C_RST + "\r\n";
  }

  if (cmd == "say") {
    if (arg.length() == 0) return String(C_ERR) + "Usage: say <text>" + C_RST + "\r\n";
    emoSet(E_HAPPY, 3000);
    audioSay(arg, EV_UI);
    return String(C_OK) + "[OK] Spoke: \"" + arg + "\"" + C_RST + "\r\n";
  }

  if (cmd == "msg") {
    if (arg.length() == 0) return String(C_ERR) + "Usage: msg <text>" + C_RST + "\r\n";
    rcPush(RC_MSG, "Terminal", arg.c_str());
    return String(C_OK) + "[OK] Message pushed to screen" + C_RST + "\r\n";
  }

  if (cmd == "play") {
    if (arg.length() == 0) return String(C_ERR) + "Usage: play <spec> (e.g. sfx:bell, mel:2, /snd/file.wav)" + C_RST + "\r\n";
    audioPlaySpec(arg.c_str(), EV_UI);
    return String(C_OK) + "[OK] Playing audio: " + arg + C_RST + "\r\n";
  }

  if (cmd == "sound") {
    if (arg == "mute") { audioSetMuted(true); return String(C_OK) + "[OK] Sound muted" + C_RST + "\r\n"; }
    if (arg == "unmute") { audioSetMuted(false); return String(C_OK) + "[OK] Sound unmuted" + C_RST + "\r\n"; }
    if (arg.startsWith("vol ")) {
      int v = arg.substring(4).toInt();
      audioSetVolume(v);
      return String(C_OK) + "[OK] Volume set to " + String(audioVolume()) + "%" + C_RST + "\r\n";
    }
    return String(C_CYAN) + "Sound: Volume " + String(audioVolume()) + "%, Muted: " + String(audioMuted() ? "YES" : "NO") + C_RST + "\r\n";
  }

  if (cmd == "screen") {
    if (arg == "on") { screenSetPower(true); return String(C_OK) + "[OK] Screen turned ON" + C_RST + "\r\n"; }
    if (arg == "off") { screenSetPower(false); return String(C_OK) + "[OK] Screen turned OFF" + C_RST + "\r\n"; }
    if (arg == "toggle") { screenToggle(); return String(C_OK) + "[OK] Screen toggled" + C_RST + "\r\n"; }
    if (arg.startsWith("bright ")) {
      int b = arg.substring(7).toInt();
      displaySetContrast(b);
      return String(C_OK) + "[OK] Display brightness set to " + String(b) + C_RST + "\r\n";
    }
    return String(C_CYAN) + "Screen is " + String(screenIsOn() ? "ON" : "OFF") + C_RST + "\r\n";
  }

  if (cmd == "saver") {
    if (arg == "on") { powerSetMode("on"); return String(C_OK) + "[OK] Battery saver ON" + C_RST + "\r\n"; }
    if (arg == "off") { powerSetMode("off"); return String(C_OK) + "[OK] Battery saver OFF" + C_RST + "\r\n"; }
    return String(C_CYAN) + "Battery saver is " + String(powerSaver() ? "ON" : "OFF") + C_RST + "\r\n";
  }

  if (cmd == "wifi") {
    if (arg.length() == 0) {
      String statusStr = netOnline() ? ("Connected (" + WiFi.SSID() + ")") : (netPortal() ? "Setup AP Active" : "Disconnected");
      String out;
      out += String(C_CYAN) + "=== Wi-Fi Status ===" + C_RST + "\r\n";
      out += "  Profile #1: " + netSSID(1) + " (pass: " + (netPass(1).length() > 0 ? "set" : "EMPTY") + ")\r\n";
      out += "  Profile #2: " + netSSID(2) + " (pass: " + (netPass(2).length() > 0 ? "set" : "EMPTY") + ")\r\n";
      out += "  Profile #3: " + netSSID(3) + " (pass: " + (netPass(3).length() > 0 ? "set" : "EMPTY") + ")\r\n";
      out += "  Status:     " + statusStr + "\r\n";
      out += "  IP Address: " + netIP() + "\r\n";
      out += "  Signal:     " + String(netRSSI()) + " dBm\r\n";
      return out;
    }
    int sp2 = arg.indexOf(' ');
    String ssid = (sp2 >= 0) ? arg.substring(0, sp2) : arg;
    String pass = (sp2 >= 0) ? arg.substring(sp2 + 1) : "";
    LOCK();
    cfg["wifi"]["ssid"] = ssid;
    cfg["wifi"]["pass"] = pass;
    if (cfg["wifi"]["nets"].is<JsonArray>()) {
      JsonArray arr = cfg["wifi"]["nets"].as<JsonArray>();
      bool updated = false;
      for (JsonObject o : arr) {
        if (ssid.equalsIgnoreCase(o["ssid"] | "")) {
          o["pass"] = pass;
          updated = true;
          break;
        }
      }
      if (!updated && arr.size() > 0) {
        arr[0]["ssid"] = ssid;
        arr[0]["pass"] = pass;
      }
    }
    UNLOCK();
    markDirty(DIRTY_CFG);
    saveNow();
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());
    return String(C_OK) + "[OK] Saved Wi-Fi credentials for '" + ssid + "'. Reconnecting..." + C_RST + "\r\n";
  }

  if (cmd == "scan") {
    int n = WiFi.scanNetworks();
    String out;
    out += String(C_CYAN) + "=== Wi-Fi Scan (" + String(n) + " found) ===" + C_RST + "\r\n";
    for (int i = 0; i < n; i++) {
      String secStr = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "[OPEN]" : "[SECURE]";
      out += "  " + String(i + 1) + ". " + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm) " + secStr + "\r\n";
    }
    WiFi.scanDelete();
    return out;
  }

  if (cmd == "action") {
    if (arg.length() == 0) return String(C_ERR) + "Usage: action <name> (e.g. pet, feed, brief, rotate, refresh)" + C_RST + "\r\n";
    doAction(arg.c_str());
    return String(C_OK) + "[OK] Executed action: " + arg + C_RST + "\r\n";
  }

  if (cmd == "log") {
    return String(C_CYAN) + "=== In-Memory Logs ===" + C_RST + "\r\n" + logDump();
  }

  if (cmd == "reboot") {
    saveNow();
    Serial.println("\033[1;33mRebooting ESP32...\033[0m");
    delay(200);
    ESP.restart();
    return "[OK] Rebooting...";
  }

  if (cmd == "factory") {
    if (cfg["lockReset"] | false) {
      return String(C_ERR) + "[ERR] Factory reset is disabled in dashboard settings." + C_RST + "\r\n";
    }
    LittleFS.remove("/config.json");
    LittleFS.remove("/cards.json");
    LittleFS.remove("/state.json");
    Serial.println("\033[1;31mFactory reset: Configuration erased. Rebooting...\033[0m");
    delay(200);
    ESP.restart();
    return "[OK] Resetting...";
  }

  return String(C_ERR) + "Unknown command '" + cmd + "'. Type 'help' for a list of commands." + C_RST + "\r\n";
}

void terminalPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        Serial.println(); // Echo newline
        String res = terminalExecute(String(lineBuf), true);
        if (res.length() > 0) Serial.print(res);
        lineLen = 0;
        Serial.print("\033[1;32memo>\033[0m ");
      } else {
        Serial.println();
        Serial.print("\033[1;32memo>\033[0m ");
      }
    } else if (c == '\b' || c == 0x7F) { // Backspace
      if (lineLen > 0) {
        lineLen--;
        Serial.print("\b \b"); // visually erase character
      }
    } else if (c >= 32 && c <= 126) {
      if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
        Serial.print(c); // Echo character back to terminal
      }
    }
  }
}
