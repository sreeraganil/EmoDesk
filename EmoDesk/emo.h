#pragma once
#include "core.h"

enum Emotion : int8_t {
  E_NEUTRAL = 0, E_HAPPY, E_SAD, E_ANGRY, E_SLEEPY, E_SURPRISED, E_LOVE, E_BORED,
  E_DIZZY, E_CONFUSED, E_EXCITED, E_SUSPICIOUS, E_CRY, E_SLEEP, E_COOL, E_FOCUSED, E_COUNT
};
extern const char* const EMO_NAMES[E_COUNT];

// Eye styles (cfg emo.style)
enum { ES_ROUND = 0, ES_PIXEL, ES_SCANNER, ES_CAT };

struct PetStats { float happy, energy, hunger; uint32_t interactions; };
extern PetStats pet;

bool emoActive();
void emoBegin();
void emoApply();                                // re-read looks + custom expressions from cfg
void emoUpdate();
void emoRender(int cx, int cy, float scale);   // face centered at cx,cy
bool drawAnimFrame(const char* name);          // render a full-screen custom animation frame from /anim/
void emoSet(int e, uint32_t ms);                // temporary expression (wins over everything)
void emoHold(int e, uint32_t ms);               // background mood (e.g. focus timer); pokes still show
int  emoCurrent();
int  emoFromName(const char* n);
void emoPoke(int n = 1);
void emoPetting();                              // call every frame while held
void emoFeed();
void emoPlay();
void emoTouched();                              // any interaction (wakes up)
bool emoSleeping();
void emoSleepNow();
String emoSpeech();
void emoSaveState();
void emoResetStats();
