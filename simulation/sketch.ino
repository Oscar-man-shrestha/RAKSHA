// RAKSHA simulator sketch for https://logicraftstudios.tech/editor/
// Open simulation/raksha-logicraft.zip in that editor, compile, then type RUN.
//
// Canvas stand-ins for parts the simulator does not include:
//   CARD button  -> RFID tap (start shift, or acknowledge an alarm)
//   sound pot    -> KY-037 analog envelope
//   buzzer       -> vibration motor on the same GPIO
//   no barometer -> heat vote is DHT22 against the NTC only

#include <Wire.h>
#include <math.h>
#include <DHTesp.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define PIN_DHT     4
#define PIN_SOUND   32
#define PIN_POT     33
#define PIN_PIR     34
#define PIN_NTC     36
#define PIN_VIBE    16
#define PIN_FAN     17
#define PIN_BEACON  26
#define PIN_LED_R   25
#define PIN_LED_G   13
#define PIN_LED_B   14
#define PIN_CARD    27

#define RELAY_OFF HIGH
#define RELAY_ON  LOW

#define OLED_W 128
#define OLED_H 64

DHTesp dht;
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

enum Mode { MODE_IDLE, MODE_SHIFT, MODE_ALERT, MODE_FAULT };
enum Band { BAND_NONE, BAND_CAUTION, BAND_EXTREME_CAUTION, BAND_DANGER, BAND_EXTREME };
enum Fall { FALL_IDLE, FALL_FREEFALL, FALL_IMPACT, FALL_CONFIRM, FALL_ALARM };
enum RunPhase {
  RUN_OFF, RUN_CARD, RUN_CLIMATE, RUN_CLAP, RUN_NOISE,
  RUN_SHAKE, RUN_DROP, RUN_ACK, RUN_UNPLUG, RUN_DONE
};

Mode mode = MODE_IDLE;
Band band = BAND_NONE;
Fall fall = FALL_IDLE;
bool heatValid = false;
bool tempFault = false;
bool airAlert = false;
bool present = false;
bool fanOn = false;
bool beaconOn = false;
bool dhtOk = false;
bool ntcOk = false;
bool mpuOk = false;
bool oledOk = false;
bool shiftOpen = false;
float tDht = NAN;
float rh = NAN;
float tNtc = NAN;
float heatC = NAN;
float soundEma = 0;
float riskScore = 0;
float riskTh = 57;
float ax = 0, ay = 0, az = 1;

bool lowSeen = false;
uint32_t lowStart = 0;
uint32_t freeStart = 0;
uint32_t impactStart = 0;
bool stillSeen = false;
uint32_t stillStart = 0;

bool cardDown = false;
uint32_t cardLockUntil = 0;

bool running = false;
RunPhase phase = RUN_OFF;
uint32_t runStart = 0;
bool dropActive = false;
uint32_t dropStart = 0;
bool shakeActive = false;
bool forceUnplug = false;
bool injectClimate = false;
float injT = 30, injRh = 55, injSound = 0.1;
bool injPresent = false;

bool vibeOn = false;
uint32_t lastFast = 0;
uint32_t lastSlow = 0;
uint32_t lastOled = 0;
uint32_t bootMs = 0;
uint32_t cautionUntil = 0;
Band lastBand = BAND_NONE;

const char *bandName(Band b) {
  switch (b) {
    case BAND_CAUTION: return "CAUTION";
    case BAND_EXTREME_CAUTION: return "EXTREME_CAUTION";
    case BAND_DANGER: return "DANGER";
    case BAND_EXTREME: return "EXTREME";
    default: return "NONE";
  }
}

const char *fallName(Fall f) {
  switch (f) {
    case FALL_FREEFALL: return "FREEFALL";
    case FALL_IMPACT: return "IMPACT";
    case FALL_CONFIRM: return "CONFIRM";
    case FALL_ALARM: return "ALARM";
    default: return "IDLE";
  }
}

float clamp01(float v) {
  if (v < 0) return 0;
  if (v > 1) return 1;
  return v;
}

