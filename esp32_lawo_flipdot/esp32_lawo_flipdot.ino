#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>

const char* ssid     = "SECRET_SSID";
const char* password = "SECRET_PW";

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

AsyncWebServer server(80);
AsyncWebSocket  ws("/ws");
HardwareSerial& matrixSerial = Serial1;

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

void setup(){
  Serial.begin(115200);
  matrixSerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);

  // SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("✗ SPIFFS mount failed");
    return;
  }

  // WebSocket
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  // Static Files
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(SPIFFS, "/index.html", "text/html");
  });
  server.on("/sketch.js", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(SPIFFS, "/sketch.js", "application/javascript");
  });
  server.onNotFound([](AsyncWebServerRequest *r){
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
  WiFi.begin(ssid, password);
  Serial.print("Verbinde WLAN");
  while(WiFi.status()!=WL_CONNECTED){
    delay(500); Serial.print(".");
  }
  Serial.println("\n✓ WLAN: " + WiFi.localIP().toString());

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
  forwardMatrixResponses();
}
