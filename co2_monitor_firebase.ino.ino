#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <time.h>

// =====================================================
// HONEYWELL CO2 MONITOR
// PIC -> ESP32 -> DS3231 + AT24C32 + Firebase *****
// =====================================================

//const char* WIFI_SSID     = "Hmm";
//const char* WIFI_PASSWORD = "123456789";

const char* WIFI_SSID     = "AirFiber-Wn4eU7_EXT";
const char* WIFI_PASSWORD = "shah3queiYimokae";
const char* FIREBASE_URL  = "https://co2-monitor-49cbd-default-rtdb.firebaseio.com/";

// I2C
#define DS3231_ADDR 0x68
#define EEPROM_ADDR 0x50
#define SDA_PIN 21       // ESP32 GPIO21 / physical pin 33
#define SCL_PIN 22       // ESP32 GPIO22 / physical pin 36

// Buttons
#define START_PIN 25
#define STOP_PIN  26

// EEPROM
#define EEPROM_SIZE 4096
#define META_MAGIC_ADDR       0
#define META_HEAD_ADDR        2
#define META_COUNT_ADDR       4
#define META_RECORDING_ADDR   6
#define META_SESSION_ADDR     8
#define META_RECNO_ADDR       10
#define META_LAST_TS_ADDR     12
#define META_ELAPSED_ADDR     16
#define META_START_TS_ADDR    20
#define META_STOP_TS_ADDR     24
#define META_MIN_ADDR         28
#define META_MAX_ADDR         30
#define META_SUM_ADDR         32
#define META_STATCOUNT_ADDR   40
#define META_NEXT_SESSION_ADDR 42
#define META_LAST_SAVE_ADDR   44
#define META_TESTNAME_ADDR    48 // 32 bytes allocated for test name

#define DATA_START 96 // Shifted from 64 to 96 to fit the test name

// timestamp(4) + ppm(2) + sessionId(2) + recordNo(2) + uploaded(1)
#define RECORD_SIZE 11
#define MAX_RECORDS ((EEPROM_SIZE - DATA_START) / RECORD_SIZE)

//#define RECORD_INTERVAL_MS 30000UL       // 30 seconds
#define RECORD_INTERVAL_MS 180000UL
#define POWER_STATE_SAVE_MS 5000UL       // save timer state every 5 sec
#define WIFI_CHECK_MS 10000UL

struct CO2Record {
  uint32_t timestamp;
  uint16_t ppm;
  uint16_t sessionId;
  uint16_t recordNo;
  uint8_t uploaded;
};

uint16_t recordHead = 0;
uint16_t recordCount = 0;

bool recording = false;
uint16_t sessionId = 0;
uint16_t nextRecordNo = 1;
char testName[32] = ""; // Global variable for test name

uint32_t sessionStartTs = 0;
uint32_t sessionStopTs = 0;
uint32_t lastRecordTs = 0;
uint32_t elapsedSinceRecordSec = 0;

uint16_t sessionMin = 65535;
uint16_t sessionMax = 0;
uint64_t sessionSum = 0;
uint32_t sessionStatCount = 0;

uint16_t liveCO2 = 0;

unsigned long lastRecordMillis = 0;
unsigned long lastStateSaveMillis = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastNTPSync = 0;
unsigned long lastLivePushMillis = 0;
unsigned long lastCommandCheckMillis = 0;
unsigned long lastRecordUploadMillis = 0;

bool lastStartState = HIGH;
bool lastStopState = HIGH;
bool wifiWasDown = false;
int firebaseTaskIndex = 0;

WiFiClientSecure firebaseClient;

// =====================================================
// EEPROM LOW LEVEL
// =====================================================

void eepromWriteByte(uint16_t address, uint8_t value) {
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write((address >> 8) & 0xFF);
  Wire.write(address & 0xFF);
  Wire.write(value);
  Wire.endTransmission();
  delay(5);
}

