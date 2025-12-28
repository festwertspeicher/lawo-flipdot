#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <vector>
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
const uint8_t BYTEMODE     = 0xA5; // New: Mode switch command

const uint8_t BYTEON  = 0x01;
const uint8_t BYTEOFF = 0x00;

// Operation Modes
enum OperationMode {
  MODE_INDIVIDUALIMAGE = 0,
  MODE_PATTERN = 1,
  MODE_CHAOS = 2
};

// aktuelle States
OperationMode currentMode = MODE_PATTERN;
long lastModeUpdate = 0;
std::vector<String> patterns;
int currentPatternIndex = 0;
const long PATTERN_INTERVAL = 20 * 1000;
const long CHAOS_INTERVAL = 1 * 1000;

bool stateBacklight = false;
bool stateInvert    = false;
bool stateActive    = true;
bool stateQuick     = true;

const int MATRIX_BYTES = 168; // 84 * 2
uint8_t matrixBuffer[MATRIX_BYTES] = {0};

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
    
    // Block Picture updates if not in WS mode
    if (action == BYTEPICTURE && currentMode != MODE_INDIVIDUALIMAGE) {
      Serial.println("Ignored Picture update (Not in INDIVIDUALIMAGE Mode)");
      return; 
    }

    switch(action) {
      case BYTEBACKL:    stateBacklight = (param == BYTEON);    break;
      case BYTEINVERT:   stateInvert    = (param == BYTEON);    break;
      case BYTEACTIVE:   stateActive    = (param == BYTEON);    break;
      case BYTEFASTMODE: stateQuick     = (param == BYTEON);    break;
      case BYTEPICTURE:
        // Bild-Daten speichern (param ist hier die Länge, z.B. 168)
        if (len >= 3 + param && param <= MATRIX_BYTES) {
          memcpy(matrixBuffer, data + 3, param);
        }
        break;
    }
  } else {
    Serial.println("Update is not in valid format.");
    return;
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

// Helper to send current matrixBuffer to Matrix
void sendBufferToMatrix() {
  uint8_t header[] = { BYTESTART, BYTEPICTURE, (uint8_t)MATRIX_BYTES };
  matrixSerial.write(header, 3);
  matrixSerial.write(matrixBuffer, MATRIX_BYTES);
}

void loadPatterns() {
  patterns.clear();
  File root = SPIFFS.open("/patterns");
  if(!root || !root.isDirectory()){
    Serial.println("Patterns directory not found!");
    // Create it if missing?
    // SPIFFS doesn't really have directories, just filenames with slashes.
    return;
  }
  File file = root.openNextFile();
  while(file){
    String name = String(file.name());
    if(name.endsWith(".json")) {
      patterns.push_back(name);
      Serial.print("Found pattern: "); Serial.println(name);
    }
    file = root.openNextFile();
  }
  Serial.printf("Total patterns found: %d\n", patterns.size());
}

void updatePattern() {
  if (millis() - lastModeUpdate >= PATTERN_INTERVAL) {
    lastModeUpdate = millis();
    if (patterns.empty()) return;

    // Cycle index
    currentPatternIndex = (currentPatternIndex + 1) % patterns.size();
    String path = patterns[currentPatternIndex];
    // Ensure path starts with slash if needed, but file.name() usually has it or not depending on fs implementation
    // SPIFFS flat fs usually includes full path.
    if (!path.startsWith("/")) path = "/" + path; // Safety

    File f = SPIFFS.open(path, "r");
    if (f) {
      JsonDocument doc; // Dynamic size? 168 bytes is small.
      DeserializationError error = deserializeJson(doc, f);
      if (!error) {
        
        // Handle Backlight if present in JSON
        if (doc.containsKey("backlight")) {
            bool bl = doc["backlight"];
            if (bl != stateBacklight) {
                stateBacklight = bl;
                uint8_t cmd[] = { BYTESTART, BYTEBACKL, bl ? BYTEON : BYTEOFF };
                matrixSerial.write(cmd, 3);
            }
        }
        
        JsonArray data = doc["data"];
        if (data.size() == MATRIX_BYTES) {
          for (int i = 0; i < MATRIX_BYTES; i++) {
            matrixBuffer[i] = data[i];
          }
          sendBufferToMatrix();
          Serial.printf("Displayed Pattern: %s\n", path.c_str());
        } else {
          Serial.println("Pattern data size mismatch");
        }
      } else {
        Serial.print("JSON Parse Error: "); Serial.println(error.c_str());
      }
      f.close();
    } else {
      Serial.print("Failed to open pattern: "); Serial.println(path);
    }
  }
}

void updateChaos() {
  if (millis() - lastModeUpdate >= CHAOS_INTERVAL) {
    lastModeUpdate = millis();
    
    // Randomize buffer
    for (int i = 0; i < MATRIX_BYTES; i++) {
      matrixBuffer[i] = random(0, 256);
    }
    
    // Randomly toggle backlight sometimes
    if (random(0, 100) < 5) { // 5% chance
       bool newBl = !stateBacklight;
       stateBacklight = newBl;
       uint8_t cmd[] = { BYTESTART, BYTEBACKL, newBl ? BYTEON : BYTEOFF };
       matrixSerial.write(cmd, 3);
    }

    sendBufferToMatrix();
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
      // Text-Frame (z.B. "getState" oder "setMode:1:pass")
      String msg = String((char*)data).substring(0, len);
      
      if(msg == "getState"){
        // JSON zusammenbauen
        JsonDocument doc;
        doc["backlight"] = stateBacklight ? 1 : 0;
        doc["invert"]    = stateInvert    ? 1 : 0;
        doc["active"]    = stateActive    ? 1 : 0;
        doc["quick"]     = stateQuick     ? 1 : 0;
        doc["mode"]      = (int)currentMode;

        String js;
        serializeJson(doc, js);
        client->text(js);
        // Serial.printf("WS→Client JSON: %s\n", js.c_str());

        // aktuelles Bild senden
        uint8_t *msgBuf = new uint8_t[3 + MATRIX_BYTES];
        msgBuf[0] = BYTESTART; 
        msgBuf[1] = BYTEPICTURE; 
        msgBuf[2] = MATRIX_BYTES;
        memcpy(msgBuf + 3, matrixBuffer, MATRIX_BYTES);
        client->binary(msgBuf, 3 + MATRIX_BYTES);
        delete[] msgBuf;
      }
      else if (msg.startsWith("setMode:")) {
        // Format: setMode:<MODE>:<PASS>
        int firstColon = msg.indexOf(':');
        int secondColon = msg.indexOf(':', firstColon + 1);
        if (secondColon > 0) {
           String modeStr = msg.substring(firstColon + 1, secondColon);
           String passStr = msg.substring(secondColon + 1);
           
           if (passStr == String(modePassword)) {
             int newMode = modeStr.toInt();
             if (newMode >= 0 && newMode <= 2) {
               currentMode = (OperationMode)newMode;
               lastModeUpdate = 0; // Trigger immediate update
               
               // Broadcast new mode to all clients
               JsonDocument doc;
               doc["mode"] = (int)currentMode;
               doc["backlight"] = stateBacklight ? 1 : 0; // Send full state just in case
               // ...
               String js;
               serializeJson(doc, js);
               ws.textAll(js);
               Serial.printf("Mode changed to %d\n", newMode);
             }
           } else {
             Serial.println("Invalid Password for Mode Change");
           }
        }
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
  loadPatterns();
  
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

  // Mode Loops
  if (currentMode == MODE_PATTERN) {
    updatePattern();
  } else if (currentMode == MODE_CHAOS) {
    updateChaos();
  }

  forwardMatrixResponses();
}
