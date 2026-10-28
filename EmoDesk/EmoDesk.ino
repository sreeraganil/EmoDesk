/*
  EmoDesk - an Emo-style desktop companion
  ESP32-C3 Super Mini + SSD1306 128x64 + TTP223 touch (+ optional MAX98357A speaker)

  Arduino IDE: board "ESP32C3 Dev Module", USB CDC On Boot: Enabled,
  Flash size 4MB, Partition scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
  (the sketch's own partitions.csv defines the layout; Huge APP ensures IDE size check matches).
  Requires Arduino-ESP32 core 3.x. See README.md.
*/
// ---- Every library used anywhere in the project, listed here on purpose. ----
// Some IDEs (e.g. ArduinoDroid) only compile libraries included directly from
// the .ino, which causes "undefined reference" link errors otherwise.
#include "config.h"
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <Network.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <AsyncUDP.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>
#include <ESP_I2S.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#if ENABLE_MQTT
#include <PubSubClient.h>
#endif
#if ENABLE_TTS
#include <AudioOutput.h>
#include <ESP8266SAM.h>
#endif
#if ENABLE_BLE
#include <BLEDevice.h>          // built into the ESP32 core - do NOT install NimBLE-Arduino
#endif

#include "core.h"
#include "display.h"
#include "input.h"
#include "emo.h"
#include "audio.h"
#include "net.h"
#include "feeds.h"
#include "cards.h"
#include "services.h"
#include "web.h"
#include "power.h"
#include "terminal.h"

SET_LOOP_TASK_STACK_SIZE(LOOP_STACK);

static uint32_t lastFrame = 0;

static void render() {
  if (!displayIsOn()) return;
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  LOCK();
  if (netPortal() && !netHasCreds()) netDrawPortal();
  else if (notifActive()) notifDraw();
  else cardsDraw();
  UNLOCK();
  
  int bat = batteryPercent();
  if (bat >= 0 && cardsCurrentShowBat()) {
    char buf[12];
    if (powerSaver()) snprintf(buf, sizeof(buf), "e %d%%", bat);
    else snprintf(buf, sizeof(buf), "%d%%", bat);
    u8g2.setFont(F_TINY);
    int w = u8g2.getStrWidth(buf);
    int bw = 11;
    int bh = 6;
    int gap = 3;
    int tw = w + gap + bw + 2;
    int x = 126 - tw;

    u8g2.setDrawColor(0);
    u8g2.drawRBox(x - 3, 0, tw + 4, 10, 2);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(x - 3, 0, tw + 4, 10, 2);

    u8g2.drawStr(x, 7, buf);

    int bx = x + w + gap;
    int by = 2;
    u8g2.drawFrame(bx, by, bw, bh);
    u8g2.drawBox(bx + bw, by + 1, 1, bh - 2); // battery positive tip
    int fill = max(0, (bw - 3) * bat / 100);
    if (fill > 0) u8g2.drawBox(bx + 1, by + 1, fill, bh - 2);
  }

  toastDraw();
  displaySend();
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  Serial.setTxTimeoutMs(0);
#endif
  delay(200);
  coreBegin();
  logf("EmoDesk %s booting (%s)", FW_VERSION, resetReasonStr());
  powerBegin();
  displayBegin();
  inputBegin();
  emoBegin();
  audioBegin();
  servicesBegin();

  // little hello while everything starts
  u8g2.clearBuffer();
  emoRender(64, 26, 0.8f);
  u8g2.setFont(F_SMALL);
  textCenter(60, "waking up...");
  displaySend();
  emoSet(lastBootCrashed() ? E_DIZZY : E_HAPPY, 3000);
  audioSfx("startup", EV_UI);

  bleBegin();
  netBegin();
  cardsBegin();
  webBegin();
  netStartTask();
  logf("Ready. Dashboard: http://%s.local  (%s)  %u KB free", (const char*)(cfg["host"] | DEFAULT_HOST), netIP().c_str(),
       (unsigned)(ESP.getFreeHeap() / 1024));
  if (gSafeMode) toastShow("Safe mode");
  terminalBegin();
  wdtEnable(true);   // from here on a frozen loop reboots itself instead of hanging forever
}

void loop() {
  wdtFeed();
  terminalPoll();
  inputPoll();
  netLoop();
  webLoop();
  coreLoop();
  emoUpdate();
  servicesLoop();
  powerLoop();

  uint8_t g, src;
  while (inputGet(g, src)) handleGesture(g, src);

  cardsLoop();

  uint32_t now = millis();
  if (now - lastFrame >= powerFrameMs()) {
    lastFrame = now;
    render();
  }
  delay(!displayIsOn() ? 20 : (powerSaver() ? 6 : 1));   // longer idle slices let the CPU nap between frames
}
