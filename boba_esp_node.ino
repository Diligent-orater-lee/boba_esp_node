#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

#define DEVICE_NAME "Human Body Sensor"
#define DEVICE_DESCRIPTION "PIR sensor that detects human presence by detecting human body movements"
#define COMPONENT_TYPE "SENSOR_PIR_001"

String getChipId();

const String chipID = getChipId();
const String hostSSID = "BOBA_NODE_" + chipID;
const String hostPassword = "admin123";

const IPAddress BROADCAST_IP(255, 255, 255, 255);
const int BROADCAST_PORT = 5000;
const int HTTP_PORT = 18018;

const int inputPin = 0;  // GPIO3 (RX) as input pin
int LAST_NOTIFIED_STATE = LOW;

WiFiUDP udp;

ESP8266WebServer server(80);
struct WiFiCredentials {
  String ssid;
  String password;
};
struct HTTP_Result {
  int httpResponseCode;
  String response;
};
typedef struct {
    const char* key;
    const char* value;
} KeyValuePair;

String masterIP = "";
bool masterConnected = false;
bool masterConnecting = false;
bool otaServiceLive = false;
const int MASTER_BROADCAST_WAIT_TIME = 100; // A single unit is 100 milliseconds.
int broadcastWaitCycle = 0;

const int MASTER_PRESENCE_TRUST_WAIT_TIME = 300;
const int MASTER_PRESENCE_RETRY_TIME = 10;
const int MASTER_PRESENCE_RETRY_WAIT_TIME = 100;
int masterPresentTrust = MASTER_PRESENCE_TRUST_WAIT_TIME;
int masterPresenceTrustBrokenTimes = 0;

int masterCheckWaitingTime = 0;
int MASTER_CHECK_MAX_WAIT_TIME;

void setup() {
  pinMode(inputPin, INPUT);

  if (!LittleFS.begin()) {
    return;
  }

  WiFiCredentials creds = readCredentials();
  startConnectWifi(creds);
  startAPMode();
  ArduinoOTA.onEnd([]() {
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  });
  ArduinoOTA.onError([](ota_error_t error) {
  });

  randomSeed(ESP.getCycleCount());
  MASTER_CHECK_MAX_WAIT_TIME = random(51) + 100;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    masterConnected = false;
    otaServiceLive = false;
  }

  if (masterConnected) {
    pingMasterIfYouDoubt();
    executeDevice();
  } else {
    if (WiFi.status() == WL_CONNECTED) {
      if (masterCheckWaitingTime == 0) {
        checkMasterConnection();
      } else {
        masterCheckWaitingTime--;
      }

      if (!otaServiceLive) {
        otaServiceLive = true;
        ArduinoOTA.begin();
      }
    }

    server.handleClient();
  }

  if (otaServiceLive) {
    ArduinoOTA.handle();
  }
  delay(100);
}

void executeDevice() {
  int inputState = digitalRead(inputPin);
  if (LAST_NOTIFIED_STATE != inputState) {
    KeyValuePair values[] = {
      { "sensorState", inputState == LOW ? "false" : "true" },
      { "sensorStateText", inputState == LOW ? "OFF" : "ON" },
      { "message", inputState == LOW ? "Device is switched OFF" : "Device is Switched ON" }
    };
    int dictSize = sizeof(values) / sizeof(values[0]);
    String url = "http://" + masterIP + ":" + String(HTTP_PORT) + "/set-state";
    String jsonPayload = dictionaryToJson(values, dictSize);
    HTTP_Result result = makeHttpCall(url, jsonPayload);
    if (result.httpResponseCode == 200) {
      LAST_NOTIFIED_STATE = inputState;
    }
  }
}

void pingMasterIfYouDoubt() {
  if (masterPresentTrust == 0) {
    String url = "http://" + masterIP + ":" + String(HTTP_PORT) + "/ping";
    char str[12];
    snprintf(str, sizeof(str), "%d", digitalRead(inputPin));
    KeyValuePair values[] = {
      { "reason", "Checking presence" }
    };
    int arrSize = sizeof(values) / sizeof(values[0]); // Yeah it's 1 so what?
    String jsonPayload = dictionaryToJson(values, arrSize);

    HTTP_Result result = makeHttpCall(url, jsonPayload);
    if (result.httpResponseCode == 200) {
      masterPresenceTrustBrokenTimes = 0;
      masterPresentTrust = MASTER_PRESENCE_TRUST_WAIT_TIME;
    } else {
      masterPresenceTrustBrokenTimes++;
      masterPresentTrust = MASTER_PRESENCE_RETRY_WAIT_TIME; // Don't jump to conclution that master is gone. Do some retries with a slight delay
    }

    if (masterPresenceTrustBrokenTimes == MASTER_PRESENCE_RETRY_TIME) {
      masterConnected = false; // Yes master is gone
    }
  } else {
    masterPresentTrust--; // Will act as a neat counter.
  }
}

