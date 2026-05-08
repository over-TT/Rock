// ROCK RC - Controller firmware (ESP32 / Seeed XIAO ESP32-S3)
// Hosts the phone web UI and bridges joystick input to the car over ESP-NOW.
// Licensed under the MIT License. See LICENSE at the repository root.

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>

// ===== WIFI / ESP-NOW SETTINGS =====
const char* ssid = "ROCK_RC";
const char* password = "12345678";

const int WIFI_CHANNEL = 6;

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// ===== ESP-NOW TYPES =====
const uint8_t TYPE_CONTROL = 1;
const uint8_t TYPE_STATUS  = 2;

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t carMac[6];
bool carKnown = false;
bool carPeerAdded = false;
bool needAddCarPeer = false;

// ===== CONTROL PACKET =====
// Must match RC car code EXACTLY
typedef struct __attribute__((packed)) {
  uint8_t type;
  int16_t motor;        // -100 to 100
  int16_t steer;        // -100 to 100
  int16_t servoOffset;  // -30 to +30 degrees
  uint8_t maxSpeed;     // 10 to 100 percent
  uint8_t rampStep;     // 1 to 25
  uint32_t seq;
} ControlPacket;

// ===== STATUS PACKET =====
// Must match RC car code EXACTLY
typedef struct __attribute__((packed)) {
  uint8_t type;
  uint32_t lastSeqReceived;
  int16_t targetMotor;
  int16_t currentMotor;
  int16_t steeringAngle;
  uint8_t maxSpeed;
  uint8_t rampStep;
  uint8_t failsafeActive;
} CarStatusPacket;

ControlPacket controlPacket;
CarStatusPacket carStatus;

uint32_t seqNum = 0;

// Latest values from phone
int currentMotor = 0;
int currentSteer = 0;
int currentServoOffset = 0;
int currentMaxSpeed = 60;
int currentRampStep = 4;

unsigned long lastPhoneCommand = 0;
const unsigned long PHONE_TIMEOUT = 500;

unsigned long lastCarStatusTime = 0;
unsigned long lastStatusWebSend = 0;
const unsigned long WEB_STATUS_INTERVAL = 200;

