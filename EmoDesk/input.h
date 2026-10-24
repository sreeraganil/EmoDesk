#pragma once
#include "core.h"

enum Gesture : uint8_t { G_NONE = 0, G_PRESS, G_RELEASE, G_TAP, G_DOUBLE, G_TRIPLE, G_LONG, G_VLONG };
enum { SRC_TOUCH = 0, SRC_BOOT = 1, SRC_WEB = 2 };

void inputBegin();
void inputApply();                  // reload timings from cfg
void inputPoll();
bool inputGet(uint8_t& g, uint8_t& src);
void inputInject(uint8_t g, uint8_t src = SRC_WEB);
void inputSetRaw(bool raw);         // raw mode: PRESS/RELEASE immediately (games)
uint32_t inputHeldMs();             // how long the touch pad has been held (0 if not)
uint32_t inputLastMs();             // millis() of last activity
const char* gestureName(uint8_t g);
