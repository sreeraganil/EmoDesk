#pragma once
#include <Arduino.h>

void terminalBegin();
void terminalPoll();
String terminalExecute(const String& cmdLine, bool ansi = true);
