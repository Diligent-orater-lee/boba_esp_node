#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "BobaConnectionManager.h"

BobaConnectionManager bobaManager;

#define DEVICE_NAME "Human Body Sensor"
#define DEVICE_DESCRIPTION "PIR sensor that detects human presence by detecting human body movements"
#define COMPONENT_TYPE "SENSOR_PIR_001"

const int inputPin = 0;
int LAST_NOTIFIED_STATE = LOW;

void setup() {
  Serial.begin(115200);
  pinMode(inputPin, INPUT);
  BobaConnectionManager::NodeMetaData nodeData = {
    DEVICE_NAME,
    DEVICE_DESCRIPTION,
    COMPONENT_TYPE
  };
  bobaManager.runDeviceUsing(executeDevice);
  bobaManager.begin(nodeData);
}

void loop() {
  bobaManager.update();
}

void executeDevice() {
  int inputState = digitalRead(inputPin);
  if (LAST_NOTIFIED_STATE != inputState) {
    BobaConnectionManager::KeyValuePair values[] = {
      { "sensorState", inputState == LOW ? "false" : "true" },
      { "sensorStateText", inputState == LOW ? "OFF" : "ON" },
      { "message", inputState == LOW ? "Device is switched OFF" : "Device is Switched ON" }
    };
    int dictSize = sizeof(values) / sizeof(values[0]);
    String endPoint = "/set-state";
    String jsonPayload = bobaManager.dictionaryToJson(values, dictSize);
    BobaConnectionManager::HTTP_Result result = bobaManager.makeHttpCall(endPoint, jsonPayload, true);
    if (result.httpResponseCode == 200) {
      LAST_NOTIFIED_STATE = inputState;
    }
  }
}