float heatIndexC(float tc, float rhIn) {
  float tf = tc * 9.0f / 5.0f + 32.0f;
  float simple = 0.5f * (tf + 61.0f + (tf - 68.0f) * 1.2f + rhIn * 0.094f);
  float hiF;
  if ((simple + tf) / 2.0f < 80.0f) {
    hiF = simple;
  } else {
    hiF = -42.379f
      + 2.04901523f * tf
      + 10.14333127f * rhIn
      - 0.22475541f * tf * rhIn
      - 0.00683783f * tf * tf
      - 0.05481717f * rhIn * rhIn
      + 0.00122874f * tf * tf * rhIn
      + 0.00085282f * tf * rhIn * rhIn
      - 0.00000199f * tf * tf * rhIn * rhIn;
    if (rhIn < 13.0f && tf >= 80.0f && tf <= 112.0f) {
      hiF -= ((13.0f - rhIn) / 4.0f) * sqrtf((17.0f - fabsf(tf - 95.0f)) / 17.0f);
    }
    if (rhIn > 85.0f && tf >= 80.0f && tf <= 87.0f) {
      hiF += ((rhIn - 85.0f) / 10.0f) * ((87.0f - tf) / 5.0f);
    }
  }
  return (hiF - 32.0f) * 5.0f / 9.0f;
}

Band bandFromHeat(float hi) {
  if (hi < 27.0f) return BAND_NONE;
  if (hi < 32.0f) return BAND_CAUTION;
  if (hi < 39.0f) return BAND_EXTREME_CAUTION;
  if (hi <= 51.0f) return BAND_DANGER;
  return BAND_EXTREME;
}

float readNtcC() {
  int raw = analogRead(PIN_NTC);
  if (raw < 1 || raw > 4094) return NAN;
  return 1.0f / (logf(1.0f / (4095.0f / raw - 1.0f)) / 3950.0f + 1.0f / 298.15f) - 273.15f;
}

bool readMpu(float &x, float &y, float &z) {
  Wire.beginTransmission(0x68);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(0x68, (uint8_t)6) != 6) return false;
  int16_t rx = (Wire.read() << 8) | Wire.read();
  int16_t ry = (Wire.read() << 8) | Wire.read();
  int16_t rz = (Wire.read() << 8) | Wire.read();
  x = rx / 16384.0f;
  y = ry / 16384.0f;
  z = rz / 16384.0f;
  return true;
}

void setVibe(bool on) {
  if (on == vibeOn) return;
  vibeOn = on;
  if (on) {
    digitalWrite(PIN_VIBE, HIGH);
    tone(PIN_VIBE, 1800);
  } else {
    noTone(PIN_VIBE);
    digitalWrite(PIN_VIBE, LOW);
  }
}

void drawOled() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print("RAKSHA ");
  if (mode == MODE_FAULT) oled.println("FAULT");
  else if (mode == MODE_ALERT) oled.println("ALERT");
  else if (mode == MODE_SHIFT) oled.println("SHIFT");
  else oled.println("IDLE");

  oled.setCursor(0, 16);
  oled.println(shiftOpen ? "worker SIM-01" : "NO WORKER");

  oled.setCursor(0, 32);
  if (mode == MODE_FAULT) {
    oled.println(forceUnplug || !dhtOk ? "FAULT DHT" : "FAULT TEMP");
  } else if (heatValid) {
    oled.print("HI ");
    oled.print(heatC, 1);
    oled.print(" RISK ");
    oled.println(riskScore, 0);
  } else {
    oled.println("HI ---");
  }

  oled.setCursor(0, 48);
  oled.print(fanOn ? "FAN ON  " : "FAN OFF ");
  oled.println(fallName(fall));
  oled.display();
}