uint8_t eepromReadByte(uint16_t address) {
  Wire.beginTransmission(EEPROM_ADDR);
  Wire.write((address >> 8) & 0xFF);
  Wire.write(address & 0xFF);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  Wire.requestFrom(EEPROM_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

void eepromWriteBytes(uint16_t address, const uint8_t* data, uint16_t length) {
  for (uint16_t i = 0; i < length; i++) eepromWriteByte(address + i, data[i]);
}

void eepromReadBytes(uint16_t address, uint8_t* data, uint16_t length) {
  for (uint16_t i = 0; i < length; i++) data[i] = eepromReadByte(address + i);
}

void writeU16(uint16_t addr, uint16_t v) {
  eepromWriteByte(addr, v & 0xFF);
  eepromWriteByte(addr + 1, v >> 8);
}
uint16_t readU16(uint16_t addr) {
  return (uint16_t)eepromReadByte(addr) | ((uint16_t)eepromReadByte(addr + 1) << 8);
}
void writeU32(uint16_t addr, uint32_t v) {
  for (int i=0;i<4;i++) eepromWriteByte(addr+i, (v >> (8*i)) & 0xFF);
}
uint32_t readU32(uint16_t addr) {
  uint32_t v=0;
  for (int i=0;i<4;i++) v |= ((uint32_t)eepromReadByte(addr+i) << (8*i));
  return v;
}
void writeU64(uint16_t addr, uint64_t v) {
  for (int i=0;i<8;i++) eepromWriteByte(addr+i, (v >> (8*i)) & 0xFF);
}
uint64_t readU64(uint16_t addr) {
  uint64_t v=0;
  for (int i=0;i<8;i++) v |= ((uint64_t)eepromReadByte(addr+i) << (8*i));
  return v;   
}

// =====================================================
// METADATA / POWER-FAILURE STATE
// =====================================================

void saveMetadata() {
  eepromWriteByte(META_MAGIC_ADDR, 0x48);
  eepromWriteByte(META_MAGIC_ADDR+1, 0x4E);
  writeU16(META_HEAD_ADDR, recordHead);
  writeU16(META_COUNT_ADDR, recordCount);
  eepromWriteByte(META_RECORDING_ADDR, recording ? 1 : 0);
  writeU16(META_SESSION_ADDR, sessionId);
  writeU16(META_RECNO_ADDR, nextRecordNo);
  writeU32(META_LAST_TS_ADDR, lastRecordTs);
  writeU32(META_ELAPSED_ADDR, elapsedSinceRecordSec);
  writeU32(META_START_TS_ADDR, sessionStartTs);
  writeU32(META_STOP_TS_ADDR, sessionStopTs);
  writeU16(META_MIN_ADDR, sessionMin);
  writeU16(META_MAX_ADDR, sessionMax);
  writeU64(META_SUM_ADDR, sessionSum);
  writeU32(META_STATCOUNT_ADDR, sessionStatCount);
  writeU16(META_NEXT_SESSION_ADDR, sessionId + 1);
  writeU32(META_LAST_SAVE_ADDR, millis() / 1000UL);

  // Save Test Name
  for (int i = 0; i < 32; i++) {
    eepromWriteByte(META_TESTNAME_ADDR + i, testName[i]);
  }
}

void loadMetadata() {
  uint8_t a=eepromReadByte(META_MAGIC_ADDR), b=eepromReadByte(META_MAGIC_ADDR+1);
  if (a != 0x48 || b != 0x4E) {
    recordHead=0; recordCount=0; recording=false; sessionId=0; nextRecordNo=1;
    sessionStartTs=sessionStopTs=lastRecordTs=0; elapsedSinceRecordSec=0;
    sessionMin=65535; sessionMax=0; sessionSum=0; sessionStatCount=0;
    saveMetadata();
    Serial.println("[EEPROM] First initialization");
    return;
  }

  recordHead=readU16(META_HEAD_ADDR);
  recordCount=readU16(META_COUNT_ADDR);
  recording=eepromReadByte(META_RECORDING_ADDR)==1;
  sessionId=readU16(META_SESSION_ADDR);
  nextRecordNo=readU16(META_RECNO_ADDR);
  lastRecordTs=readU32(META_LAST_TS_ADDR);
  elapsedSinceRecordSec=readU32(META_ELAPSED_ADDR);
  sessionStartTs=readU32(META_START_TS_ADDR);
  sessionStopTs=readU32(META_STOP_TS_ADDR);
  sessionMin=readU16(META_MIN_ADDR);
  sessionMax=readU16(META_MAX_ADDR);
  sessionSum=readU64(META_SUM_ADDR);
  sessionStatCount=readU32(META_STATCOUNT_ADDR);

  // Load Test Name
  for (int i = 0; i < 32; i++) {
    testName[i] = (char)eepromReadByte(META_TESTNAME_ADDR + i);
  }

  if (recordHead >= MAX_RECORDS) recordHead=0;
  if (recordCount > MAX_RECORDS) recordCount=0;
  if (nextRecordNo == 0) nextRecordNo=1;

  Serial.println("[EEPROM] Metadata restored");
  Serial.printf("Head=%u Count=%u Session=%u NextRecord=%u Recording=%s\n",
                recordHead, recordCount, sessionId, nextRecordNo, recording?"YES":"NO");
}

// =====================================================
// RECORD READ / WRITE
// =====================================================

uint16_t recordAddress(uint16_t index) {
  return DATA_START + index * RECORD_SIZE;
}

void writeRecord(uint16_t index, const CO2Record &r) {
  uint8_t b[RECORD_SIZE];
  b[0]=r.timestamp; b[1]=r.timestamp>>8; b[2]=r.timestamp>>16; b[3]=r.timestamp>>24;
  b[4]=r.ppm; b[5]=r.ppm>>8;
  b[6]=r.sessionId; b[7]=r.sessionId>>8;
  b[8]=r.recordNo; b[9]=r.recordNo>>8;
  b[10]=r.uploaded;
  eepromWriteBytes(recordAddress(index), b, RECORD_SIZE);
}

CO2Record readRecord(uint16_t index) {
  CO2Record r; uint8_t b[RECORD_SIZE];
  eepromReadBytes(recordAddress(index), b, RECORD_SIZE);
  r.timestamp=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
  r.ppm=(uint16_t)b[4]|((uint16_t)b[5]<<8);
  r.sessionId=(uint16_t)b[6]|((uint16_t)b[7]<<8);
  r.recordNo=(uint16_t)b[8]|((uint16_t)b[9]<<8);
  r.uploaded=b[10];
  return r;
}

void markUploaded(uint16_t index) {
  eepromWriteByte(recordAddress(index)+10, 1);
}

int findOldestPending() {
  for (uint16_t i=0;i<recordCount;i++) {
    uint16_t idx=(recordHead+i)%MAX_RECORDS;
    CO2Record r=readRecord(idx);
    if (r.uploaded==0) return idx;
  }
  return -1;
}

// =====================================================
// TIME
// =====================================================

uint8_t decToBcd(uint8_t v) { return ((v/10)<<4)|(v%10); }
uint8_t bcdToDec(uint8_t v) { return ((v>>4)*10)+(v&0x0F); }

bool readRTC(struct tm &t) {
  Wire.beginTransmission(DS3231_ADDR); Wire.write(0x00);
  if (Wire.endTransmission(false)!=0) return false;
  if (Wire.requestFrom(DS3231_ADDR,7)<7) return false;
  t.tm_sec=bcdToDec(Wire.read()&0x7F);
  t.tm_min=bcdToDec(Wire.read()&0x7F);
  t.tm_hour=bcdToDec(Wire.read()&0x3F);
  uint8_t wd=bcdToDec(Wire.read()&0x07);
  t.tm_wday=wd%7;
  t.tm_mday=bcdToDec(Wire.read()&0x3F);
  t.tm_mon=bcdToDec(Wire.read()&0x1F)-1;
  t.tm_year=bcdToDec(Wire.read())+100;
  return true;
}

// Days from civil date, UTC calendar calculation.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era=(y>=0 ? y : y-399)/400;
  const unsigned yoe=(unsigned)(y-era*400);
  const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
  const unsigned doe=yoe*365+yoe/4-yoe/100+doy;
  return (int64_t)era*146097+(int64_t)doe-719468;
}

uint32_t localIndiaToEpoch(const struct tm &t) {
  // RTC/NTP display is India time (UTC+5:30), so subtract 19800 seconds.
  int y=t.tm_year+1900, m=t.tm_mon+1, d=t.tm_mday;
  int64_t days=daysFromCivil(y,m,d);
  int64_t epoch=days*86400LL+t.tm_hour*3600+t.tm_min*60+t.tm_sec-19800LL;
  return epoch<0 ? 0 : (uint32_t)epoch;
}

String dateFromTs(uint32_t ts) {
  time_t x=(time_t)ts+19800;
  struct tm *p=gmtime(&x);
  char s[16]; sprintf(s,"%02d/%02d/%04d",p->tm_mday,p->tm_mon+1,p->tm_year+1900);
  return String(s);
}
String timeFromTs(uint32_t ts) {
  time_t x=(time_t)ts+19800;
  struct tm *p=gmtime(&x);
  char s[12]; sprintf(s,"%02d:%02d:%02d",p->tm_hour,p->tm_min,p->tm_sec);
  return String(s);
}

bool rtcValid() {
  struct tm t;
  if(!readRTC(t)) return false;
  int y=t.tm_year+1900;
  return y>=2025 && t.tm_mon>=0 && t.tm_mon<12 && t.tm_mday>=1 && t.tm_mday<=31;
}

void syncNTP() {
  if(WiFi.status()!=WL_CONNECTED) return;
  Serial.println("[NTP] Synchronizing...");
  configTime(19800,0,"pool.ntp.org","time.nist.gov","time.google.com");
  struct tm t;
  if(!getLocalTime(&t,15000)) { Serial.println("[NTP] FAILED"); return; }

  Wire.beginTransmission(DS3231_ADDR); Wire.write(0x00);
  Wire.write(decToBcd(t.tm_sec)); Wire.write(decToBcd(t.tm_min)); Wire.write(decToBcd(t.tm_hour));
  Wire.write(decToBcd(t.tm_wday==0?7:t.tm_wday)); Wire.write(decToBcd(t.tm_mday));
  Wire.write(decToBcd(t.tm_mon+1)); Wire.write(decToBcd((t.tm_year+1900)-2000));
  Wire.endTransmission();
  Serial.println("[NTP] DS3231 updated");
}

// =====================================================
// PIC CO2
// =====================================================

void readPIC() {
  while(Serial.available()) {
    String line=Serial.readStringUntil('\n'); line.trim();
    if(!line.length()) continue;
    Serial.print("[PIC] "); Serial.println(line);

    String num="";
    for(int i=0;i<line.length();i++) {
      char c=line[i];
      if(isDigit(c)) num+=c;
      else if(num.length()) break;
    }
    if(num.length()) {
      long v=num.toInt();
      if(v>=0 && v<=65535) {
        liveCO2=(uint16_t)v;
        Serial.printf("[CO2] LIVE = %u ppm\n",liveCO2);
      }
    }
  }
}

// =====================================================
// SESSION + RECORDING
// =====================================================

void pushSessionSummary();

void startRecording() {
  if(recording) return;

  struct tm t;
  if(!readRTC(t) || !rtcValid()) {
    Serial.println("[RTC FAULT] Cannot start a new session without valid RTC");
    return;
  }

  sessionId++;
  if(sessionId==0) sessionId=1;
  nextRecordNo=1;
  sessionStartTs=localIndiaToEpoch(t);
  sessionStopTs=0;
  sessionMin=65535;
  sessionMax=0;
  sessionSum=0;
  sessionStatCount=0;
  elapsedSinceRecordSec=0;
  lastRecordTs=sessionStartTs;

  recording=true;
  lastRecordMillis=millis();
  lastStateSaveMillis=millis();
  saveMetadata();

  Serial.printf("[SESSION %u] STARTED at %s %s\n",sessionId,dateFromTs(sessionStartTs).c_str(),timeFromTs(sessionStartTs).c_str());
}

void stopRecording() {
  if(!recording) return;

  struct tm t;
  if(readRTC(t)) sessionStopTs=localIndiaToEpoch(t);
  else sessionStopTs=lastRecordTs;

  recording=false;
  elapsedSinceRecordSec=(millis()-lastRecordMillis)/1000UL;
  saveMetadata();
  if (WiFi.status() == WL_CONNECTED) pushSessionSummary();

  float avg=sessionStatCount ? (float)sessionSum/sessionStatCount : 0;
  Serial.printf("[SESSION %u] STOPPED. Duration=%lu sec, Records=%lu, Min=%u, Max=%u, Avg=%.1f\n",
                sessionId,(unsigned long)(sessionStopTs-sessionStartTs), (unsigned long)sessionStatCount,
                sessionStatCount?sessionMin:0, sessionStatCount?sessionMax:0,avg);
}

void saveCO2Record(uint16_t ppm) {
  struct tm t;
  if(!readRTC(t) || !rtcValid()) {
    Serial.println("[RTC FAULT] Using last valid timestamp");
    if(lastRecordTs==0) return;
  } else {
    lastRecordTs=localIndiaToEpoch(t);
  }

  CO2Record r;
  r.timestamp=lastRecordTs;
  r.ppm=ppm;
  r.sessionId=sessionId;
  r.recordNo=nextRecordNo++;
  if(nextRecordNo==0) nextRecordNo=1;
  r.uploaded=0;

  uint16_t idx;
  if(recordCount<MAX_RECORDS) {
    idx=(recordHead+recordCount)%MAX_RECORDS;
    recordCount++;
  } else {
    idx=recordHead;
    recordHead=(recordHead+1)%MAX_RECORDS;
    Serial.println("[EEPROM] FULL -> overwriting oldest record");
  }

  writeRecord(idx,r);

  sessionMin=min(sessionMin,ppm);
  sessionMax=max(sessionMax,ppm);
  sessionSum+=ppm;
  sessionStatCount++;

  saveMetadata();

  Serial.printf("[RECORD] Session=%u No=%u CO2=%u ppm EEPROM index=%u Total=%u\n",
                r.sessionId,r.recordNo,r.ppm,idx,recordCount);
}

void handleRecording() {
  if(!recording) return;

  unsigned long now=millis();
  if(now-lastStateSaveMillis>=POWER_STATE_SAVE_MS) {
    elapsedSinceRecordSec=(now-lastRecordMillis)/1000UL;
    saveMetadata();
    lastStateSaveMillis=now;
  }

  if(now-lastRecordMillis>=RECORD_INTERVAL_MS) {
    lastRecordMillis=now;
    elapsedSinceRecordSec=0;
    if(liveCO2>0) saveCO2Record(liveCO2);
    else Serial.println("[RECORD] No valid CO2 reading");
  }
}

// =====================================================
// BUTTONS
// =====================================================

void handleButtons() {
  bool s=digitalRead(START_PIN), p=digitalRead(STOP_PIN);
  if(lastStartState==HIGH && s==LOW) startRecording();
  if(lastStopState==HIGH && p==LOW) stopRecording();
  lastStartState=s; lastStopState=p;
}

// =====================================================
// WIFI
// =====================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  unsigned long st=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-st<20000) { delay(500); Serial.print("."); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) {
    Serial.print("[WIFI] Connected IP="); Serial.println(WiFi.localIP());
  } else Serial.println("[WIFI] Offline - local EEPROM logging continues");
}

