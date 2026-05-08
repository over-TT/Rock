// ROCK RC - Car firmware (ESP32)
// Receives ESP-NOW control packets, drives the ESC and steering servo,
// and reports status back to the controller. Failsafe stops the car if
// packets stop arriving.
// Licensed under the MIT License. See LICENSE at the repository root.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <ESP32Servo.h>

Servo esc;
Servo steeringServo;

// ===== PINS =====
// These pins are for the board used in this build.
// Change them if using a different ESP32 board.
// Always check the actual GPIO pinout before wiring.
const int ESC_PIN = 9;
const int SERVO_PIN = 10;

// ===== ESP-NOW SETTINGS =====
const int WIFI_CHANNEL = 6;

// ===== ESP-NOW TYPES =====
const uint8_t TYPE_CONTROL = 1;
const uint8_t TYPE_STATUS  = 2;

// ===== ESC SETTINGS =====
const int ESC_MIN_ATTACH = 1000;
const int ESC_MAX_ATTACH = 2000;

const int ESC_FORWARD = 1200;
const int ESC_NEUTRAL = 1500;
const int ESC_REVERSE = 1700;

// If motor direction is backwards, swap ESC_FORWARD and ESC_REVERSE.

// ===== SERVO SETTINGS =====
const int SERVO_MIN_ATTACH = 500;
const int SERVO_MAX_ATTACH = 2500;

const int SERVO_LEFT = 120;
const int SERVO_CENTER = 90;
const int SERVO_RIGHT = 60;

// ===== SAFETY =====
unsigned long lastCommandTime = 0;
const unsigned long FAILSAFE_TIME = 500;

// ===== MOTOR RAMPING =====
const unsigned long RAMP_INTERVAL = 20;
unsigned long lastRampTime = 0;

// ===== STATUS SENDING =====
const unsigned long STATUS_INTERVAL = 200;
unsigned long lastStatusSend = 0;

// Control values
int targetMotor = 0;
int currentMotor = 0;
int targetSteer = 0;

int servoOffset = 0;
int maxSpeed = 60;
int rampStep = 4;

int currentThrottlePulse = ESC_NEUTRAL;
int currentSteeringAngle = SERVO_CENTER;

bool failsafeActive = false;

uint8_t controllerMac[6];
bool controllerKnown = false;
bool controllerPeerAdded = false;
bool needAddControllerPeer = false;

uint32_t lastSeqReceived = 0;

// ===== CONTROL PACKET =====
// Must match controller code EXACTLY
typedef struct __attribute__((packed)) {
  uint8_t type;
  int16_t motor;
  int16_t steer;
  int16_t servoOffset;
  uint8_t maxSpeed;
  uint8_t rampStep;
  uint32_t seq;
} ControlPacket;

// ===== STATUS PACKET =====
// Must match controller code EXACTLY
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

ControlPacket incoming;
CarStatusPacket statusPacket;

void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

void addControllerPeerIfNeeded() {
  if (!controllerKnown) return;
  if (!needAddControllerPeer && controllerPeerAdded) return;

  if (esp_now_is_peer_exist(controllerMac)) {
    controllerPeerAdded = true;
    needAddControllerPeer = false;
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, controllerMac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    controllerPeerAdded = true;
    needAddControllerPeer = false;

    Serial.print("Controller paired: ");
    printMac(controllerMac);
    Serial.println();
  } else {
    Serial.println("Failed to add controller peer");
  }
}

void sendStatusToController() {
  if (!controllerPeerAdded) return;

  statusPacket.type = TYPE_STATUS;
  statusPacket.lastSeqReceived = lastSeqReceived;
  statusPacket.targetMotor = targetMotor;
  statusPacket.currentMotor = currentMotor;
  statusPacket.steeringAngle = currentSteeringAngle;
  statusPacket.maxSpeed = maxSpeed;
  statusPacket.rampStep = rampStep;
  statusPacket.failsafeActive = failsafeActive ? 1 : 0;

  esp_now_send(controllerMac, (uint8_t*)&statusPacket, sizeof(statusPacket));
}

void applyMotorPulseFromValue(int motorValue) {
  motorValue = constrain(motorValue, -100, 100);

  int pulse;

  if (abs(motorValue) < 5) {
    pulse = ESC_NEUTRAL;
  } else if (motorValue > 0) {
    pulse = map(motorValue, 0, 100, ESC_NEUTRAL, ESC_FORWARD);
  } else {
    pulse = map(motorValue, 0, -100, ESC_NEUTRAL, ESC_REVERSE);
  }

  pulse = constrain(pulse, ESC_MIN_ATTACH, ESC_MAX_ATTACH);

  currentThrottlePulse = pulse;
  esc.writeMicroseconds(currentThrottlePulse);
}

