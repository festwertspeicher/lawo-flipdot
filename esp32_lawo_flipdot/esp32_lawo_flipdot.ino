#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
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
const long PATTERN_INTERVAL = 10 * 1000;
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
    debugMessage("Update is not in valid format.");
    return;
  }

  // 1) in Seriellen Monitor loggen
  debugMessage("WS→ESP32 (len=" + String(len) + "): ");
  for(size_t i=0;i<len;i++){
    debugMessage("0x" + String(data[i], HEX) + " ");
  }

  // 2) an die Matrix senden
  size_t written = matrixSerial.write(data, len);
  debugMessage("ESP32→Matrix (written=" + String(written) + ")");
}

// Rückkanal: Serial1 → WebSocket
void forwardMatrixResponses(){
  while(matrixSerial.available()){
    uint8_t b = matrixSerial.read();
    ws.binaryAll(&b, 1);
    debugMessage("Matrix→ESP32→WS: 0x" + String(b, HEX));
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
  File root = LittleFS.open("/patterns");
  if(!root || !root.isDirectory()){
    debugMessage("Patterns directory not found! Creating...");
    if(LittleFS.mkdir("/patterns")){
       debugMessage("Patterns directory created");
       root = LittleFS.open("/patterns");
    } else {
       debugMessage("Error creating patterns directory");
       return;
    }
  }
  File file = root.openNextFile();
  while(file){
    String name = String(file.name());
    if(name.endsWith(".json")) {
      size_t fSize = file.size();
      
      String fullPath = name;
      if (!fullPath.startsWith("/")) {
        if (!fullPath.startsWith("patterns/") && !fullPath.startsWith("/patterns/")) {
             fullPath = "/patterns/" + fullPath; 
        } else if (!fullPath.startsWith("/")) {
             fullPath = "/" + fullPath;
        }
      }

      if (fSize > 0) {
        patterns.push_back(fullPath);
        debugMessage("Found pattern: " + fullPath + " (Size: " + String(fSize) + ")");
      } else {
        debugMessage("Skipping empty pattern: " + fullPath + " (Size: 0)");
      }
    }
    file = root.openNextFile();
  }
  debugMessage("Total patterns found: " + String(patterns.size()));
}

void updatePattern() {
  if (millis() - lastModeUpdate >= PATTERN_INTERVAL) {
    lastModeUpdate = millis();
    if (patterns.empty()) return;

    // Try up to patterns.size() times to find a valid pattern
    for(size_t attempt = 0; attempt < patterns.size(); attempt++) {
        // Cycle index
        currentPatternIndex = (currentPatternIndex + 1) % patterns.size();
        String path = patterns[currentPatternIndex];
        
        if (!path.startsWith("/")) path = "/" + path; // Safety
    
        File f = LittleFS.open(path, "r");
        if (f) {
          size_t fSize = f.size();
          if (fSize == 0) {
            debugMessage("Error: Pattern file '" + path + "' is empty! Skipping...");
            f.close();
            continue; // Try next pattern immediately without waiting
          }
          
          debugMessage("Opening pattern: " + path + ", Size: " + String(fSize));
    
          JsonDocument doc; 
          DeserializationError error = deserializeJson(doc, f);
          if (!error) {
            // ... (rest of logic) ...
            
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
              debugMessage("Displayed Pattern: " + path);
              f.close();  
              return; // Success!
            } else {
              debugMessage("Pattern data size mismatch");
            }
          } else {
            debugMessage("JSON Parse Error: " + String(error.c_str()));
          }
          f.close();
        } else {
          debugMessage("Failed to open pattern: " + path);
        }
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
    if (random(0, 100) < 25) { // 25% chance
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
    debugMessage("WS Client #" + String(client->id()) + " connected");
  }
  else if(type == WS_EVT_DISCONNECT){
    debugMessage("WS Client #" + String(client->id()) + " disconnected");
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
        doc["mode"]      = currentMode;

        String js;
        serializeJson(doc, js);
        client->text(js);

        // aktuelles Bild senden (matrixBuffer wird oben im RAM gespeichert)
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
               debugMessage("Mode changed to " + String(newMode));
             }
           } else {
             debugMessage("Invalid Password for Mode Change");
             // Send error back to client
             JsonDocument errDoc;
             errDoc["error"] = "Invalid Password";
             String errJs;
             serializeJson(errDoc, errJs);
             client->text(errJs);
           }
        }
      }
    }
  }
}

void debugMessage(String message) {
  if (debugMode) {
    Serial.println(message);
  }
}

// LittleFS Inhalt listen
void checkLittleFSFiles() {
  debugMessage("Checking LittleFS files:");
  
  debugMessage("LittleFS Total: " + String(LittleFS.totalBytes()) + ", Used: " + String(LittleFS.usedBytes()));

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  int fileCount = 0;
  while(file){
    debugMessage("  FILE: " + file.name() + " (Size: " + String(file.size()) + ")");
    fileCount++;
    file = root.openNextFile();
  }
  if(fileCount == 0) {
    debugMessage("ALERT: LittleFS is empty! data folder is not filled with any files");
  }
}

