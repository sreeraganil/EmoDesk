#pragma once
#include "core.h"
#include <functional>

void netBegin();                 // connect (blocking up to ~20 s) or start setup AP
void netLoop();                  // main loop housekeeping (captive DNS, reconnect)
void netStartTask();             // background fetch task (feeds + cards)
bool netOnline();
bool netPortal();
bool netHasCreds();
String netIP();
int  netRSSI();
void netDrawPortal();
void timeApply();
String netSSID(int p);
String netPass(int p);
void netApplyTxPower();

// HTTP helpers - call from the network task only
int  httpGet(const String& url, std::function<bool(Stream&)> cb, const char* hName = nullptr,
             const char* hVal = nullptr, bool browserUA = false);
bool httpJson(const String& url, JsonDocument& doc, JsonDocument* filter = nullptr, const char* hName = nullptr,
              const char* hVal = nullptr, bool browserUA = false);
String urlEncode(const char* s);