void printState(const char *step) {
  Serial.print("STATE ");
  Serial.print(step);
  Serial.print(" HI=");
  if (heatValid) Serial.print(heatC, 1);
  else Serial.print("-");
  Serial.print(" band=");
  Serial.print(bandName(band));
  Serial.print(" risk=");
  Serial.print(riskScore, 1);
  Serial.print(" air=");
  Serial.print(airAlert ? 1 : 0);
  Serial.print(" fan=");
  Serial.print(fanOn ? 1 : 0);
  Serial.print(" beacon=");
  Serial.print(beaconOn ? 1 : 0);
  Serial.print(" fall=");
  Serial.print(fallName(fall));
  Serial.print(" present=");
  Serial.println(present ? 1 : 0);

  Serial.print("CSV ");
  Serial.print(millis());
  Serial.print(",SIM-01,");
  Serial.print(dhtOk ? String(tDht, 1) : "-");
  Serial.print(",");
  Serial.print(dhtOk ? String(rh, 0) : "-");
  Serial.print(",");
  Serial.print(heatValid ? String(heatC, 1) : "-");
  Serial.print(",");
  Serial.print(ntcOk ? String(tNtc, 1) : "-");
  Serial.print(",-,");
  Serial.print(soundEma, 2);
  Serial.print(",");
  Serial.print(riskScore, 1);
  Serial.print(",");
  Serial.print(present ? 1 : 0);
  Serial.print(",");
  Serial.print(bandName(band));
  Serial.print(",");
  Serial.print(fallName(fall));
  Serial.print(",");
  Serial.print(fanOn ? 1 : 0);
  Serial.print(",");
  Serial.print(beaconOn ? 1 : 0);
  Serial.print(",");
  Serial.println(mode == MODE_FAULT ? "DHT" : "NONE");
}

void estimate() {
  if (dhtOk) {
    heatC = heatIndexC(tDht, rh);
    tempFault = ntcOk && fabsf(tDht - tNtc) > 8.0f;
    heatValid = !tempFault;
  } else {
    heatValid = false;
    tempFault = false;
    heatC = NAN;
  }

  float stag = 0;
  float couple = 0;
  if (dhtOk) stag = clamp01((rh - 65.0f) / 20.0f);
  if (heatValid) couple = clamp01((heatC - 27.0f) / 12.0f);
  riskScore = 100.0f * (0.50f * soundEma + 0.30f * stag + 0.20f * couple);

  if (!heatValid || !shiftOpen || !present) {
    band = BAND_NONE;
  } else {
    band = bandFromHeat(heatC);
  }
  if ((band == BAND_CAUTION || band == BAND_EXTREME_CAUTION) && band != lastBand) {
    cautionUntil = millis() + 100;
  }
  lastBand = band;
  airAlert = shiftOpen && present && heatValid && riskScore >= riskTh;
}

void applyOutputs() {
  if (mode == MODE_FAULT || !shiftOpen) {
    fanOn = false;
    beaconOn = false;
    band = (mode == MODE_FAULT) ? BAND_NONE : band;
    airAlert = false;
  } else {
    bool heatAct = band == BAND_DANGER || band == BAND_EXTREME;
    fanOn = heatAct || airAlert;
    beaconOn = fall == FALL_ALARM;
  }

  digitalWrite(PIN_FAN, fanOn ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_BEACON, beaconOn ? RELAY_ON : RELAY_OFF);

  bool alert = shiftOpen && (airAlert || band == BAND_DANGER || band == BAND_EXTREME || fall == FALL_ALARM);
  if (mode != MODE_FAULT) {
    if (!shiftOpen) mode = MODE_IDLE;
    else if (alert || fall == FALL_ALARM) mode = MODE_ALERT;
    else mode = MODE_SHIFT;
  }

  if (mode == MODE_FAULT) {
    bool blink = ((millis() / 250) % 2) == 0;
    digitalWrite(PIN_LED_R, blink ? HIGH : LOW);
    digitalWrite(PIN_LED_G, LOW);
    digitalWrite(PIN_LED_B, LOW);
    setVibe(false);
  } else if (fall == FALL_ALARM || band == BAND_DANGER || band == BAND_EXTREME || airAlert) {
    digitalWrite(PIN_LED_R, HIGH);
    digitalWrite(PIN_LED_G, LOW);
    digitalWrite(PIN_LED_B, LOW);
    if (fall == FALL_ALARM) setVibe(true);
    else setVibe(((millis() / 200) % 2) == 0);
  } else if (band == BAND_CAUTION || band == BAND_EXTREME_CAUTION) {
    digitalWrite(PIN_LED_R, HIGH);
    digitalWrite(PIN_LED_G, HIGH);
    digitalWrite(PIN_LED_B, LOW);
    setVibe(millis() < cautionUntil);
  } else if (shiftOpen) {
    digitalWrite(PIN_LED_R, LOW);
    digitalWrite(PIN_LED_G, HIGH);
    digitalWrite(PIN_LED_B, LOW);
    setVibe(false);
  } else {
    digitalWrite(PIN_LED_R, LOW);
    digitalWrite(PIN_LED_G, LOW);
    digitalWrite(PIN_LED_B, LOW);
    setVibe(false);
  }

  if (millis() - lastOled > 250) {
    lastOled = millis();
    drawOled();
  }
}