void applySteering(int steerValue) {
  steerValue = constrain(steerValue, -100, 100);

  int angle;

  if (abs(steerValue) < 5) {
    angle = SERVO_CENTER;
  } else {
    angle = map(steerValue, -100, 100, SERVO_LEFT, SERVO_RIGHT);
  }

  angle += servoOffset;
  angle = constrain(angle, 0, 180);

  currentSteeringAngle = angle;
  steeringServo.write(currentSteeringAngle);
}

void updateMotorRamp() {
  if (millis() - lastRampTime < RAMP_INTERVAL) return;
  lastRampTime = millis();

  int difference = targetMotor - currentMotor;

  if (difference == 0) {
    applyMotorPulseFromValue(currentMotor);
    return;
  }

  int step = rampStep;

  // Symmetric ramping:
  // forward, reverse, speeding up, slowing down all use same step.
  if (abs(difference) <= step) {
    currentMotor = targetMotor;
  } else {
    if (difference > 0) {
      currentMotor += step;
    } else {
      currentMotor -= step;
    }
  }

  applyMotorPulseFromValue(currentMotor);
}

void stopCarHard() {
  targetMotor = 0;
  currentMotor = 0;
  targetSteer = 0;

  currentThrottlePulse = ESC_NEUTRAL;
  currentSteeringAngle = SERVO_CENTER + servoOffset;
  currentSteeringAngle = constrain(currentSteeringAngle, 0, 180);

  esc.writeMicroseconds(currentThrottlePulse);
  steeringServo.write(currentSteeringAngle);
}

void handleEspNowReceive(const uint8_t *srcMac, const uint8_t *data, int len) {
  if (len != sizeof(ControlPacket)) return;

  ControlPacket temp;
  memcpy(&temp, data, sizeof(temp));

  if (temp.type != TYPE_CONTROL) return;

  memcpy(&incoming, &temp, sizeof(incoming));

  memcpy(controllerMac, srcMac, 6);
  controllerKnown = true;
  needAddControllerPeer = true;

  int receivedMotor = constrain(incoming.motor, -100, 100);
  int receivedSteer = constrain(incoming.steer, -100, 100);

  servoOffset = constrain(incoming.servoOffset, -30, 30);
  maxSpeed = constrain(incoming.maxSpeed, 10, 100);
  rampStep = constrain(incoming.rampStep, 1, 25);

  targetMotor = (receivedMotor * maxSpeed) / 100;

  if (abs(receivedMotor) < 5) {
    targetMotor = 0;
  }

  targetSteer = receivedSteer;
  lastSeqReceived = incoming.seq;

  applySteering(targetSteer);

  lastCommandTime = millis();
  failsafeActive = false;
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

void setup() {
  Serial.begin(115200);

  // ===== START SAFE =====
  esc.setPeriodHertz(50);
  esc.attach(ESC_PIN, ESC_MIN_ATTACH, ESC_MAX_ATTACH);
  esc.writeMicroseconds(ESC_NEUTRAL);

  steeringServo.setPeriodHertz(50);
  steeringServo.attach(SERVO_PIN, SERVO_MIN_ATTACH, SERVO_MAX_ATTACH);
  steeringServo.write(SERVO_CENTER);

  Serial.println("Starting ESC at neutral 1500 us...");
  Serial.println("Waiting 5 seconds for ESC to arm...");
  delay(5000);

  // ===== ESP-NOW SETUP =====
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println();
  Serial.println("RC Car ESP-NOW Receiver");
  Serial.print("Car MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP-NOW channel: ");
  Serial.println(WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  lastCommandTime = millis();
  lastRampTime = millis();
  lastStatusSend = millis();

  Serial.println("Receiver ready.");
}

void loop() {
  addControllerPeerIfNeeded();

  if (millis() - lastCommandTime > FAILSAFE_TIME) {
    if (!failsafeActive) {
      stopCarHard();
      failsafeActive = true;
    }
  } else {
    updateMotorRamp();
  }

  if (millis() - lastStatusSend > STATUS_INTERVAL) {
    lastStatusSend = millis();
    sendStatusToController();
  }
}