// ===== HTML PAGE =====
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>ROCK RC Controller</title>

  <style>
    body {
      margin: 0;
      font-family: Arial, sans-serif;
      background: #111;
      color: white;
      text-align: center;
      overflow: hidden;
    }

    h2 {
      margin: 10px 0 4px 0;
      font-size: 23px;
    }

    .topBar {
      display: flex;
      justify-content: center;
      align-items: center;
      gap: 8px;
      flex-wrap: wrap;
      padding: 3px 8px;
    }

    .toggleBtn {
      background: #2a2a2a;
      color: white;
      border: 1px solid #555;
      border-radius: 10px;
      padding: 7px 12px;
      font-size: 14px;
    }

    .statusRow {
      display: flex;
      justify-content: center;
      gap: 8px;
      flex-wrap: wrap;
      padding: 3px 8px;
    }

    .statusBox {
      background: #1b1b1b;
      border: 1px solid #333;
      border-radius: 10px;
      padding: 6px 9px;
      min-width: 120px;
      font-size: 12px;
    }

    .good { color: #55ff88; }
    .bad { color: #ff6666; }
    .warn { color: #ffcc66; }

    .settings {
      display: flex;
      justify-content: center;
      gap: 10px;
      flex-wrap: wrap;
      padding: 5px 10px;
      font-size: 13px;
      max-height: 140px;
      overflow: hidden;
      transition: max-height 0.2s ease, opacity 0.2s ease, padding 0.2s ease;
      opacity: 1;
    }

    .settings.collapsed {
      max-height: 0;
      opacity: 0;
      padding-top: 0;
      padding-bottom: 0;
      pointer-events: none;
    }

    .settingBox {
      background: #1b1b1b;
      border: 1px solid #333;
      border-radius: 10px;
      padding: 7px;
      min-width: 155px;
    }

    input[type="range"] {
      width: 140px;
    }

    .container {
      display: flex;
      justify-content: space-around;
      align-items: center;
      height: calc(100vh - 165px);
      gap: 18px;
      padding: 8px;
    }

    .stickBox {
      display: flex;
      flex-direction: column;
      align-items: center;
    }

    .label {
      font-size: 19px;
      margin-bottom: 9px;
    }

    .joystick {
      width: 220px;
      height: 220px;
      background: #222;
      border: 3px solid #555;
      border-radius: 50%;
      position: relative;
      touch-action: none;
    }

    .knob {
      width: 80px;
      height: 80px;
      background: #ddd;
      border-radius: 50%;
      position: absolute;
      left: 70px;
      top: 70px;
      box-shadow: 0 0 15px #000;
    }

    .value {
      margin-top: 9px;
      font-size: 15px;
      color: #bbb;
    }

    @media (max-height: 600px) {
      h2 { display: none; }

      .statusBox {
        font-size: 11px;
        padding: 4px 7px;
        min-width: 105px;
      }

      .container {
        height: calc(100vh - 95px);
      }

      .joystick {
        width: 190px;
        height: 190px;
      }

      .knob {
        width: 70px;
        height: 70px;
        left: 60px;
        top: 60px;
      }
    }
  </style>
</head>

<body>
  <h2>ROCK RC Car Control</h2>

  <div class="topBar">
    <button id="settingsToggle" class="toggleBtn">Show Settings</button>
  </div>

  <div class="statusRow">
    <div class="statusBox">
      Phone: <span id="phoneStatus" class="bad">Disconnected</span>
    </div>
    <div class="statusBox">
      Car: <span id="carStatus" class="bad">Searching</span>
    </div>
    <div class="statusBox">
      Last seen: <span id="lastSeen">---</span>
    </div>
    <div class="statusBox">
      Seq: <span id="seqVal">---</span>
    </div>
    <div class="statusBox">
      Motor: <span id="motorStatus">---</span>
    </div>
    <div class="statusBox">
      Servo: <span id="servoStatus">---</span>
    </div>
  </div>

  <div id="settingsPanel" class="settings collapsed">
    <div class="settingBox">
      <div>Servo offset: <span id="offsetVal">0</span>°</div>
      <input id="offsetSlider" type="range" min="-30" max="30" value="0">
    </div>

    <div class="settingBox">
      <div>Max speed: <span id="speedVal">60</span>%</div>
      <input id="speedSlider" type="range" min="10" max="100" value="60">
    </div>

    <div class="settingBox">
      <div>Ramp step: <span id="rampVal">4</span></div>
      <input id="rampSlider" type="range" min="1" max="25" value="4">
    </div>
  </div>

  <div class="container">
    <div class="stickBox">
      <div class="label">Motor</div>
      <div id="motorJoy" class="joystick">
        <div id="motorKnob" class="knob"></div>
      </div>
      <div id="motorVal" class="value">Motor: 0</div>
    </div>

    <div class="stickBox">
      <div class="label">Steering</div>
      <div id="steerJoy" class="joystick">
        <div id="steerKnob" class="knob"></div>
      </div>
      <div id="steerVal" class="value">Steer: 0</div>
    </div>
  </div>

<script>
let motor = 0;
let steer = 0;

let servoOffset = 0;
let maxSpeed = 60;
let rampStep = 4;

let ws;

const phoneStatus = document.getElementById("phoneStatus");
const carStatus = document.getElementById("carStatus");
const settingsPanel = document.getElementById("settingsPanel");
const settingsToggle = document.getElementById("settingsToggle");

settingsToggle.addEventListener("click", function() {
  settingsPanel.classList.toggle("collapsed");

  if (settingsPanel.classList.contains("collapsed")) {
    settingsToggle.innerText = "Show Settings";
  } else {
    settingsToggle.innerText = "Hide Settings";
  }
});

document.getElementById("offsetSlider").addEventListener("input", function() {
  servoOffset = parseInt(this.value);
  document.getElementById("offsetVal").innerText = servoOffset;
});

document.getElementById("speedSlider").addEventListener("input", function() {
  maxSpeed = parseInt(this.value);
  document.getElementById("speedVal").innerText = maxSpeed;
});

document.getElementById("rampSlider").addEventListener("input", function() {
  rampStep = parseInt(this.value);
  document.getElementById("rampVal").innerText = rampStep;
});

function connectWebSocket() {
  ws = new WebSocket("ws://" + location.hostname + ":81/");

  ws.onopen = function() {
    phoneStatus.innerText = "Connected";
    phoneStatus.className = "good";
  };

  ws.onclose = function() {
    phoneStatus.innerText = "Disconnected";
    phoneStatus.className = "bad";
    setTimeout(connectWebSocket, 500);
  };

  ws.onerror = function() {
    phoneStatus.innerText = "Error";
    phoneStatus.className = "warn";
  };

  ws.onmessage = function(event) {
    const msg = event.data;

    if (!msg.startsWith("S,")) return;

    const p = msg.split(",");

    const connected = parseInt(p[1]);
    const age = parseInt(p[2]);
    const seq = p[3];
    const targetMotor = p[4];
    const currentMotor = p[5];
    const steeringAngle = p[6];
    const failsafe = parseInt(p[7]);

    if (connected) {
      carStatus.innerText = failsafe ? "Failsafe" : "Connected";
      carStatus.className = failsafe ? "warn" : "good";
      document.getElementById("lastSeen").innerText = age + " ms";
    } else {
      carStatus.innerText = "Searching";
      carStatus.className = "bad";
      document.getElementById("lastSeen").innerText = "---";
    }

    document.getElementById("seqVal").innerText = seq;
    document.getElementById("motorStatus").innerText = targetMotor + " / " + currentMotor;
    document.getElementById("servoStatus").innerText = steeringAngle + "°";
  };
}

connectWebSocket();

function sendControl() {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send("C," + motor + "," + steer + "," + servoOffset + "," + maxSpeed + "," + rampStep);
  }
}

setInterval(sendControl, 30);

function setupJoystick(joyId, knobId, type) {
  const joy = document.getElementById(joyId);
  const knob = document.getElementById(knobId);

  const joySize = 220;
  const knobSize = 80;
  const center = joySize / 2;
  const maxDist = 70;

  let active = false;

  function moveKnob(dx, dy) {
    let distance = Math.sqrt(dx * dx + dy * dy);

    if (distance > maxDist) {
      dx = dx / distance * maxDist;
      dy = dy / distance * maxDist;
    }

    if (type === "motor") {
      dx = 0;
      motor = Math.round((-dy / maxDist) * 100);
      if (Math.abs(motor) < 5) motor = 0;
      document.getElementById("motorVal").innerText = "Motor: " + motor;
    }

    if (type === "steer") {
      dy = 0;
      steer = Math.round((dx / maxDist) * 100);
      if (Math.abs(steer) < 5) steer = 0;
      document.getElementById("steerVal").innerText = "Steer: " + steer;
    }

    knob.style.left = (center - knobSize / 2 + dx) + "px";
    knob.style.top = (center - knobSize / 2 + dy) + "px";
  }

  function resetKnob() {
    knob.style.left = "70px";
    knob.style.top = "70px";

    if (type === "motor") {
      motor = 0;
      document.getElementById("motorVal").innerText = "Motor: 0";
    }

    if (type === "steer") {
      steer = 0;
      document.getElementById("steerVal").innerText = "Steer: 0";
    }

    sendControl();
  }

  joy.addEventListener("pointerdown", function(e) {
    active = true;
    joy.setPointerCapture(e.pointerId);
  });

  joy.addEventListener("pointermove", function(e) {
    if (!active) return;

    const rect = joy.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;

    const dx = x - center;
    const dy = y - center;

    moveKnob(dx, dy);
  });

  joy.addEventListener("pointerup", function() {
    active = false;
    resetKnob();
  });

  joy.addEventListener("pointercancel", function() {
    active = false;
    resetKnob();
  });
}

setupJoystick("motorJoy", "motorKnob", "motor");
setupJoystick("steerJoy", "steerKnob", "steer");
</script>

</body>
</html>
)rawliteral";

void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

void addCarPeerIfNeeded() {
  if (!carKnown) return;
  if (!needAddCarPeer && carPeerAdded) return;

  if (esp_now_is_peer_exist(carMac)) {
    carPeerAdded = true;
    needAddCarPeer = false;
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, carMac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    carPeerAdded = true;
    needAddCarPeer = false;

    Serial.print("Car paired: ");
    printMac(carMac);
    Serial.println();
  } else {
    Serial.println("Failed to add car peer");
  }
}

void sendPacketToCar(int motorValue, int steerValue, int servoOffset, int maxSpeed, int rampStep) {
  controlPacket.type = TYPE_CONTROL;
  controlPacket.motor = constrain(motorValue, -100, 100);
  controlPacket.steer = constrain(steerValue, -100, 100);
  controlPacket.servoOffset = constrain(servoOffset, -30, 30);
  controlPacket.maxSpeed = constrain(maxSpeed, 10, 100);
  controlPacket.rampStep = constrain(rampStep, 1, 25);
  controlPacket.seq = seqNum++;

  uint8_t* target = broadcastAddress;

  if (carPeerAdded && millis() - lastCarStatusTime < 2000) {
    target = carMac;
  }

  esp_now_send(target, (uint8_t*)&controlPacket, sizeof(controlPacket));
}

void webSocketEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length) {
  if (type != WStype_TEXT) return;

  String msg = "";
  for (size_t i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  char cmd;
  int motorValue, steerValue, servoOffset, maxSpeed, rampStep;

  int parsed = sscanf(
    msg.c_str(),
    "%c,%d,%d,%d,%d,%d",
    &cmd,
    &motorValue,
    &steerValue,
    &servoOffset,
    &maxSpeed,
    &rampStep
  );

  if (parsed != 6) return;
  if (cmd != 'C') return;

  currentMotor = constrain(motorValue, -100, 100);
  currentSteer = constrain(steerValue, -100, 100);
  currentServoOffset = constrain(servoOffset, -30, 30);
  currentMaxSpeed = constrain(maxSpeed, 10, 100);
  currentRampStep = constrain(rampStep, 1, 25);

  lastPhoneCommand = millis();

  sendPacketToCar(
    currentMotor,
    currentSteer,
    currentServoOffset,
    currentMaxSpeed,
    currentRampStep
  );
}

void handleEspNowReceive(const uint8_t *srcMac, const uint8_t *data, int len) {
  if (len != sizeof(CarStatusPacket)) return;

  CarStatusPacket temp;
  memcpy(&temp, data, sizeof(temp));

  if (temp.type != TYPE_STATUS) return;

  memcpy(&carStatus, &temp, sizeof(carStatus));

  memcpy(carMac, srcMac, 6);
  carKnown = true;
  needAddCarPeer = true;

  lastCarStatusTime = millis();
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  handleEspNowReceive(info->src_addr, data, len);
}
#else
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  handleEspNowReceive(mac, data, len);
}
#endif

