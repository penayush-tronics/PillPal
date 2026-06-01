#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "time.h"
#include "RTClib.h"

// --- Cloud Data Links Configuration ---
const char* jsonBinUrl = "https://getpantry.cloud/apiv1/pantry/0f98a64f-4ca2-4963-a375-aee72cc5f57e/basket/PillPal";
//const char* masterKey  = "$2a$10$nztv8uuzh/JTdWUuIQ1xLONyt9/j4f6SicwFS9eZywC7HPQfIslXS"; 

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
const int BUZZER_PIN  = 4;   
const int SPEAKER_PIN = 14;  
const int IR_USER_PIN = 16;  

const int SERVO_PINS[3] = {27, 26, 25}; 
const int CAP_PINS[3]   = {17, 18, 19};

const char* SERVICE_UUID        = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
const char* CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RTC_DS3231 rtc;
Servo servos[3];
Preferences prefs;
BLECharacteristic *pCharacteristic;

struct Medication {
  String name;
  int hour;
  int minute;
  bool active;
  bool firedToday; // Now saved to flash memory to survive power cycles
  int missed;
  int low_cap; 
};
Medication schedule[3];

int activeAlarmChamber = -1; 
int lastDisplayedSecond = -1;
unsigned long lastServerSyncMillis = 0;
const unsigned long syncInterval = 5000; 

unsigned long alarmStartTime = 0;
const unsigned long TIMEOUT_INTERVAL = 30 * 60 * 1000UL; 

bool bleCredentialsReceived = false;
String receivedWifiData = "";

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String value = pCharacteristic->getValue(); 
      if (value.length() > 0) {
        receivedWifiData = "";
        for (int i = 0; i < value.length(); i++) {
          receivedWifiData += value[i];
        }
        bleCredentialsReceived = true;
      }
    }
};

void fetchScheduleFromServer();
void pushScheduleToServer(bool resetFlag);
void saveScheduleToFlash();
void loadLocalScheduleFromFlash();
void runBLEProvisioningMode();
int getNextScheduledMedicationIndex(int curHour, int curMin);

void setup() {
  Serial.begin(115200);
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(SPEAKER_PIN, LOW);
  pinMode(IR_USER_PIN, INPUT_PULLUP);

  for(int i=0; i<3; i++) {
    pinMode(CAP_PINS[i], INPUT_PULLUP); 
    servos[i].attach(SERVO_PINS[i]);
    servos[i].write(160); 
    delay(100);
    servos[i].detach(); 
  }

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  rtc.begin();

  prefs.begin("med-system", false);
  
  String storedSSID = prefs.getString("ssid", "");
  String storedPASS = prefs.getString("pass", "");

  loadLocalScheduleFromFlash();

  if(storedSSID == "" || storedSSID.length() < 2) {
    runBLEProvisioningMode();
    storedSSID = prefs.getString("ssid", "");
    storedPASS = prefs.getString("pass", "");
  }

  WiFi.begin(storedSSID.c_str(), storedPASS.c_str());
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 12) {
    delay(500);
    retries++;
  }

  if(WiFi.status() == WL_CONNECTED) {
    configTime(19800, 0, "pool.ntp.org"); 
    struct tm timeinfo;
    delay(500);
    if (getLocalTime(&timeinfo)) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    }
    fetchScheduleFromServer();
  }
  lastServerSyncMillis = millis();
}

