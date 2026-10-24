#pragma once
#include "core.h"

// Everything here is safe to call from any task: commands go through a queue
// to the audio task, which renders sound into the MAX98357 over I2S.

void audioBegin();
void audioApply();                                  // reload volume/voice from cfg
bool audioInstalled();
bool audioMuted();
void audioSetMuted(bool m);
void audioSetVolume(int v);                         // 0..100 (saved to cfg)
int  audioVolume();
bool audioAllowed(uint8_t ev);

void audioSfx(const char* name, uint8_t ev = EV_UI);
void audioMelody(int idx, uint8_t ev = EV_UI);
void audioRtttl(const char* rtttl, uint8_t ev = EV_UI);
void audioFile(const char* path, uint8_t ev = EV_UI);
void audioSay(const String& text, uint8_t ev = EV_UI);
void audioChirp(int emotion, uint8_t ev = EV_EMO);
void audioNoise(int type, uint32_t ms);             // 0 white 1 pink 2 brown 3 rain 4 ocean, ms 0 = endless
void audioMetronome(int bpm);                       // 0 = stop
void audioAlarm(const char* spec);                  // loops with volume ramp until audioStop()
void audioPlaySpec(const char* spec, uint8_t ev);   // "sfx:x" "mel:3" "file:/snd/a.wav" "say:text" "rtttl:..."
void audioStop();

bool audioBusy();
bool audioSpeaking();
bool audioContinuous();                             // noise / metronome running
uint32_t audioMetroBeatMs();                        // millis() of last metronome beat

int  melodyCount();
const char* melodyName(int i);
int  melodyFind(const char* name);
int  sfxCount();
const char* sfxName(int i);

extern int16_t vizBuf[128];
extern volatile uint32_t vizStamp;
