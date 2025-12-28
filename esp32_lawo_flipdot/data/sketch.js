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
let selMode;
let mode0Container; // Container for Mode 0 specific tools
let mode1Container; // Container for Mode 1 (Pattern Editor) tools
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
  createUI();

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

function createUI() {
  const header = select('#ui-header');
  const controls = select('#controls');

  // Clear existing content if any (though setup runs once)
  if (header) header.html('');
  if (controls) controls.html('');

  // --- Header: Mode Selector ---
  if (header) {
    createSpan('Mode: ').parent(header).style('font-weight', 'bold');
    selMode = createSelect();
    selMode.parent(header);
    selMode.option('Individual Image', 0);
    selMode.option('Pattern Cycle', 1);
    selMode.option('Chaos Mode', 2);
    selMode.changed(onModeChange);
  }

  if (controls) {
    // --- Global Settings ---
    const settingsDiv = createDiv().parent(controls).class('control-group');
    cbBacklight = createCheckbox('Backlight', false).parent(settingsDiv).attribute('disabled', '');
    cbInvert = createCheckbox('Invert', false).parent(settingsDiv).attribute('disabled', '');
    cbActive = createCheckbox('Active', false).parent(settingsDiv).attribute('disabled', '');
    cbQuick = createCheckbox('QuickUpdt', false).parent(settingsDiv).attribute('disabled', '');

    // --- Mode 0 Tools (Fill, Send) ---
    mode0Container = createDiv().parent(controls).class('control-group');

    createButton('Fill Black')
      .parent(mode0Container)
      .mousePressed(() => { fillGrid(0); drawGrid(); });

    createButton('Fill Yellow')
      .parent(mode0Container)
      .mousePressed(() => { fillGrid(1); drawGrid(); });

    createButton('Send')
      .parent(mode0Container)
      .class('send-button')
      .mousePressed(sendMatrix);
  }

  // --- Mode 1 Tools (Pattern Editor) ---
  if (controls) {
    mode1Container = createDiv().parent(controls).class('control-group').style('display', 'none');

    // Editor Tools
    createButton('Fill Black')
      .parent(mode1Container)
      .mousePressed(() => { fillGrid(0); drawGrid(); });

    createButton('Fill Yellow')
      .parent(mode1Container)
      .mousePressed(() => { fillGrid(1); drawGrid(); });

    // Pattern Settings
    const editorSettings = createDiv().parent(mode1Container).style('display', 'flex').style('gap', '10px').style('align-items', 'center');

    createSpan('Filename:').parent(editorSettings);
    const inpFilename = createInput('pattern.json').parent(editorSettings).style('width', '150px');

    const cbPatternBacklight = createCheckbox('Backlight On', false).parent(editorSettings);

    createButton('Save to Device')
      .parent(mode1Container)
      .class('send-button')
      .mousePressed(() => savePattern(inpFilename.value(), cbPatternBacklight.checked()));
  }

  // Checkbox Event Listeners
  cbBacklight.changed(() => sendCmd(BYTEBACKL, cbBacklight.checked() ? BYTEON : BYTEOFF));
  cbInvert.changed(() => {
    sendCmd(BYTEINVERT, cbInvert.checked() ? BYTEON : BYTEOFF);
    drawGrid();
  });
  cbActive.changed(() => sendCmd(BYTEACTIVE, cbActive.checked() ? BYTEON : BYTEOFF));
  cbQuick.changed(() => sendCmd(BYTEFASTMODE, cbQuick.checked() ? BYTEON : BYTEOFF));

  // Initialize visibility
  updateUIForMode(selMode ? selMode.value() : 0);
}

function updateUIForMode(mode) {
  if (!mode0Container || !mode1Container) return;

  // Convert mode to integer just in case
  mode = parseInt(mode);

  if (mode === 0) {
    mode0Container.style('display', 'flex');
    mode1Container.style('display', 'none');
  } else if (mode === 1) {
    mode0Container.style('display', 'none');
    mode1Container.style('display', 'flex');
  } else {
    mode0Container.style('display', 'none');
    mode1Container.style('display', 'none');
  }
}

function onModeChange() {
  const newMode = selMode.value();
  const pwd = prompt("Enter Password to switch mode:");
  if (pwd) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(`setMode:${newMode}:${pwd}`);
    } else {
      alert("Not connected!");
    }
  } else {
    // wait for server update to correct us if needed.
  }
}

// Füll-Funktionen
function fillGrid(v) {
  for (let x = 0; x < colsX; x++) grid[x].fill(v);
}

function savePattern(filename, useBacklight) {
  const arr = [];
  for (let x = 0; x < colsX; x++) {
    for (let block = 0; block < rowsY / 8; block++) {
      let b = 0;
      for (let bit = 0; bit < 8; bit++) {
        if (grid[x][block * 8 + bit]) b |= 1 << (7 - bit);
      }
      arr.push(b);
    }
  }

  const json = {
    data: arr
  };

  if (useBacklight) {
    json.backlight = true;
  } else {
    // Let's explicitly set it to false if unchecked so the pattern enforces "No Backlight".
    json.backlight = false;
  }

  // Send to server
  fetch(`/api/save?filename=${encodeURIComponent(filename)}`, {
    method: 'POST',
    body: JSON.stringify(json),
    headers: {
      'Content-Type': 'application/json'
    }
  })
    .then(response => {
      if (response.ok) {
        alert(`Pattern '${filename}' saved successfully to device!`);
      } else {
        alert('Error saving pattern: ' + response.statusText);
      }
    })
    .catch(error => {
      alert('Network error saving pattern: ' + error);
    });
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
      if (cb) {
        cb.elt.disabled = false;
        cb.checked(s[key] === 1);
      }
    });

    if (s.mode !== undefined) {
      if (selMode) selMode.selected(s.mode);
      updateUIForMode(s.mode);
    }

    drawGrid();
  } else {
    const data = new Uint8Array(evt.data);
    // Check auf Picture-Frame (FF A0 LEN ...)
    if (data.length > 3 && data[0] === BYTESTART && data[1] === BYTEPICTURE) {
      const len = data[2];
      if (len === bytesToSend && data.length >= 3 + len) {
        console.log('← Received full matrix state');
        updateGridFromBytes(data.subarray(3, 3 + len));
        return;
      }
    }

    // Sonst Echo-Bytes loggen
    data.forEach(b => console.log('←', b.toString(16).padStart(2, '0')));
  }
}

function updateGridFromBytes(bytes) {
  let byteIdx = 0;
  for (let x = 0; x < colsX; x++) {
    for (let block = 0; block < rowsY / 8; block++) {
      let b = bytes[byteIdx++];
      for (let bit = 0; bit < 8; bit++) {
        // von links nach rechts (?)
        const val = (b >> (7 - bit)) & 1;
        grid[x][block * 8 + bit] = val;
      }
    }
  }
  drawGrid();
}

let paintValue = 1;    // wird beim Press festgelegt

function handlePixelPaint() {
  // Allow painting in Mode 0 (Individual) and Mode 1 (Pattern Editor)
  // In Mode 1, we just edit the local grid for saving, we don't send updates.
  if (selMode && selMode.value() != 0 && selMode.value() != 1) return;

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