void loop() {
  DateTime now = rtc.now();

  // --- POWER-CUT MIDNIGHT ROLLOVER PROTECTION ---
  // Tracks actual calendar dates instead of a single clock-second tick
  int lastResetDay = prefs.getInt("lastResetDay", 0);
  if (now.day() != lastResetDay && now.year() >= 2026) {
    for(int i = 0; i < 3; i++) {
      schedule[i].firedToday = false;
      schedule[i].missed = 0;
    }
    prefs.putInt("lastResetDay", now.day());
    saveScheduleToFlash();
    pushScheduleToServer(false);
  }

  if (activeAlarmChamber == -1 && WiFi.status() == WL_CONNECTED && (millis() - lastServerSyncMillis >= syncInterval)) {
    fetchScheduleFromServer();
    lastServerSyncMillis = millis();
  }

  if (activeAlarmChamber == -1) {
    if (now.second() != lastDisplayedSecond) {
      display.clearDisplay();
      
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("STANDBY");
      
      char timeBuf[] = "hh:mm:ss";
      display.setCursor(80, 0); 
      display.print(now.toString(timeBuf));
      
      display.drawFastHLine(0, 11, 128, SSD1306_WHITE);

      int nextIdx = getNextScheduledMedicationIndex(now.hour(), now.minute());
      display.setCursor(0, 18);
      if(nextIdx != -1) {
        display.printf("NEXT: %s", schedule[nextIdx].name.c_str());
        display.setCursor(0, 28);
        display.printf("TIME: %02d:%02d", schedule[nextIdx].hour, schedule[nextIdx].minute);
      } else {
        bool codeHasActiveProfiles = false;
        for(int i = 0; i < 3; i++) {
          if(schedule[i].active) codeHasActiveProfiles = true;
        }
        
        if(codeHasActiveProfiles) {
          display.println("ALL DONE FOR TODAY!");
          display.setCursor(0, 28);
          display.print("Pal is proud of you");
        } else {
          display.println("NEXT: No Active Meds");
        }
      }

      display.drawFastHLine(0, 42, 128, SSD1306_WHITE);
      
      display.setCursor(0, 48);
      display.print("CAP:");
      display.setCursor(32, 48);
      display.print(digitalRead(CAP_PINS[0]) == HIGH ? "[C1:EMPTY]" : "[C1:OK]");
      display.setCursor(82, 48);
      display.print(digitalRead(CAP_PINS[1]) == HIGH ? "![EMPTY]" : "[C2:OK]");
      display.setCursor(32, 56);
      display.print(digitalRead(CAP_PINS[2]) == HIGH ? "[C3:EMPTY]" : "[C3:OK]");

      display.display();
      lastDisplayedSecond = now.second();
    }

    // --- CATCH-UP TIME SLICE CHECKER ---
    // Instead of evaluating exact seconds, check if current time passed the slot
    int currentMinutes = (now.hour() * 60) + now.minute();
    
    for (int i = 0; i < 3; i++) {
      if (schedule[i].active && !schedule[i].firedToday) {
        int medMinutes = (schedule[i].hour * 60) + schedule[i].minute;
        
        if (currentMinutes >= medMinutes) {
          activeAlarmChamber = i; 
          alarmStartTime = millis(); 
          break; // Fire the highest priority catch-up dose immediately
        }
      }
    }
  } 
  else {
    unsigned long runningAlertDuration = millis() - alarmStartTime;

    if (runningAlertDuration >= TIMEOUT_INTERVAL) {
      schedule[activeAlarmChamber].missed = 1;      
      schedule[activeAlarmChamber].firedToday = true; 
      saveScheduleToFlash();
      pushScheduleToServer(false); 
      activeAlarmChamber = -1;
      lastDisplayedSecond = -1;
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(SPEAKER_PIN, LOW);
      return; 
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("TAKE MEDICATION:");
    
    display.setTextSize(2);
    display.setCursor(0, 16);
    display.println(schedule[activeAlarmChamber].name);
    
    display.setTextSize(1);
    display.drawFastHLine(0, 48, 128, SSD1306_WHITE);
    display.setCursor(0, 54);
    long remainingMins = 30 - (runningAlertDuration / 60000);
    display.printf("IR Sensor Active (%ldm left)", remainingMins);
    display.display();

    digitalWrite(SPEAKER_PIN, HIGH); 
    if (runningAlertDuration >= 5000) { 
      digitalWrite(BUZZER_PIN, HIGH); 
    }
    
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(SPEAKER_PIN, LOW);

    if (digitalRead(IR_USER_PIN) == LOW) {
      display.clearDisplay();
      display.setCursor(0, 25);
      display.setTextSize(2);
      display.println("DISPENSING");
      display.display();

      servos[activeAlarmChamber].attach(SERVO_PINS[activeAlarmChamber]);
      delay(50);
      servos[activeAlarmChamber].write(1);
      delay(600); 
      servos[activeAlarmChamber].write(160); 
      delay(400);
      servos[activeAlarmChamber].detach(); 

      schedule[activeAlarmChamber].firedToday = true; 
      schedule[activeAlarmChamber].missed = 0; 
      saveScheduleToFlash();
      pushScheduleToServer(false); 
      activeAlarmChamber = -1; 
      lastDisplayedSecond = -1; 
    }
    delay(400);
  }
}

void loadLocalScheduleFromFlash() {
  for(int i = 0; i < 3; i++) {
    String pKey = "m" + String(i);
    schedule[i].name       = prefs.getString((pKey + "_nm").c_str(), "Chamber " + String(i+1));
    schedule[i].hour       = prefs.getInt((pKey + "_hr").c_str(), 0);
    schedule[i].minute     = prefs.getInt((pKey + "_mn").c_str(), 0);
    schedule[i].active     = prefs.getBool((pKey + "_ac").c_str(), false);
    schedule[i].missed     = prefs.getInt((pKey + "_ms").c_str(), 0);
    schedule[i].firedToday = prefs.getBool((pKey + "_fd").c_str(), false); // Loaded securely from non-volatile memory
    schedule[i].low_cap    = 0;
  }
}

void saveScheduleToFlash() {
  for(int i = 0; i < 3; i++) {
    String pKey = "m" + String(i);
    prefs.putString((pKey + "_nm").c_str(), schedule[i].name);
    prefs.putInt((pKey + "_hr").c_str(), schedule[i].hour);
    prefs.putInt((pKey + "_mn").c_str(), schedule[i].minute);
    prefs.putBool((pKey + "_ac").c_str(), schedule[i].active);
    prefs.putInt((pKey + "_ms").c_str(), schedule[i].missed);
    prefs.putBool((pKey + "_fd").c_str(), schedule[i].firedToday); // Committed instantly on any event transition
  }
}

void fetchScheduleFromServer() {
  if(WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(jsonBinUrl);
  //http.addHeader("X-Master-Key", masterKey);
  //http.addHeader("X-Bin-Meta", "false");

  int httpResponseCode = http.GET();
  if (httpResponseCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(1536);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonArray array = doc["meds"].as<JsonArray>();

      for(JsonObject v : array) {
        if (v.containsKey("reset") && v["reset"].as<int>() == 1) {
          display.clearDisplay();
          display.setCursor(0, 15);
          display.setTextSize(2);
          display.println("FACTORY");
          display.println("RESETTING...");
          display.display();
          
          pushScheduleToServer(false); 
          delay(1000);
          prefs.clear(); 
          prefs.end();
          ESP.restart(); 
        }

        int ch = v["ch"].as<int>();
        if(ch >= 1 && ch <= 3) {
            int i = ch - 1; 
            schedule[i].name   = v["name"].as<String>();
            schedule[i].hour   = v["hr"].as<int>();
            schedule[i].minute = v["min"].as<int>();
            schedule[i].active = (v["act"].as<int>() >= 1 || v["act"].as<bool>() == true);
            schedule[i].missed = v["missed"].as<int>();
        }
      }
      saveScheduleToFlash();
    }
  }
  http.end();
}

void pushScheduleToServer(bool resetFlag) {
  if(WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(jsonBinUrl);
  //http.addHeader("X-Master-Key", masterKey);
  //http.addHeader("X-Bin-Meta", "false");
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(1536);
  JsonArray array = doc.createNestedArray("meds");
  
  for(int i = 0; i < 3; i++) {
    JsonObject obj = array.createNestedObject();
    obj["ch"]      = i + 1;
    obj["name"]    = schedule[i].name;
    obj["hr"]      = schedule[i].hour;
    obj["min"]     = schedule[i].minute;
    obj["act"]     = schedule[i].active ? 1 : 0;
    obj["missed"]  = schedule[i].missed;
    obj["low_cap"] = (digitalRead(CAP_PINS[i]) == HIGH) ? 1 : 0; 
    obj["taken"]   = schedule[i].firedToday ? 1 : 0; 
    obj["reset"]   = (i == 0 && resetFlag) ? 1 : 0;
  }

  String requestBody;
  serializeJson(doc, requestBody);
  int httpServerResponse = http.POST(requestBody); 
  http.end();
}

int getNextScheduledMedicationIndex(int curHour, int curMin) {
  int targetIdx = -1;
  int currentTotalMinutes = (curHour * 60) + curMin;
  int closestTimeDifference = 9999; 

  for(int i = 0; i < 3; i++) {
    if(!schedule[i].active) continue;

    int medTotalMinutes = (schedule[i].hour * 60) + schedule[i].minute;
    int diff = medTotalMinutes - currentTotalMinutes;
    
    if(schedule[i].firedToday || diff < 0) {
      diff += 1440; 
    }
    
    if(diff < closestTimeDifference) {
      closestTimeDifference = diff;
      targetIdx = i;
    }
  }
  return targetIdx;
}

void runBLEProvisioningMode() {
  BLEDevice::init("Pill Pal Companion");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  while(!bleCredentialsReceived) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.setTextSize(1);
    display.println("=== PILL PAL SETUP ===");
    display.println("\nOpen Control Portal");
    display.println("Awaiting BLE handshake...");
    display.display();
    delay(500);
  }

  int commaIndex = receivedWifiData.indexOf(',');
  if(commaIndex > 0) {
    String parsedSSID = receivedWifiData.substring(0, commaIndex);
    String parsedPASS = receivedWifiData.substring(commaIndex + 1);
    
    prefs.putString("ssid", parsedSSID);
    prefs.putString("pass", parsedPASS);
  }
  prefs.end();
  ESP.restart(); 
}