void setFall(Fall next) {
  if (next == fall) return;
  fall = next;
  Serial.print("FALL ");
  Serial.println(fallName(fall));
  if (next == FALL_IDLE) {
    lowSeen = false;
    stillSeen = false;
  }
}

void stepFall(uint32_t now) {
  if (mode == MODE_FAULT) return;
  float g = sqrtf(ax * ax + ay * ay + az * az);
  bool tiltOk = false;
  float tilt = 0;
  if (g > 0.7f && g < 1.3f) {
    float c = az / g;
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    tilt = acosf(c) * 57.2958f;
    tiltOk = true;
  }
  bool pirQuiet = !present;

  switch (fall) {
    case FALL_IDLE:
      if (g < 0.4f) {
        if (!lowSeen) {
          lowSeen = true;
          lowStart = now;
        } else if (now - lowStart >= 80) {
          setFall(FALL_FREEFALL);
          freeStart = now;
        }
      } else {
        lowSeen = false;
      }
      break;
    case FALL_FREEFALL:
      if (g > 2.5f) {
        setFall(FALL_IMPACT);
        impactStart = now;
        stillSeen = false;
      } else if (now - freeStart > 500) {
        setFall(FALL_IDLE);
      }
      break;
    case FALL_IMPACT:
      if (tiltOk && tilt > 60.0f && pirQuiet) {
        setFall(FALL_CONFIRM);
        stillStart = now;
        stillSeen = true;
      } else if (now - impactStart > 1500) {
        setFall(FALL_IDLE);
      }
      break;
    case FALL_CONFIRM:
      if (!(tiltOk && tilt > 60.0f && pirQuiet)) {
        setFall(FALL_IDLE);
      } else if (now - stillStart >= 2000) {
        setFall(FALL_ALARM);
      }
      break;
    case FALL_ALARM:
      break;
  }
}

void acknowledge() {
  if (fall == FALL_ALARM || fall == FALL_CONFIRM) {
    setFall(FALL_IDLE);
    beaconOn = false;
    digitalWrite(PIN_BEACON, RELAY_OFF);
    setVibe(false);
    Serial.println("ACK");
  }
}

void startShift() {
  shiftOpen = true;
  mode = MODE_SHIFT;
  Serial.println("SHIFT worker=SIM-01");
}

void onCard() {
  if (mode == MODE_FAULT) {
    Serial.println("CARD ignored fault");
    return;
  }
  if (fall == FALL_ALARM) {
    acknowledge();
    return;
  }
  if (!shiftOpen) startShift();
  else Serial.println("CARD shift already open");
}

void enterFault() {
  mode = MODE_FAULT;
  shiftOpen = false;
  airAlert = false;
  fanOn = false;
  beaconOn = false;
  heatValid = false;
  setFall(FALL_IDLE);
  digitalWrite(PIN_FAN, RELAY_OFF);
  digitalWrite(PIN_BEACON, RELAY_OFF);
  setVibe(false);
  Serial.println("FAULT DHT relays=OFF");
}

void applyClimate(float t, float h, float sound, bool person) {
  injectClimate = true;
  injT = t;
  injRh = h;
  injSound = sound;
  injPresent = person;
  forceUnplug = false;
}

void startRun() {
  running = true;
  runStart = millis();
  phase = RUN_CARD;
  dropActive = false;
  shakeActive = false;
  forceUnplug = false;
  injectClimate = false;
  mode = MODE_IDLE;
  shiftOpen = false;
  fall = FALL_IDLE;
  fanOn = false;
  beaconOn = false;
  riskTh = 57;
  digitalWrite(PIN_FAN, RELAY_OFF);
  digitalWrite(PIN_BEACON, RELAY_OFF);
  Serial.println();
  Serial.println("RUN start  threshold=57");
  Serial.println("RUN script card, caution, clap, noise, shake, drop, ack, unplug");
}

