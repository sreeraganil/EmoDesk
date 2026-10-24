#pragma once
#include "core.h"

void servicesBegin();
void servicesLoop();                       // alarms, reminders, chime, briefing, overlays, LED, screen power
bool notifActive();
void notifDraw();
void toastShow(const char* text);
void toastDraw();
void handleGesture(uint8_t g, uint8_t src);
void doAction(const char* action);
void ledBlink(int n);
void displayWake();
void screenSetPower(bool on);
bool screenIsOn();
void screenToggle();
void briefingNow();
String briefingText();

// Bluetooth (ble.cpp) - stubs when ENABLE_BLE is 0
void bleBegin();
void bleApply();
void bleLoop();
void bleMedia(const char* key);            // play, next, prev, volup, voldown, mute
bool blePresent();
const char* bleMode();                     // "off" | "presence" | "remote" — no allocation
void bleStatusCopy(char* buf, size_t n);  // writes status string into caller buffer — no allocation
String bleStatus();
