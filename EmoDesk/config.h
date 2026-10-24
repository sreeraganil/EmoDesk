#pragma once
// =====================================================================
//  EmoDesk - desktop companion for ESP32-C3 Super Mini
//  Hardware: SSD1306 128x64 I2C OLED, TTP223 touch, (optional) MAX98357A
// =====================================================================

#define FW_VERSION      "1.2.0"
#define DEFAULT_HOST    "emo"          // dashboard at http://emo.local

// ---------------- Pins (see WIRING.md) ----------------
#define PIN_SDA         4
#define PIN_SCL         5
#define PIN_TOUCH       3              // TTP223 OUT (active HIGH), also wakes Emo from deep sleep
#define PIN_BOOT        9              // on-board BOOT button (active LOW)
#define PIN_LED         8              // on-board blue LED
#define LED_ACTIVE_LOW  1
#define PIN_BATTERY     0              // battery divider (2 x 10k) to GPIO0 / ADC1. Remove the line if unused
#define PIN_I2S_BCLK    6              // MAX98357 BCLK
#define PIN_I2S_LRC     7              // MAX98357 LRC / WS
#define PIN_I2S_DIN     10             // MAX98357 DIN
#define PIN_AMP_SD      1              // MAX98357 SD (amp on/off)

#define OLED_I2C_HZ     800000

// ---------------- Optional features ----------------
// Robot voice (needs libraries ESP8266Audio + ESP8266SAM). Set 0 if they fail to build.
#define ENABLE_TTS      0
// MQTT / Home Assistant (needs PubSubClient)
#define ENABLE_MQTT     0
// Bluetooth (phone presence / media remote) .
// Uses the BLE library that ships with the ESP32 core (3.3+). Do NOT install the
// separate "NimBLE-Arduino" library: on core 3.3.7+ two NimBLE stacks clash and the
// board reboots in a loop at "waking up...". Costs about 45 KB of RAM when on.
#define ENABLE_BLE      0

// ---------------- Internals ----------------
#define SAMPLE_RATE     22050
#define MAX_CARDS       40
#define FRAME_MS        33             // ~30 fps; battery saver lowers this
#define LOOP_STACK      (12 * 1024)    // was 16 KB, measured peak is well under 8 KB
#define NET_STACK       (16 * 1024)    // TLS handshake needs ~7 KB
#define WDT_SECONDS     20             // reboot if the main loop freezes this long (0 = off)
