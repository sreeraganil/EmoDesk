#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"

// ---------- Persistent documents ----------
extern JsonDocument cfg;       // /config.json  - all settings
extern JsonDocument cardsDoc;  // /cards.json   - {"cards":[...]}
extern JsonDocument stateDoc;  // /state.json   - pet stats, high scores...

// Recursive lock shared by main loop, web handlers and network task
extern SemaphoreHandle_t gLock;
#define LOCK()   xSemaphoreTakeRecursive(gLock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(gLock)

enum { DIRTY_CFG = 1, DIRTY_CARDS = 2, DIRTY_STATE = 4 };
void coreBegin();
void coreLoop();
void markDirty(uint8_t what);
void saveNow();
bool loadJson(const char* path, JsonDocument& d);
bool saveJson(const char* path, JsonDocument& d);
void createDefaultCards();

// ---------- Logging ----------
void logf(const char* fmt, ...);
String logDump();
uint32_t logSeq();

// ---------- Time helpers ----------
bool   timeOK();
bool   nowTm(struct tm& t);
int    nowMin();                                  // minutes since local midnight, -1 if unknown
int    parseHM(const char* s);                    // "07:30" -> 450, -1 bad
bool   inWindow(int m, int from, int to);         // handles windows over midnight
bool   dayAllowed(int mask, int wday);            // mask bit0=Mon..bit6=Sun, wday tm_wday
uint32_t dayKey(const struct tm& t);              // YYYYMMDD
time_t makeUTC(int y, int mo, int d, int h, int mi, int s);
time_t parseISO(const char* s);                   // ISO8601, no zone = UTC
void   localIn(const char* posixTz, time_t t, struct tm& out);
void   fmtHM(char* out, int h, int m, bool h24);
bool   cfgH24();
extern const char* const WDAY[7];
extern const char* const WDAY_LONG[7];
extern const char* const MONTH[12];
extern const char* const MONTH_LONG[12];

// ---------- Speech text helpers ----------
String numWords(long n);
String sayTime(int h, int m);
String ttsClean(const String& s);

// ---------- Events / notifications ----------
enum Ev : uint8_t { EV_UI = 0, EV_TOUCH, EV_EMO, EV_CHIME, EV_ALARM, EV_TIMER, EV_REMIND,
                    EV_SPORTS, EV_MSG, EV_GAME, EV_NOTIFY, EV_COUNT };
extern const char* const EV_KEYS[EV_COUNT];

enum { NK_NORMAL = 0, NK_ALARM = 1 };
struct Notif {
  char title[28];
  char text[160];
  int8_t emo;        // emotion to show, -1 none
  char snd[40];      // sound spec: "sfx:goal", "mel:2", "file:/snd/x.wav", "" none
  uint8_t ev;        // sound category
  uint8_t speak;     // speak the text
  uint16_t secs;     // display time
  uint8_t kind;      // NK_NORMAL / NK_ALARM (loops until dismissed)
  int16_t ref;       // alarm index etc.
  char anim[36];     // custom animation (EMOA) file in /anim/
};
void notify(const char* title, const char* text, int emo = -1, const char* snd = "sfx:notify",
            uint8_t ev = EV_NOTIFY, bool speak = false, uint16_t secs = 8,
            uint8_t kind = NK_NORMAL, int16_t ref = -1, const char* anim = nullptr);
bool notifyPop(Notif& n);

// Remote commands coming from network task (Telegram / MQTT) -> main loop
enum { RC_MSG = 1, RC_SAY, RC_EMO, RC_PLAY, RC_STOP, RC_NOTIFY };
struct RCmd { uint8_t type; char from[24]; char arg[200]; };
void rcPush(uint8_t type, const char* from, const char* arg);
bool rcPop(RCmd& c);

// ---------- Shared live data ----------
struct PcStats {
  uint32_t ts; float cpu, ram, gpu, cpuT, gpuT, up, down;
  char song[64]; char artist[40]; bool playing;
};
extern PcStats pcStats;

struct MsgData { char from[24]; char text[200]; time_t t; uint32_t count; };
extern MsgData lastMsg;

extern volatile bool otaBusy;
extern volatile bool gNetBusy;      // an HTTP(S) request is running (Bluetooth scan waits)

// ---------- Memory / health ----------
size_t heapLargest();                                  // largest free block (8-bit capable)
bool   heapOk(size_t needFree, size_t needBlock);
extern UBaseType_t gNetStackFree, gAudioStackFree;     // stack high-water marks (bytes)

// ---------- Crash safety ----------
extern bool gSafeMode;              // set after 3 crashes in a row: optional features stay off
const char* resetReasonStr();
int  bootCrashCount();
bool lastBootCrashed();
void wdtEnable(bool on);            // main-loop watchdog (auto-reboot on a freeze)
void wdtFeed();                     // call inside long jobs in the main loop
void safeCopy(char* dst, const char* src, size_t n);
bool icontains(const char* hay, const char* needle);
bool isQualifierMismatch(const char* a, const char* b);
bool teamMatch(const char* target, const char* teamFilter);
bool matchTeamItem(const char* longName, const char* shortCode, const char* teamFilter);
String fmtNum(double v);       // 1234567 -> 1.23M
String fmtPrice(double v);     // sensible decimals
int batteryPercent();               // -1 when no battery is wired
int batteryRaw();
int batteryMv();