void checkWiFi() {
  if(millis()-lastWiFiCheck<WIFI_CHECK_MS) return;
  lastWiFiCheck=millis();
  if(WiFi.status()!=WL_CONNECTED) {
    wifiWasDown=true;
    WiFi.disconnect(); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  } else if(wifiWasDown) {
    wifiWasDown=false;
    Serial.println("[WIFI] Restored");
    syncNTP(); lastNTPSync=millis();
  }
}

// =====================================================
// FIREBASE REST
// =====================================================

int firebasePut(const String &path,const String &body) {
  if(WiFi.status()!=WL_CONNECTED) return -1;
  HTTPClient http;
  String url=String(FIREBASE_URL)+path;
  if(!http.begin(firebaseClient,url)) return -2;
  http.addHeader("Content-Type","application/json");
  int code=http.PUT(body);
  http.end(); firebaseClient.stop();
  return code;
}

String firebaseGet(const String &path) {
  if(WiFi.status()!=WL_CONNECTED) return "";
  HTTPClient http; String url=String(FIREBASE_URL)+path;
  if(!http.begin(firebaseClient,url)) return "";
  int code=http.GET(); String out=(code==200)?http.getString():"";
  http.end(); firebaseClient.stop(); return out;
}

void pushLiveData() {
  struct tm t; String date="---", tm="---";
  if(readRTC(t)) {
    char d[16],x[12];
    sprintf(d,"%02d/%02d/%04d",t.tm_mday,t.tm_mon+1,t.tm_year+1900);
    sprintf(x,"%02d:%02d:%02d",t.tm_hour,t.tm_min,t.tm_sec);
    date=d; tm=x;
  }
  String json="{";
  json+="\"testName\":\""+String(testName)+"\",";
  json+="\"co2\":"+String(liveCO2)+",";
  json+="\"rssi\":"+String(WiFi.RSSI())+",";
  json+="\"date\":\""+date+"\",";
  json+="\"time\":\""+tm+"\",";
  json+="\"recording\":\""+String(recording?"YES":"NO")+"\",";
  json+="\"count\":"+String(recordCount)+",";
  json+="\"sessionId\":"+String(sessionId)+",";
  json+="\"nextRecordNo\":"+String(nextRecordNo)+",";
  json+="\"min\":"+String(sessionStatCount?sessionMin:0)+",";
  json+="\"max\":"+String(sessionStatCount?sessionMax:0)+",";
  json+="\"avg\":"+String(sessionStatCount?(float)sessionSum/sessionStatCount:0.0,1)+",";
  json+="\"sessionRecords\":"+String(sessionStatCount)+",";
  json+="\"sessionStart\":\""+(sessionStartTs?dateFromTs(sessionStartTs)+" "+timeFromTs(sessionStartTs):"---")+"\",";
  json+="\"sessionStop\":\""+(sessionStopTs?dateFromTs(sessionStopTs)+" "+timeFromTs(sessionStopTs):"---")+"\"";
  json+="}";
  firebasePut("live.json",json);
}

