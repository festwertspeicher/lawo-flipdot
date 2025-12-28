#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "secrets.h"

#define RX_PIN 16
#define TX_PIN 17

// Protokoll-Bytes
const uint8_t BYTESTART    = 0xFF;
const uint8_t BYTEPICTURE  = 0xA0;
const uint8_t BYTEBACKL    = 0xA1;
const uint8_t BYTEINVERT   = 0xA2;
const uint8_t BYTEACTIVE   = 0xA3;
const uint8_t BYTEFASTMODE = 0xA4;

const uint8_t BYTEON  = 0x01;
const uint8_t BYTEOFF = 0x00;

// aktuelle States
bool stateBacklight = false;
bool stateInvert    = false;
bool stateActive    = true;
bool stateQuick     = true;

long lastWifiCheck = 0;
const long WIFI_CHECK_INTERVAL = 10000; 

AsyncWebServer server(80);
AsyncWebSocket  ws("/ws");
HardwareSerial& matrixSerial = Serial1;

// WiFi Debug Hilfsfunktionen um heruaszufinden falls es Probleme gibt 
String getWiFiStatusDescription(wl_status_t status) {
  switch(status) {
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS (0) - WiFi im Leerlauf";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL (1) - SSID nicht verfügbar";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED (2) - Scan abgeschlossen";
    case WL_CONNECTED: return "WL_CONNECTED (3) - Verbunden";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED (4) - Verbindung fehlgeschlagen";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST (5) - Verbindung verloren";
    case WL_DISCONNECTED: return "WL_DISCONNECTED (6) - Getrennt";
    default: return String("Unbekannter Status: ") + String(status);
  }
}

// Log & Weiterleitung von Binary-Frames an die Matrix
void handleWebSocketMessage(void*, uint8_t *data, size_t len) {
  // Update state-flags bei Commands
  if(len >= 3 && data[0] == BYTESTART) {
    uint8_t action = data[1], param = data[2];
    switch(action) {
      case BYTEBACKL:    stateBacklight = (param == BYTEON);    break;
      case BYTEINVERT:   stateInvert    = (param == BYTEON);    break;
      case BYTEACTIVE:   stateActive    = (param == BYTEON);    break;
      case BYTEFASTMODE: stateQuick     = (param == BYTEON);    break;
    }
  }

  // 1) in Seriellen Monitor loggen
  Serial.print("WS→ESP32 (len="); Serial.print(len); Serial.print("): ");
  for(size_t i=0;i<len;i++){
    Serial.printf("0x%02X ", data[i]);
  }
  Serial.println();

  // 2) an die Matrix senden
  size_t written = matrixSerial.write(data, len);
  Serial.printf("ESP32→Matrix (written=%u)\n\n", written);
}

// Rückkanal: Serial1 → WebSocket
void forwardMatrixResponses(){
  while(matrixSerial.available()){
    uint8_t b = matrixSerial.read();
    ws.binaryAll(&b, 1);
    Serial.printf("Matrix→ESP32→WS: 0x%02X\n", b);
  }
}

// Event-Handler für WS
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if(type == WS_EVT_CONNECT){
    Serial.printf("WS Client #%u connected\n", client->id());
  }
  else if(type == WS_EVT_DISCONNECT){
    Serial.printf("WS Client #%u disconnected\n", client->id());
  }
  else if(type == WS_EVT_DATA){
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if(info->opcode == WS_BINARY){
      // Binary-Command
      handleWebSocketMessage(nullptr, data, len);
    }
    else if(info->opcode == WS_TEXT){
      // Text-Frame (z.B. "getState")
      String msg = String((char*)data).substring(0, len);
      if(msg == "getState"){
        // JSON zusammenbauen
        String js = "{\"backlight\":" + String(stateBacklight?1:0)
                  + ",\"invert\":"  + String(stateInvert   ?1:0)
                  + ",\"active\":"  + String(stateActive   ?1:0)
                  + ",\"quick\":"   + String(stateQuick    ?1:0)
                  + "}";
        client->text(js);
        Serial.printf("WS→Client JSON: %s\n", js.c_str());
      }
    }
  }
}

// SPIFFS Inhalt listen
void checkSPIFFSFiles() {
  Serial.println("Checking SPIFFS files:");
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  int fileCount = 0;
  while(file){
    Serial.print("  FILE: ");
    Serial.println(file.name());
    fileCount++;
    file = root.openNextFile();
  }
  if(fileCount == 0) {
    Serial.println("ALERT: SPIFFS is empty! data folder is not filled with any files");
  }
}

