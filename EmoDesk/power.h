#pragma once
#include "core.h"

// Battery saver + deep sleep.
// cfg.power: saver "off" | "auto" | "on", at (%), cpu, fps, dim, offMin, slowNet, ble, led, quiet,
//            lowAlert, critPct, night (deep sleep window) nFrom / nTo
void powerBegin();
void powerLoop();                 // main loop, cheap
bool powerSaver();                // saver currently active
uint16_t powerFrameMs();          // frame interval to use
uint8_t powerBrightCap();         // max OLED contrast (255 = no cap)
uint32_t powerScale();            // multiplier for network refresh intervals (1 or 3)
bool powerLedOff();
bool powerQuiet();                // mute everything except alarms
bool powerPauseBle();
int  powerOffIdleMin();           // screen-off idle minutes while saving (0 = never)
void powerSetMode(const char* m); // "off" "on" "auto"
const char* powerModeName();
void powerApplyRadio();           // re-apply Wi-Fi sleep + CPU after connect / config change
void powerSleepNow(uint32_t wakeAfterMin = 0, const char* why = "Sleeping");
String powerWakeReason();
void powerActivity();             // the dashboard is open: postpone night sleep