void fillInputs() {
  if (running && forceUnplug) {
    dhtOk = false;
    tDht = NAN;
    rh = NAN;
  } else if (running && injectClimate) {
    dhtOk = true;
    tDht = injT;
    rh = injRh;
    tNtc = injT;
    ntcOk = true;
    soundEma = injSound;
    present = injPresent;
  } else {
    float t = dht.getTemperature();
    float h = dht.getHumidity();
    dhtOk = !isnan(t) && !isnan(h);
    if (dhtOk) {
      tDht = t;
      rh = h;
    }
    float n = readNtcC();
    ntcOk = !isnan(n);
    if (ntcOk) tNtc = n;
    int sraw = analogRead(PIN_SOUND);
    float sample = sraw / 4095.0f;
    float alpha = 1.0f - expf(-2.0f / 10.0f);
    soundEma = alpha * sample + (1.0f - alpha) * soundEma;
    int praw = analogRead(PIN_POT);
    riskTh = 30.0f + (praw / 4095.0f) * 60.0f;
    present = digitalRead(PIN_PIR) == HIGH;
  }

  if (running && (shakeActive || dropActive)) {
    // Acceleration is injected below.
  } else if (!(running && injectClimate)) {
    float x, y, z;
    if (readMpu(x, y, z)) {
      ax = x;
      ay = y;
      az = z;
      mpuOk = true;
    }
  } else if (!shakeActive && !dropActive) {
    ax = 0;
    ay = 0;
    az = 1;
  }
}

void serviceDrop(uint32_t now) {
  if (!dropActive) return;
  uint32_t dt = now - dropStart;
  present = false;
  injPresent = false;
  if (dt < 100) {
    ax = 0;
    ay = 0;
    az = 0.15f;
  } else if (dt < 160) {
    ax = 0;
    ay = 0;
    az = 3.2f;
  } else {
    ax = 0.95f;
    ay = 0;
    az = 0.25f;
  }
}

void serviceShake(uint32_t now) {
  if (!shakeActive) return;
  float w = (now % 200) / 200.0f;
  ax = 0.45f;
  ay = 0.20f;
  az = 1.35f + w * 0.4f;
}

void scenario(uint32_t now) {
  uint32_t t = now - runStart;
  RunPhase next = phase;
  if (t >= 500 && phase == RUN_CARD) next = RUN_CLIMATE;
  else if (t >= 2500 && phase == RUN_CLIMATE) next = RUN_CLAP;
  else if (t >= 5000 && phase == RUN_CLAP) next = RUN_NOISE;
  else if (t >= 8000 && phase == RUN_NOISE) next = RUN_SHAKE;
  else if (t >= 10500 && phase == RUN_SHAKE) next = RUN_DROP;
  else if (t >= 15000 && phase == RUN_DROP) next = RUN_ACK;
  else if (t >= 17000 && phase == RUN_ACK) next = RUN_UNPLUG;
  else if (t >= 19000 && phase == RUN_UNPLUG) next = RUN_DONE;
  if (next == phase) return;

  if (phase == RUN_SHAKE && next != RUN_SHAKE) shakeActive = false;

  phase = next;
  if (phase == RUN_CLIMATE) {
    onCard();
    applyClimate(30, 55, 0.10f, true);
    Serial.println("STEP climate 30C 55% present sound=0.10");
  } else if (phase == RUN_CLAP) {
    applyClimate(28, 70, 0.12f, true);
    Serial.println("STEP clap sound=0.12 held briefly");
  } else if (phase == RUN_NOISE) {
    applyClimate(28, 70, 0.90f, true);
    Serial.println("STEP noise sound=0.90 settled envelope");
  } else if (phase == RUN_SHAKE) {
    shakeActive = true;
    Serial.println("STEP shake  (no free-fall, fall must stay IDLE)");
  } else if (phase == RUN_DROP) {
    shakeActive = false;
    dropActive = true;
    dropStart = now;
    present = false;
    Serial.println("STEP drop");
  } else if (phase == RUN_ACK) {
    dropActive = false;
    ax = 0;
    ay = 0;
    az = 1;
    onCard();
    Serial.println("STEP ack");
  } else if (phase == RUN_UNPLUG) {
    forceUnplug = true;
    injectClimate = false;
    dhtOk = false;
    enterFault();
    Serial.println("STEP unplug DHT");
  } else if (phase == RUN_DONE) {
    running = false;
    Serial.println("RUN done");
    printState("done");
  }
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;
  if (cmd == "RUN") {
    startRun();
    return;
  }
  if (cmd == "HELP") {
    Serial.println("Commands: RUN, CARD, DROP, SHAKE, UNPLUG, HELP");
    Serial.println("RUN plays the full scenario in about 20 seconds.");
    return;
  }
  if (cmd == "CARD") {
    onCard();
    return;
  }
  if (cmd == "DROP") {
    dropActive = true;
    dropStart = millis();
    present = false;
    Serial.println("DROP injected");
    return;
  }
  if (cmd == "SHAKE") {
    shakeActive = true;
    Serial.println("SHAKE on  (type RUN or reset to clear)");
    return;
  }
  if (cmd == "UNPLUG") {
    forceUnplug = true;
    enterFault();
    return;
  }
  Serial.println("Unknown command. Type HELP.");
}

void readSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  handleCommand(cmd);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_BEACON, OUTPUT);
  pinMode(PIN_VIBE, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_CARD, INPUT_PULLUP);
  pinMode(PIN_PIR, INPUT);
  digitalWrite(PIN_FAN, RELAY_OFF);
  digitalWrite(PIN_BEACON, RELAY_OFF);
  digitalWrite(PIN_VIBE, LOW);
  digitalWrite(PIN_LED_R, LOW);
  digitalWrite(PIN_LED_G, LOW);
  digitalWrite(PIN_LED_B, LOW);

  bootMs = millis();
  Wire.begin(21, 22);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOk) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("RAKSHA");
    oled.println("relays OFF");
    oled.println("self-test");
    oled.display();
  }

  Wire.beginTransmission(0x68);
  Wire.write(0x6B);
  Wire.write(0x00);
  mpuOk = Wire.endTransmission() == 0;

  dht.setup(PIN_DHT, DHTesp::DHT22);

  Serial.println("RAKSHA simulator");
  Serial.println("BOOT relays=OFF");
  Serial.print("BOOT oled=");
  Serial.println(oledOk ? "OK" : "FAIL");
  Serial.print("BOOT mpu=");
  Serial.println(mpuOk ? "OK" : "FAIL");
  Serial.println("Type RUN");
  drawOled();
}

void loop() {
  uint32_t now = millis();
  readSerial();
  if (running) scenario(now);

  bool pressed = digitalRead(PIN_CARD) == LOW;
  if (!running && pressed && !cardDown && now > cardLockUntil) {
    cardLockUntil = now + 300;
    onCard();
  }
  cardDown = pressed;

  if (now - lastFast >= 10) {
    lastFast = now;
    if (shakeActive) serviceShake(now);
    else if (dropActive) serviceDrop(now);
    else if (!(running && injectClimate)) {
      float x, y, z;
      if (readMpu(x, y, z)) {
        ax = x;
        ay = y;
        az = z;
        mpuOk = true;
      }
    } else {
      ax = 0;
      ay = 0;
      az = 1;
    }

    if (!shakeActive && !dropActive && now - lastSlow >= 2000) {
      lastSlow = now;
      fillInputs();
      if (!running && !dhtOk && now - bootMs > 4000 && shiftOpen && !forceUnplug) {
        enterFault();
      }
      estimate();
    }

    if (running && injectClimate && !forceUnplug) {
      tDht = injT;
      rh = injRh;
      tNtc = injT;
      dhtOk = true;
      ntcOk = true;
      soundEma = injSound;
      if (!dropActive) present = injPresent;
      estimate();
    }

    stepFall(now);
    applyOutputs();

    static RunPhase printed = RUN_OFF;
    if (running && phase != printed && phase != RUN_CARD) {
      printed = phase;
      if (phase != RUN_DONE) printState("step");
    }
    static uint32_t lastPrint = 0;
    if (!running && now - lastPrint > 2000 && shiftOpen && mode != MODE_FAULT) {
      lastPrint = now;
      printState("live");
    }
  }
}
