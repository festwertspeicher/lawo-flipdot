// --- PROTOKOLL-KONSTANTEN ---
// Bytes vom Browser → ESP32 → Matrix
const BYTESTART = 0xFF;
const BYTEPICTURE = 0xA0;
const BYTEBACKL = 0xA1;
const BYTEINVERT = 0xA2;
const BYTEACTIVE = 0xA3;
const BYTEFASTMODE = 0xA4;

const BYTEON = 0x01;
const BYTEOFF = 0x00;

const colsX = 84;
const rowsY = 16;
const bytesToSend = (rowsY / 8) * colsX;

let grid = [];
let ws;
// Checkbox-Referenzen
let cbBacklight, cbInvert, cbActive, cbQuick;
// zuletzt bearbeiteter Pixel
let lastX = -1, lastY = -1;

function connectWebSocket() {
  console.log("Connecting WebSocket...");
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    console.log("WebSocket connected");
    ws.send('getState');
  };

  ws.onmessage = handleWSMessage;

  ws.onclose = (e) => {
    console.log('WebSocket connection closed. Reconnecting in 3s...', e.reason);
    setTimeout(connectWebSocket, 3000);
  };

  ws.onerror = (err) => {
    console.error('WebSocket error: ', err);
    ws.close();
  };
}

function setup() {
  console.log("+++ setup() gestartet");

  // Canvas
  const sw = colsX * 10, sh = rowsY * 10;
  createCanvas(sw, sh).parent('sketch');

  // Grid initialisieren
  for (let i = 0; i < colsX; i++) {
    grid[i] = Array(rowsY).fill(0);
  }

  // UI-Elemente anlegen
  createButtons();

  // WebSocket
  connectWebSocket();

  drawGrid();
}

function drawGrid() {
  background(100);
  noStroke();
  const invert = cbInvert && cbInvert.checked();
  for (let x = 0; x < colsX; x++) {
    for (let y = 0; y < rowsY; y++) {
      let val = grid[x][y];
      if (invert) val = 1 - val;
      fill(val ? 'yellow' : 'black');
      rect(x * 10, y * 10, 10, 10);
    }
  }
}

function createButtons() {
  const container = select('#controls');
  container.html('');

  // Muster-Buttons ohne Random
  createButton('Fill Black')
    .parent(container)
    .mousePressed(() => { fillGrid(0); drawGrid(); });

  createButton('Fill Yellow')
    .parent(container)
    .mousePressed(() => { fillGrid(1); drawGrid(); });

  // Send-Button rechts
  createButton('Send')
    .parent(container)
    .class('send-button')
    .mousePressed(sendMatrix);

  // Command-Checkboxen
  cbBacklight = createCheckbox('Backlight', false).parent(container).attribute('disabled', '');
  cbInvert = createCheckbox('Invert', false).parent(container).attribute('disabled', '');
  cbActive = createCheckbox('Active', false).parent(container).attribute('disabled', '');
  cbQuick = createCheckbox('QuickUpdt', false).parent(container).attribute('disabled', '');

  cbBacklight.changed(() => sendCmd(BYTEBACKL, cbBacklight.checked() ? BYTEON : BYTEOFF));
  cbInvert.changed(() => {
    sendCmd(BYTEINVERT, cbInvert.checked() ? BYTEON : BYTEOFF);
    drawGrid();
  });
  cbActive.changed(() => sendCmd(BYTEACTIVE, cbActive.checked() ? BYTEON : BYTEOFF));
  cbQuick.changed(() => sendCmd(BYTEFASTMODE, cbQuick.checked() ? BYTEON : BYTEOFF));
}

// Füll-Funktionen
function fillGrid(v) {
  for (let x = 0; x < colsX; x++) grid[x].fill(v);
}

// Matrix senden
function sendMatrix() {
  const arr = [BYTESTART, BYTEPICTURE, bytesToSend];
  for (let x = 0; x < colsX; x++) {
    for (let block = 0; block < rowsY / 8; block++) {
      let b = 0;
      for (let bit = 0; bit < 8; bit++) {
        if (grid[x][block * 8 + bit]) b |= 1 << (7 - bit);
      }
      arr.push(b);
    }
  }
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(Uint8Array.from(arr));
    console.log('→ A0: Send Bitmap');
  } else {
    console.log('WebSocket not connected, cannot send bitmap');
  }
}

// Protokollbefehle
function sendCmd(actionByte, param) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(Uint8Array.from([BYTESTART, actionByte, param]));
  }
}

// WS-Handler
function handleWSMessage(evt) {
  if (typeof evt.data === 'string') {
    const s = JSON.parse(evt.data);
    [
      { cb: cbBacklight, key: 'backlight' },
      { cb: cbInvert, key: 'invert' },
      { cb: cbActive, key: 'active' },
      { cb: cbQuick, key: 'quick' }
    ].forEach(({ cb, key }) => {
      cb.elt.disabled = false;
      cb.checked(s[key] === 1);
    });
    drawGrid();
  } else {
    const data = new Uint8Array(evt.data);
    data.forEach(b => console.log('←', b.toString(16).padStart(2, '0')));
  }
}

let paintValue = 1;    // wird beim Press festgelegt

function handlePixelPaint() {
  const x = floor(mouseX / 10);
  const y = floor(mouseY / 10);
  if (x >= 0 && x < colsX && y >= 0 && y < rowsY) {
    if (lastX === -1 && lastY === -1) {
      // erster Klick: Bestimme Modus anhand des Start-Pixels
      paintValue = grid[x][y] === 0 ? 1 : 0;
    }
    if (x !== lastX || y !== lastY) {
      // setze das Feld auf paintValue statt toggeln
      grid[x][y] = paintValue;
      lastX = x;
      lastY = y;
      drawGrid();
    }
  } else {
    lastX = lastY = -1;
  }
}

function mousePressed() {
  lastX = lastY = -1;
  handlePixelPaint();
}

function mouseDragged() {
  handlePixelPaint();
}

function mouseReleased() {
  lastX = lastY = -1;
}