void uploadOldestPendingRecord() {
  lastRecordUploadMillis=millis();
  if(WiFi.status()!=WL_CONNECTED) return;
  int idx=findOldestPending(); if(idx<0) return;
  CO2Record r=readRecord(idx);

  String key=String(r.timestamp)+"_"+String(r.sessionId)+"_"+String(r.recordNo);
  String json="{";
  json+="\"timestamp\":"+String(r.timestamp)+",";
  json+="\"date\":\""+dateFromTs(r.timestamp)+"\",";
  json+="\"time\":\""+timeFromTs(r.timestamp)+"\",";
  json+="\"ppm\":"+String(r.ppm)+",";
  json+="\"sessionId\":"+String(r.sessionId)+",";
  json+="\"recordNumber\":"+String(r.recordNo);
  json+="}";

  int code=firebasePut("records/"+key+".json",json);
  if(code==200) {
    markUploaded(idx);
    Serial.printf("[FIREBASE] Record uploaded S%u R%u\n",r.sessionId,r.recordNo);
  } else Serial.printf("[FIREBASE] Record upload failed HTTP=%d\n",code);
}

void pushSessionSummary() {
  if(WiFi.status()!=WL_CONNECTED || sessionId==0) return;
  float avg=sessionStatCount?(float)sessionSum/sessionStatCount:0;
  String json="{";
  json+="\"sessionId\":"+String(sessionId)+",";
  json+="\"testName\":\""+String(testName)+"\",";
  json+="\"start\":\""+(sessionStartTs?dateFromTs(sessionStartTs)+" "+timeFromTs(sessionStartTs):"---")+"\",";
  json+="\"stop\":\""+(sessionStopTs?dateFromTs(sessionStopTs)+" "+timeFromTs(sessionStopTs):"---")+"\",";
  json+="\"durationSeconds\":"+String(sessionStopTs>sessionStartTs?sessionStopTs-sessionStartTs:0)+",";
  json+="\"totalRecords\":"+String(sessionStatCount)+",";
  json+="\"min\":"+String(sessionStatCount?sessionMin:0)+",";
  json+="\"max\":"+String(sessionStatCount?sessionMax:0)+",";
  json+="\"avg\":"+String(avg,1);
  json+="}";
  firebasePut("sessions/"+String(sessionId)+".json",json);
}
void checkRemoteCommand() {
  String r=firebaseGet("command.json"); r.trim(); 
  if(r=="\"\"" || r=="null") return;
  
  if (r.startsWith("\"")) r = r.substring(1, r.length() - 1); // Remove Firebase quotes

  if(r.startsWith("start:")) { 
    String tName = r.substring(6);
    tName.toCharArray(testName, 32);
    startRecording(); 
    firebasePut("command.json","\"\""); 
  }
  else if(r=="stop") { stopRecording(); pushSessionSummary(); firebasePut("command.json","\"\""); }
  else if(r=="clear") { 
    recordHead=0; recordCount=0; 
    saveMetadata();
    firebasePut("records.json","null");
    // Removed the line that deleted sessions.json so test history stays safe
    firebasePut("command.json","\"\"");
    Serial.println("[CLEAR] EEPROM record index cleared + Firebase records cleared");
  }
}
// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n================================");
  Serial.println(" HONEYWELL CO2 MONITOR - POWER ON");
  Serial.println("================================");

  Wire.begin(SDA_PIN,SCL_PIN);
  pinMode(START_PIN,INPUT_PULLUP);
  pinMode(STOP_PIN,INPUT_PULLUP);

  Wire.beginTransmission(DS3231_ADDR);
  Serial.println(Wire.endTransmission()==0?"[INIT] RTC found":"[INIT] RTC NOT FOUND");

  Wire.beginTransmission(EEPROM_ADDR);
  Serial.print("[INIT] EEPROM 0x50: ");
  Serial.println(Wire.endTransmission()==0?"FOUND":"NOT FOUND");
  Serial.printf("[INIT] MAX RECORDS = %u\n",MAX_RECORDS);

  loadMetadata();
  connectWiFi();
  firebaseClient.setInsecure();

  if(WiFi.status()==WL_CONNECTED) { syncNTP(); lastNTPSync=millis(); }

  // If recording was ON before power failure, resume.
  if(recording) {
    uint32_t elapsed=elapsedSinceRecordSec;
    if(elapsed>=30) elapsed=29; // next loop records after at most 1 sec
    lastRecordMillis=millis()-(unsigned long)elapsed*1000UL;
    Serial.printf("[POWER RESTORE] Session %u resumed. Timer elapsed=%lu sec\n",sessionId,(unsigned long)elapsed);
  }

  Serial.println("[INIT] Firebase web dashboard ready");
  Serial.println("SYSTEM READY");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  readPIC();
  handleButtons();
  checkWiFi();

  if(WiFi.status()==WL_CONNECTED && millis()-lastNTPSync>=21600000UL) {
    syncNTP(); lastNTPSync=millis();
  }

  handleRecording();

  // One Firebase operation per cycle.
  switch(firebaseTaskIndex) {
    case 0:
      if(millis()-lastCommandCheckMillis>=4000) {
        lastCommandCheckMillis=millis(); checkRemoteCommand();
      }
      break;
    case 1:
      if(millis()-lastLivePushMillis>=5000) {
        lastLivePushMillis=millis(); pushLiveData();
      }
      break;
    case 2:
      if(millis()-lastRecordUploadMillis>=3000) uploadOldestPendingRecord();
      break;
  }
  firebaseTaskIndex=(firebaseTaskIndex+1)%3;
  delay(10);
}
