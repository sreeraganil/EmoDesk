#pragma once
#include "core.h"

// ---------- Weather (Open-Meteo, no key) ----------
struct WeatherData {
  bool ok; uint32_t ts;
  float temp, feels, hum, wind; int16_t code; bool isDay;
  int16_t dCode[3]; float dMax[3], dMin[3]; uint8_t dWday[3];
  float hTemp[12]; int16_t hCode[12]; uint8_t hHour[12]; uint8_t hn;
  char sunrise[6], sunset[6]; float uv;
};
struct AirData { bool ok; uint32_t ts; int aqi; float pm25, pm10, uv; };
extern WeatherData homeW;
bool fetchWeather(float lat, float lon, bool imperial, WeatherData& out);
bool fetchAir(float lat, float lon, AirData& out);

// ---------- Cricket (CricketData.org / cricapi, free key) ----------
struct CricInn { char team[8]; int16_t r; int8_t w; float o; };
struct CricMatch {
  char id[32];
  char name[64]; char status[80]; char type[6];
  char t1[8], t2[8]; char t1n[28], t2n[28];
  bool started, ended; uint8_t ni; CricInn inn[4]; time_t start;
  char venue[32]; char series[32]; int16_t target;
  bool t1Batting, t2Batting;
  char bat1[28]; char bat2[28]; char bowl[28]; float crr;
  char recent[32];
  float rrr;
  int16_t remRuns;
  float remOvers;
  char partner[16];
  uint8_t winProb;
};
struct CricData { bool ok; uint32_t ts; uint8_t n; CricMatch m[8]; int hits, limit; char err[48]; };
extern CricData cric;
bool cricLive(const CricMatch& m);
void cricScore(const CricMatch& m, int side, char* out, size_t n);   // side 0/1
void cleanShortName(const char* shortVal, const char* fullVal, char* out, size_t outLen);

// ---------- MQTT ----------
struct MqttVal { char topic[64]; char val[24]; };
extern MqttVal mqttVals[12];
extern uint8_t mqttValN;
bool mqttConnected();
void mqttPublish(const char* topic, const char* payload);
const char* mqttGet(const char* topic);
void mqttCardsChanged();

// ---------- misc ----------
void feedsLoop();                   // network task
void feedsRefresh();                // refresh weather & cricket soon
void cricRefresh();                 // refresh cricket only
void tgQueue(const char* text);     // queue a Telegram message to the paired chat
void tgSendNow(const char* text);   // queue and send immediately