void setup(){
  Serial.begin(115200);
  matrixSerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);

  // SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("✗ SPIFFS mount failed");
    return;
  }
  
  // SPIFFS files überprüfen
  checkSPIFFSFiles();
  
  // WebSocket
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  
  // Static Files
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  
  server.onNotFound([](AsyncWebServerRequest *r){
    Serial.println("Server Route not found");
    r->send(404, "text/plain", "Not found");
  });

  // Upload-Handler …
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *req){ req->send(200, "text/plain", "Upload erfolgreich"); },
    [](AsyncWebServerRequest *req, String fn, size_t idx, uint8_t *data, size_t len, bool fin){
      static File f;
      if(idx==0){
        SPIFFS.remove("/"+fn);
        f = SPIFFS.open("/"+fn, "w");
      }
      if(f) f.write(data, len);
      if(fin){ f.close(); Serial.printf("Upload fertig: %s (%u bytes)\n", fn.c_str(), idx+len); }
    }
  );

  // WLAN

  // Station-Mode sicherstellen (https://randomnerdtutorials.com/esp32-useful-wi-fi-functions-arduino/#1)
  WiFi.mode(WIFI_STA); 

  Serial.printf("Verbinde mit WLAN: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  // Max. 6 Versuche
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 6){
    delay(500); 
    Serial.print(".");
    attempts++;
    Serial.printf(" (Versuch %d, Status: %s)\n", attempts, getWiFiStatusDescription(WiFi.status()).c_str());
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WLAN verbunden!");
    Serial.printf("IP-Adresse: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("Subnetzmaske: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("DNS-Server: %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("MAC-Adresse: %s\n", WiFi.macAddress().c_str());
    Serial.printf("RSSI (Signalstärke): %d dBm\n", WiFi.RSSI());
    Serial.printf("WiFi-Kanal: %d\n", WiFi.channel());
    Serial.printf("Hostname: %s\n", WiFi.getHostname());
  } else {
    Serial.println("\n✗ WLAN konnte nicht verbunden werden. Sammle zusätzliche Diagnose-Informationen...");
    wl_status_t finalStatus = WiFi.status();
    Serial.printf("Letzter Status: %s\n", getWiFiStatusDescription(finalStatus).c_str());
    
    // Scanne verfügbare Netzwerke
    Serial.println("Scanne verfügbare WLAN-Netzwerke...");
    int n = WiFi.scanNetworks();
    Serial.printf("Gefundene Netzwerke: %d\n", n);
    bool ssidFound = false;
    for (int i = 0; i < n; ++i) {
      String ssidName = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      int channel = WiFi.channel(i);
      Serial.printf("  %d: %s (RSSI: %d dBm, Channel: %d)\n", i+1, ssidName.c_str(), rssi, channel);
      if (ssidName == String(ssid)) {
        ssidFound = true;
        Serial.printf("Netzwerk '%s' gefunden!\n", ssid);
      }
    }
    if (!ssidFound) {
      Serial.printf("Gewünschtes Netzwerk '%s' NICHT gefunden\n", ssid);
    }
    
    Serial.println("Bitte für Verbindung sorgen und Device neustarten.");
  }

  // initiale Zustände an Matrix senden
  uint8_t cmd1[] = { BYTESTART, BYTEACTIVE, BYTEON }; //matrix active
  matrixSerial.write(cmd1, sizeof(cmd1));
  uint8_t cmd2[] = { BYTESTART, BYTEBACKL, BYTEOFF }; //Backlight off
  matrixSerial.write(cmd2, sizeof(cmd2));
  uint8_t cmd3[] = { BYTESTART, BYTEINVERT, BYTEOFF }; //not inverted
  matrixSerial.write(cmd3, sizeof(cmd3));
  uint8_t cmd4[] = { BYTESTART, BYTEFASTMODE, BYTEON }; // fast mode on
  matrixSerial.write(cmd4, sizeof(cmd4));

  // lokale Flags setzen
  stateBacklight = false;
  stateInvert    = false;
  stateActive    = true;
  stateQuick     = true;

  server.begin();
}

void loop(){

  // WLAN Reconnect (https://randomnerdtutorials.com/esp32-useful-wi-fi-functions-arduino/#9)
  if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    wl_status_t status = WiFi.status();
    if (status != WL_CONNECTED) {
      Serial.printf("! WLAN verloren. Aktueller Status: %d. Versuche Reconnect...\n", status);
      WiFi.reconnect();
      delay(5000); // Warte 5 Sekunden auf Reconnect
      status = WiFi.status();
      if (status == WL_CONNECTED) {
        Serial.println("✓ WLAN Reconnect erfolgreich!");
        Serial.printf("Neue IP-Adresse: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
      } else {
        Serial.printf("✗ WLAN Reconnect fehlgeschlagen. Status: %d\n", status);
      }
    } else {
      // Info, wenn verbunden
      Serial.printf("WLAN OK - IP: %s, RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
  }

  forwardMatrixResponses();
}
