#pragma once
#include <U8g2lib.h>
#include "core.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern int8_t burnX, burnY;        // burn-in protection offset for static cards

#define F_TINY   u8g2_font_4x6_tf
#define F_SMALL  u8g2_font_5x7_tf
#define F_MED    u8g2_font_6x10_tf
#define F_BOLD   u8g2_font_7x13B_tf
#define F_HELV   u8g2_font_helvB10_tf
#define F_HELV14 u8g2_font_helvB14_tf
#define F_NUM16  u8g2_font_logisoso16_tr
#define F_NUM24  u8g2_font_logisoso24_tn
#define F_NUM32  u8g2_font_logisoso32_tn
#define F_NUM42  u8g2_font_logisoso42_tn

void displayBegin();
void displayApply();               // flip / invert / contrast from cfg
void displaySend();
void displayPower(bool on);
bool displayIsOn();
void displaySetContrast(uint8_t c);

void textCenter(int y, const char* s);
void textRight(int xr, int y, const char* s);
int  wrapText(int x, int y, int w, int lineH, const char* s, int maxLines, int skip = 0);
void marquee(int x, int y, int w, const char* s, uint32_t t0, int pxPerSec = 30);
void drawBar(int x, int y, int w, int h, float frac);
void drawDottedHLine(int x, int y, int w);
void drawBadge(int xr, int y, const char* text);
void drawSignalBars(int x, int y, int rssi);
void drawHeader(const char* title, const char* right = nullptr);
void drawSpark(int x, int y, int w, int h, const float* v, int n);
void drawWeatherIcon(int x, int y, int s, int code, bool day);
void drawCenteredMsg(const char* l1, const char* l2 = nullptr);
void drawQRCode(int x, int y, const char* text, uint8_t scale = 2);
void drawIconBall(int cx, int cy, int r, uint8_t fg = 1);
void drawIconBell(int x, int y);
void drawIconMail(int x, int y);
void drawIconBat(int x, int y, int pct);
void drawPageDots(int xr, int y, int n, int cur);

const char* wmoText(int code);
int  wmoClass(int code);           // 0 clear 1 partly 2 cloudy 3 fog 4 rain 5 snow 6 thunder