void setup(){
  Serial.begin(115200);
  matrixSerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);

  if(!LittleFS.begin(true)){
    debugMessage("✗ LittleFS mount failed");
    return;
  }
  
  checkLittleFSFiles();

  loadPatterns();
  
  // WebSocket
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  
  // Static Files
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *r){
    debugMessage("Server Route not found");
    r->send(404, "text/plain", "Not found");
  });

  // API to save pattern directly from editor
  server.on("/api/save", HTTP_POST, 
    [](AsyncWebServerRequest *request){ 
      request->send(200, "text/plain", "Pattern Saved"); 
    }, 
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      static File f;
      if(index == 0){
        if(f) f.close();
        
        String filename = "pattern.json";
        if(request->hasParam("filename")) {
           filename = request->getParam("filename")->value();
        }
        if(!filename.endsWith(".json")) filename += ".json";
        
        // Ensure it goes to /patterns/
        int lastSlash = filename.lastIndexOf('/');
        if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
        
        String path = "/patterns/" + filename;
        debugMessage("Saving pattern to: " + path);

        LittleFS.remove(path);
        f = LittleFS.open(path, "w");
        if(!f) debugMessage("ERROR: LittleFS.open failed!");
      }
      if(f){
        f.write(data, len);
        debugMessage("Write pattern");
      }
      if(index + len == total){
        if(f) f.close();
        debugMessage("Pattern save complete");
        loadPatterns();
      }
    }
  );

  // Upload-Handler …
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *req){ req->send(200, "text/plain", "Upload successful"); },
    [](AsyncWebServerRequest *req, String fn, size_t idx, uint8_t *data, size_t len, bool fin){
      static File f;
      if(idx==0){
        LittleFS.remove("/"+fn);
        f = LittleFS.open("/"+fn, "w");
      }
      if(f) f.write(data, len);
      if(fin){ f.close(); debugMessage("Upload erfolgreich: " + fn + " (" + String(idx+len) + " bytes)"); }
    }
  );

  // WLAN

  // Station-Mode sicherstellen (https://randomnerdtutorials.com/esp32-useful-wi-fi-functions-arduino/#1)
  WiFi.mode(WIFI_STA); 

  if (ssid != NULL) {
    debugMessage("Verbinde mit WLAN: " + String(ssid));
  } else {
    debugMessage("Fehler: SSID ist NULL! Bitte in secrets.h eintragen.");
    return;
  }

  WiFi.begin(ssid, password);
  
  // Max. 6 Versuche
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 6){
    delay(500); 
    debugMessage("WLAN Verbindung wird hergestellt...");
    attempts++;
    debugMessage(" (Versuch " + String(attempts) + ", Status: " + getWiFiStatusDescription(WiFi.status()) + ")");
  }

  if(WiFi.status() == WL_CONNECTED) {
    debugMessage("\n✓ WLAN verbunden!");
    debugMessage("IP-Adresse: " + WiFi.localIP().toString());
    debugMessage("Gateway: " + WiFi.gatewayIP().toString());
    debugMessage("Subnetzmaske: " + WiFi.subnetMask().toString());
    debugMessage("DNS-Server: " + WiFi.dnsIP().toString());
    debugMessage("MAC-Adresse: " + WiFi.macAddress());
    debugMessage("RSSI (Signalstärke): " + String(WiFi.RSSI()) + " dBm");
    debugMessage("WiFi-Kanal: " + String(WiFi.channel()));
    debugMessage("Hostname: " + WiFi.getHostname());
  } else {
    debugMessage("\n✗ WLAN konnte nicht verbunden werden. Sammle zusätzliche Diagnose-Informationen...");
    wl_status_t finalStatus = WiFi.status();
    debugMessage("Letzter Status: " + getWiFiStatusDescription(finalStatus));
    
    // Scanne verfügbare Netzwerke
    debugMessage("Scanne verfügbare WLAN-Netzwerke...");
    int n = WiFi.scanNetworks();
    debugMessage("Gefundene Netzwerke: " + String(n));
    bool ssidFound = false;
    for (int i = 0; i < n; ++i) {
      String ssidName = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      int channel = WiFi.channel(i);
      debugMessage("  " + String(i+1) + ": " + ssidName + " (RSSI: " + String(rssi) + " dBm, Channel: " + String(channel) + ")");
      if (ssidName == String(ssid != NULL ? ssid : "")) {
        ssidFound = true;
        debugMessage("Netzwerk '" + String(ssid != NULL ? ssid : "NULL") + "' gefunden!");
      }
    }
    if (!ssidFound) {
      debugMessage("Gewünschtes Netzwerk '" + String(ssid != NULL ? ssid : "NULL") + "' NICHT gefunden");
    }
    
    debugMessage("Bitte für Verbindung sorgen und Device neustarten.");
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
      debugMessage("! WLAN verloren. Aktueller Status: " + String(status) + ". Versuche Reconnect...");
      WiFi.reconnect();
      delay(5000); // Warte 5 Sekunden auf Reconnect
      status = WiFi.status();
      if (status == WL_CONNECTED) {
        debugMessage("✓ WLAN Reconnect erfolgreich!");
        debugMessage("Neue IP-Adresse: " + WiFi.localIP().toString());
        debugMessage("RSSI: " + String(WiFi.RSSI()) + " dBm");
      } else {
        debugMessage("✗ WLAN Reconnect fehlgeschlagen. Status: " + String(status));
      }
    } else {
      // Info, wenn verbunden
      debugMessage("WLAN OK - IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
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
