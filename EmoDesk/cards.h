#pragma once
#include "core.h"
#include "display.h"
#include "input.h"
#include "emo.h"
#include "audio.h"
#include "feeds.h"
#include "net.h"

// A "card" is one screen. Each card type is a table of callbacks; every card
// instance in /cards.json gets its own params (p), a small state array (st)
// and an optional data block (d) filled by fetch() on the network task.

enum { CF_RAW = 1, CF_NOROT = 2 };

struct CardCtx { JsonObject p; void* d; int idx; uint32_t* st; uint32_t t0; };

struct CardType {
  const char* id;
  uint16_t dsize;                           // bytes of POD data per instance
  uint16_t refresh;                         // default fetch interval (s), 0 = no fetch
  uint8_t flags;                            // CF_*
  void (*enter)(CardCtx&);                  // card became visible
  void (*draw)(CardCtx&);                   // draw one frame
  bool (*input)(CardCtx&, uint8_t g);       // return true if gesture consumed
  bool (*fetch)(JsonObject p, void* d);     // network task, p and d are private copies
  void (*speak)(CardCtx&, String& out);     // text for "speak this card"
  void (*tick)(CardCtx&);                   // runs every loop for every instance
};

struct CardInst { const CardType* t; void* d; uint32_t st[6]; uint32_t nextFetch; int ai; };

#define CARD_LIST(X) \
  X(emo) X(pet) X(clock) X(analog) X(binary) X(wordclock) X(calendar) X(world) X(progress) X(sunmoon) \
  X(countdown) X(dayssince) X(weather) X(forecast) X(aqi) X(cricket) X(cricfix) X(football) X(ftable) \
  X(crypto) X(stock) X(currency) X(news) X(quote) X(joke) X(fact) X(word) X(iss) X(youtube) X(github) \
  X(pomodoro) X(timer) X(stopwatch) X(alarms) X(reminders) X(todo) X(habits) X(water) X(note) X(calevents) \
  X(message) X(mqtt) X(pcstats) X(nowplaying) X(musicbox) X(soundboard) X(sleepsounds) X(metronome) \
  X(visualizer) X(images) X(anim) X(scroller) X(flappy) X(dino) X(stack) X(dodge) X(reaction) X(rhythm) \
  X(eightball) X(dice) X(decision) X(matrix) X(starfield) X(life) X(fireworks) X(bounce) X(netinfo) X(health) X(battery) X(ble) X(whatsapp)

#define CARD_DECL(n) extern const CardType CT_##n;
CARD_LIST(CARD_DECL)

extern bool cardHold;          // set inside draw() to pause auto-rotation
extern uint32_t gFetchNext;    // fetch() may set seconds until its next run

void cardsBegin();
void cardsRebuild();
void cardsLoop();
void cardsDraw();
bool cardsInput(uint8_t g);
void cardsNext(int dir);
void cardsGoto(int i);
void cardsHome();
void cardsSpeak();
String cardsCurrentText();
void cardsFetchLoop();
void cardRefreshNow(int idx);
int  cardsCount();
int  cardsCurrent();
const char* cardsCurrentType();
const char* cardsTypeAt(int i);
String cardsCurrentId();
bool cardsCurrentShowBat();
bool cardsHaveType(const char* type);
bool cardVisible(int i);
const CardType* cardTypeFind(const char* id);
CardCtx cardCtx(int i);
String cardName(int i, const char* def);
void cardTypesJson(String& out);

// helpers for card implementations
bool dataGate(bool ok, const char* what);     // draws "Loading/Offline" if !ok
void drawTitle(CardCtx& c, const char* def, const char* right = nullptr);
void highScore(const char* game, uint32_t score, uint32_t& best);
uint32_t getHighScore(const char* game);
bool locFromParams(JsonObject p, float& lat, float& lon, bool& custom);