HTTP_Result makeHttpCall(String url, String jsonPayload) {
  HTTP_Result result;
  WiFiClient client;
  HTTPClient http;

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    int httpResponseCode = http.POST(jsonPayload);
    result.httpResponseCode = httpResponseCode;
    result.response = http.getString();

    http.end();
    return result;
  } else {
    result.httpResponseCode = 0;
    result.response = "Unable to connect to client";

    return result;
  }
}

void checkMasterConnection() {
  if (masterIP != "") {
    if (masterNotified()) {
      masterConnectionSuccess();
      return;
    }
  }

  if (masterConnecting) {
    if (broadcastWaitCycle == MASTER_BROADCAST_WAIT_TIME) {
      broadcastToMaster();
    }

    String ip = getMasterReply();
    if (ip != "") {
      masterIP = ip;
      if (masterNotified()) {
        masterConnectionSuccess();
        return;
      }
    }
  } else {
    broadcastToMaster();
    masterConnecting = true;
  }
}

void masterConnectionSuccess() {
  masterConnecting = false;
  masterConnected = true;
  masterPresenceTrustBrokenTimes = 0;
  masterPresentTrust = MASTER_PRESENCE_TRUST_WAIT_TIME;
}

void broadcastToMaster() {
  udp.begin(BROADCAST_PORT);
  broadcastWaitCycle = 0;
  udp.beginPacket(BROADCAST_IP, BROADCAST_PORT);
  String message = "DISCOVER_ME:" + chipID;
  udp.write(message.c_str());
  udp.endPacket();
}

String getMasterReply() {
  broadcastWaitCycle++;
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, 255);
    if (len > 0) {
      incomingPacket[len] = 0;
    }
    String response = String(incomingPacket);
    if (response.startsWith("FOUND:")) {
      return udp.remoteIP().toString();
    }
  }
  return "";
}

bool masterNotified() {
  String url = "http://" + masterIP + ":" + String(HTTP_PORT) + "/register";
  char ip[16];
  getIPAddress(ip, sizeof(ip));
  KeyValuePair values[] = {
    { "id", chipID.c_str() },
    { "ip", ip },
    {"name", DEVICE_NAME},
    { "description", DEVICE_DESCRIPTION },
    {"componentType", COMPONENT_TYPE}
  };
  int arrSize = sizeof(values) / sizeof(values[0]);
  String jsonPayload = dictionaryToJson(values, arrSize);
  HTTP_Result result = makeHttpCall(url, jsonPayload);
  if (result.httpResponseCode == 200) {
    return true;
  } else {
    masterCheckWaitingTime = MASTER_CHECK_MAX_WAIT_TIME;
    return false;
  }
}

void startConnectWifi(const WiFiCredentials& creds) {
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(creds.ssid.c_str(), creds.password.c_str());
}

void startAPMode() {
  WiFi.softAP(hostSSID, hostPassword);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", "<form action=\"/update\" method=\"post\">SSID:<input type=\"text\" name=\"ssid\"><br>Password:<input type=\"password\" name=\"password\"><br><input type=\"submit\" value=\"Save\"></form>");
  });

  server.on("/update", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    if (saveCredentials(ssid, password)) {
      server.send(200, "text/html", "<h1>Updated. Rebooting...</h1>");
      delay(2000);
      ESP.restart();
    } else {
      server.send(500, "text/html", "<h1>Error saving credentials.</h1>");
    }
  });

  server.begin();
}

bool saveCredentials(const String& ssid, const String& password) {
  File file = LittleFS.open("/network.txt", "w");
  if (!file) {
    return false;
  }

  file.println(ssid);
  file.println(password);
  file.close();
  return true;
}

WiFiCredentials readCredentials() {
  WiFiCredentials creds;
  File file = LittleFS.open("/network.txt", "r");
  if (!file) {
    return creds; // Return empty credentials if file does not exist
  }

  creds.ssid = file.readStringUntil('\n');
  creds.password = file.readStringUntil('\n');
  file.close();

  creds.ssid.trim();       // Remove any newline or whitespace
  creds.password.trim();   // Remove any newline or whitespace

  return creds;
}

String getChipId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  String macStr = "";
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) {
      macStr += "0";
    }
    macStr += String(mac[i], HEX);
    if (i < 5) macStr += ":";
  }
  return "ESP-" + macStr;
}

String dictionaryToJson(KeyValuePair* dict, int size) {
    StaticJsonDocument<200> doc;

    for (int i = 0; i < size; i++) {
        doc[dict[i].key] = dict[i].value;
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

void getIPAddress(char* ip_address, size_t size) {
    IPAddress ip = WiFi.localIP();
    snprintf(ip_address, size, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}
