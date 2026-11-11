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

  int bat = batteryPercent();
  bool showBat = (bat >= 0 && cardsCurrentShowBat() && !(netPortal() && !netHasCreds()) && !notifActive());
  char batBuf[12] = "";
  int batW = 0, batTw = 0;
  if (showBat) {
    if (powerSaver()) snprintf(batBuf, sizeof(batBuf), "e%d%%", bat);
    else snprintf(batBuf, sizeof(batBuf), "%d%%", bat);
    u8g2.setFont(F_TINY);
    batTw = u8g2.getStrWidth(batBuf);
    batW = batTw + 18; // text + 2px gap + 11px battery + 1px tip + 4px padding
    displaySetBatReserve(batW + 2);
  } else {
    displaySetBatReserve(0);
  }

  LOCK();
  if (netPortal() && !netHasCreds()) netDrawPortal();
  else if (notifActive()) notifDraw();
  else cardsDraw();
  UNLOCK();

  if (showBat) {
    u8g2.setFont(F_TINY);
    int bx0 = 127 - batW;
    u8g2.setDrawColor(0);
    u8g2.drawRBox(bx0, 0, batW + 1, 10, 2);
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(bx0, 0, batW + 1, 10, 2);
    u8g2.drawStr(bx0 + 3, 7, batBuf);
    int ibx = bx0 + 3 + batTw + 2;
    u8g2.drawFrame(ibx, 2, 10, 6);
    u8g2.drawBox(ibx + 10, 3, 1, 4);
    int fill = max(0, 8 * bat / 100);
    if (fill > 0) u8g2.drawBox(ibx + 1, 3, fill, 4);
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
  drawHeader("EmoDesk", "v" FW_VERSION);
  emoRender(64, 29, 0.72f);
  u8g2.setFont(F_SMALL);
  textCenter(59, "Waking up...");
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
  uint16_t frameMs = powerFrameMs();
  if (now - lastFrame >= frameMs) {
    if (now - lastFrame > frameMs * 2) lastFrame = now;
    else lastFrame += frameMs;
    render();
  }
  delay(!displayIsOn() ? 20 : (powerSaver() ? 6 : 1));   // longer idle slices let the CPU nap between frames
}