void broadcastStatusToBrowser() {
  bool carConnected = carKnown && (millis() - lastCarStatusTime < 1000);
  unsigned long age = carConnected ? millis() - lastCarStatusTime : 9999;

  String msg = "S,";
  msg += carConnected ? "1" : "0";
  msg += ",";
  msg += age;
  msg += ",";
  msg += carStatus.lastSeqReceived;
  msg += ",";
  msg += carStatus.targetMotor;
  msg += ",";
  msg += carStatus.currentMotor;
  msg += ",";
  msg += carStatus.steeringAngle;
  msg += ",";
  msg += carStatus.failsafeActive;

  webSocket.broadcastTXT(msg);
}

void handleRoot() {
  server.send_P(200, "text/html", htmlPage);
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(ssid, password, WIFI_CHANNEL);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println();
  Serial.println("ROCK RC Controller Started");
  Serial.print("WiFi name: ");
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);
  Serial.print("Open browser at: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("ESP-NOW channel: ");
  Serial.println(WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, broadcastAddress, 6);
  broadcastPeer.channel = WIFI_CHANNEL;
  broadcastPeer.encrypt = false;

  if (esp_now_add_peer(&broadcastPeer) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
    return;
  }

  server.on("/", handleRoot);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  lastPhoneCommand = millis();

  Serial.println("HTTP server started on port 80");
  Serial.println("WebSocket server started on port 81");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  addCarPeerIfNeeded();

  if (millis() - lastPhoneCommand > PHONE_TIMEOUT) {
    currentMotor = 0;
    currentSteer = 0;

    sendPacketToCar(
      0,
      0,
      currentServoOffset,
      currentMaxSpeed,
      currentRampStep
    );

    lastPhoneCommand = millis();
  }

  if (millis() - lastStatusWebSend > WEB_STATUS_INTERVAL) {
    lastStatusWebSend = millis();
    broadcastStatusToBrowser();
  }
}