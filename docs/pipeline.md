# RAKSHA pipeline

This is the build specification. Each stage lists its inputs, its work, its outputs, and what it does when something is wrong. Implement stages in order. Do not add a behavior that has no stage here.

```text
  12 V adapter
       │
       ▼
  ┌─────────┐   measured 5.0 V
  │  POWER  │───────────────────────────── ESP32 VIN, relay VCC, motor supply
  └─────────┘
       │
       ▼
  ┌─────────┐   relays forced off, I2C scan, one DHT read, RFID check
  │  BOOT   │───────────────────────────── READY  or  FAULT (stay here)
  └─────────┘
       │ READY
       ▼
  ┌─────────┐   100 Hz IMU    2 s environment    edge RFID
  │  SENSE  │
  └─────────┘
       │ Sample
       ▼
  ┌──────────┐  heat index, temp vote, risk score, presence, |a|, tilt
  │ ESTIMATE │
  └──────────┘
       │ Features
       ▼
  ┌─────────┐   shift, heat band, air alert, fall state, fault
  │ DECIDE  │
  └─────────┘
       │ Decision
       ├──────────────────────┐
       ▼                      ▼
  ┌─────────┐            ┌─────────┐
  │   ACT   │            │ RECORD  │  CSV every 15 s and on every edge
  └─────────┘            └─────────┘
   OLED, RGB,                 │
   vibration,                 ▼
   fan, beacon           ┌─────────┐
                         │  SERVE  │  dashboard on AP or Wi-Fi
                         └─────────┘
                          GET /api/state
                          POST /api/ack | /fan | /threshold
```

## Shared records

Every stage after Sense reads or writes these structs. Names here are the names to use in code.

### Sample (Sense output)

| Field | Unit | Source | Cadence |
|---|---|---|---|
| `t_dht` | °C | DHT22 | 2 s |
| `rh` | % | DHT22 | 2 s |
| `t_ntc` | °C | NTC divider on GPIO 36 | 2 s |
| `t_baro` | °C | Barometer | 2 s |
| `pressure_hpa` | hPa | Barometer | 2 s |
| `sound_raw` | ADC counts | KY-037 analog on GPIO 32 | 2 s, average of 20 reads |
| `pot_raw` | ADC counts | Potentiometer on GPIO 33 | 2 s |
| `pir` | bool | PIR after 74HC14 | 2 s |
| `ir_near` | bool | IR proximity | 2 s |
| `ax, ay, az` | g | MPU6050 at ±2 g | 10 ms |
| `rfid_uid` | 8 hex chars or empty | RC522 | on card present |
| `dht_ok, mpu_ok, baro_ok, ntc_ok` | bool | driver | each read |

### Features (Estimate output)

| Field | Unit | Meaning |
|---|---|---|
| `heat_index_c` | °C | Rothfusz heat index, valid only if `heat_valid` |
| `heat_valid` | bool | DHT good and temperature vote agrees |
| `temp_fault` | bool | DHT disagrees with both other thermometers by more than 8 °C |
| `sound_ema` | 0–1 | 10 s envelope of noise |
| `risk_score` | 0–100 | Process-risk score |
| `risk_threshold` | 30–90 | Potentiometer mapped into this range |
| `present` | bool | PIR or IR |
| `accel_g` | g | Vector magnitude |
| `tilt_deg` | degrees from upright | Used only when magnitude is near 1 g |
| `freefall` | bool | Magnitude under 0.4 g |

### Decision (Decide output)

| Field | Values |
|---|---|
| `mode` | `BOOT`, `FAULT`, `IDLE`, `SHIFT`, `ALERT` |
| `worker_id` | UID or empty |
| `heat_band` | `NONE`, `CAUTION`, `EXTREME_CAUTION`, `DANGER`, `EXTREME` |
| `air_alert` | bool |
| `fall_state` | `IDLE`, `FREEFALL`, `IMPACT`, `CONFIRM`, `ALARM` |
| `fan_on` | bool |
| `beacon_on` | bool |
| `exposure_s` | seconds in caution or worse, this shift |
| `danger_s` | seconds in danger or extreme, this shift |
| `fault_reason` | `NONE`, `DHT`, `MPU`, `I2C`, `TEMP_VOTE`, `RFID` |

### Log row (Record output)

One CSV line, fixed column order:

```text
ms,worker_id,t_dht,rh,heat_index_c,t_ntc,t_baro,sound_ema,risk_score,present,heat_band,fall_state,fan_on,beacon_on,fault_reason
```

Write a row every 15 s while a shift is open, and immediately on any edge: shift start, shift end, band change, fall alarm, acknowledge, fan change, fault.

---

## Stage 1 — Power

**Parts.** 12 V adapter, HW-411 buck, ESP32 VIN, relay module VCC, vibration-motor supply.

**Work.**

1. Unload the buck. Set it to 5.0 V. Measure with a multimeter and write the number down.
2. Feed ESP32 VIN, the relay board VCC, and the motor collector supply from that 5 V rail.
3. Grounds meet at one point: buck ground, ESP32 GND, relay GND, motor emitter GND.

**Output.** A stable 5.0 V rail. Photograph the meter.

**Failure.** If the buck is outside 4.8–5.2 V, stop. Do not plug in the ESP32.

The ESP32 3.3 V pin does not power the motor or the relays.

---

## Stage 2 — Boot

**Input.** Power applied, or a watchdog reset.

**Work, in this order.**

1. Set both relay GPIOs to the inactive level before any other call. Default is active-low, so write them HIGH. External pull-ups hold that level while the pins are still floating.
2. Start the task watchdog.
3. I2C scan on GPIO 21/22. Require OLED `0x3C` and MPU6050 `0x68`. Accept barometer at `0x76` or `0x77`.
4. Read DHT22 once.
5. Init RC522. A missing reader is a fault. A missing card is not.
6. Show the result on the OLED for 1 s. Cycle RGB once so the demo can show the LED works.

**Output.** `mode = IDLE` when OLED, MPU, and DHT all answered. Otherwise `mode = FAULT` and `fault_reason` names the first failure.

**Failure.** Fault mode has one behavior: relays off, red blink, OLED text, no shift, no exposure. The only exit is a reset that passes self-test.

---

## Stage 3 — Sense

Two clocks. The fall path never waits on the DHT22.

### Fast clock, every 10 ms

Read MPU6050 accelerometer. Full-scale ±2 g, so LSB sensitivity is 16384 counts per g.

```text
ax_g = ax_raw / 16384
ay_g = ay_raw / 16384
az_g = az_raw / 16384
```

Mount the board with **+Z up**. Write that on the enclosure.

### Slow clock, every 2 s

| Reading | How |
|---|---|
| DHT22 | Single-wire on GPIO 4, 10 kΩ to 3.3 V. A NaN or timeout sets `dht_ok = false` and keeps the previous sample out of Estimate. |
| NTC | Divider: 10 kΩ from 3.3 V to the ADC node, NTC from the node to GND, node on GPIO 36. |
| Barometer | Temperature and pressure over I2C. Pressure is logged. It does not enter the alert. |
| KY-037 | 20 analog reads on GPIO 32, averaged. |
| Pot | One read on GPIO 33. |
| PIR | Digital on GPIO 34, after the 74HC14. |
| IR | Digital on GPIO 35. |

Active levels of PIR and IR are `PIR_ACTIVE_HIGH` and `IR_ACTIVE_HIGH` in `config.h`. Confirm them once with a hand and then leave them.

### RFID

RC522 on SPI: SCK 18, MISO 19, MOSI 23, SS 5, RST 27. A new UID is latched for Decide and then cleared. Debounce card presence by 300 ms so one tap is one event.

### Analog rule

Wi-Fi uses ADC2. Every analog pin in this design is ADC1: GPIO 32, 33, and 36.

### NTC conversion

```text
v = (ntc_raw / 4095) * 3.3
r = 10000 * v / (3.3 - v)
t_kelvin = 1 / (1/298.15 + ln(r / 10000) / 3950)
t_ntc = t_kelvin - 273.15
```

This assumes a 10 kΩ NTC, β = 3950. At room temperature, measure the divider with the multimeter and record ADC counts, resistance, and a reference thermometer. If the beta of the part in the kit is different, change `NTC_BETA` and `NTC_R0` in `config.h` only.

### Schmitt

74HC14 powered from **3.3 V**, not 5 V. A 5 V logic-high into an ESP32 GPIO damages the pin. One gate cleans PIR, one gate cleans the KY-037 digital output. Firmware uses the analog pin for the score. The digital path is there so a scope or a meter can show a clean edge for the hardware score.

---

## Stage 4 — Estimate

Pure functions. No GPIO writes here.

### Heat index

Convert DHT temperature to °F: `tf = t_dht * 9/5 + 32`.

Screening value:

```text
hi_simple = 0.5 * (tf + 61 + (tf - 68) * 1.2 + rh * 0.094)
```

If `(hi_simple + tf) / 2` is below 80 °F, use `hi_simple`. Otherwise use Rothfusz, with `tf` in °F and `rh` in percent:

```text
hi = -42.379
   + 2.04901523 * tf
   + 10.14333127 * rh
   - 0.22475541 * tf * rh
   - 0.00683783 * tf * tf
   - 0.05481717 * rh * rh
   + 0.00122874 * tf * tf * rh
   + 0.00085282 * tf * rh * rh
   - 0.00000199 * tf * tf * rh * rh
```

Corrections, applied only inside Rothfusz:

- Relative humidity under 13% and temperature from 80 °F to 112 °F: subtract `((13 - rh) / 4) * sqrt((17 - abs(tf - 95)) / 17)`.
- Relative humidity above 85% and temperature from 80 °F to 87 °F: add `((rh - 85) / 10) * ((87 - tf) / 5)`.

Convert back to °C for the rest of the pipeline: `heat_index_c = (hi - 32) * 5/9`.

### Temperature vote

```text
dht_vs_ntc  = abs(t_dht - t_ntc)
dht_vs_baro = abs(t_dht - t_baro)
temp_fault  = dht_ok and ntc_ok and baro_ok
              and dht_vs_ntc > 8 and dht_vs_baro > 8
heat_valid  = dht_ok and not temp_fault
```

A single disagreeing sensor does not fault the node. DHT must miss **both** other thermometers. If the barometer is absent, compare DHT with NTC only, threshold still 8 °C.

### Sound envelope

On each slow sample, with `tau = 10` seconds and `dt = 2`:

```text
alpha = 1 - exp(-dt / tau)
sound_ema = alpha * (sound_raw / 4095) + (1 - alpha) * sound_ema
```

A clap moves `sound_ema` a little and it decays. Grinding or a kiln blower holds it up.

### Process-risk score

```text
stagnation  = clamp((rh - 65) / 20, 0, 1)
heat_couple = clamp((heat_index_c - 27) / 12, 0, 1)    if heat_valid else 0
risk_score  = 100 * (0.5 * sound_ema + 0.3 * stagnation + 0.2 * heat_couple)
risk_threshold = 30 + (pot_raw / 4095) * 60            range 30 to 90
```

Weights are constants in `config.h`: `RISK_W_SOUND 0.50`, `RISK_W_STAG 0.30`, `RISK_W_HEAT 0.20`.

What to say in the pitch: this score is process risk. Sustained shop noise stands in for fume-generating work, humidity stands in for a room that is not clearing, and the score is gated by presence in Decide. It is not a ppm reading.

### Presence and tilt

```text
present = (pir == PIR_ACTIVE_HIGH) or (ir_near == IR_ACTIVE_HIGH)
accel_g = sqrt(ax_g^2 + ay_g^2 + az_g^2)
```

Tilt is computed only when `accel_g` is between 0.7 g and 1.3 g, so an impact spike cannot look like a posture change:

```text
tilt_deg = acos(clamp(az_g / accel_g, -1, 1)) * (180 / pi)
```

`freefall` is `accel_g < 0.4`.

---

## Stage 5 — Decide

Decide owns the only state in the firmware, apart from the CSV file.

### Shift

| Event | From | To | Effect |
|---|---|---|---|
| Known UID, no shift | `IDLE` | `SHIFT` | Store `worker_id`, zero `exposure_s` and `danger_s` |
| Unknown UID | any | unchanged | OLED `UNKNOWN CARD` for 1 s |
| Known UID while `fall_state = ALARM` or heat/air alert showing | `ALERT` | `SHIFT` | Acknowledge: beacon off, vibration off, event logged |
| Known UID while `present` has been false for 60 s | `SHIFT` | `IDLE` | Close the shift, write the final row |
| `present` becomes false | `SHIFT` | `SHIFT` | Start a 60 s pause timer |
| Pause timer hits 60 s | `SHIFT` | `SHIFT` | Stop adding exposure. Do not clear the counters. |
| `present` returns | `SHIFT` | `SHIFT` | Resume counters |

Known UIDs live in `config.h` as a list. Add the demo cards before the pitch.

### Heat band

Applied only when `heat_valid` and `present` and `mode` is `SHIFT` or `ALERT`.

| `heat_index_c` | Band | Counts |
|---|---|---|
| below 27 | `NONE` | no |
| 27 to 32 | `CAUTION` | `exposure_s` |
| 32 to 39 | `EXTREME_CAUTION` | `exposure_s` |
| 39 to 51 | `DANGER` | `exposure_s` and `danger_s` |
| above 51 | `EXTREME` | `exposure_s` and `danger_s` |

Counters add 2 seconds per slow tick, which is the sample period, and only while `present` is true and the pause timer is clear.

### Air alert

```text
air_alert = present and heat_valid and risk_score >= risk_threshold
```

`heat_valid` is required so a dead DHT cannot silently certify the air score.

### Fall machine

Runs on the 10 ms clock. Thresholds:

| Constant | Value | Role |
|---|---|---|
| `FALL_G_LOW` | 0.4 g | Free-fall |
| `FALL_LOW_MS` | 80 ms | How long free-fall must last |
| `FALL_G_HIGH` | 2.5 g | Impact |
| `FALL_IMPACT_MS` | 500 ms | Impact must arrive within this window |
| `FALL_TILT_DEG` | 60° | Posture after impact |
| `FALL_STILL_MS` | 2000 ms | Tilt holds and PIR stays quiet |

```text
IDLE
  │ accel_g < 0.4 g for 80 ms
  ▼
FREEFALL
  │ accel_g > 2.5 g within 500 ms        timeout → IDLE
  ▼
IMPACT
  │ tilt > 60° and PIR quiet for 2000 ms  timeout or worker moves → IDLE
  ▼
CONFIRM
  │ condition held
  ▼
ALARM
```

`ALARM` latches until a second RFID tap from the worker who owns the shift, or until fault mode.

Kiln vibration stays in `IDLE` because it never produces an 80 ms free-fall. That gate is the whole rejection. Do not add a gyro check and do not enable the MPU DMP.

### Outputs of Decide toward Act

| Condition | `fan_on` | `beacon_on` | Vibration pattern |
|---|---|---|---|
| `FAULT` or not `heat_valid` during a shift | false | false | off, RGB blinks red |
| `fall_state = ALARM` | unchanged | true | continuous |
| Band `DANGER` or `EXTREME`, or `air_alert` | true | false | repeat 200 ms on / 200 ms off |
| Band `CAUTION` or `EXTREME_CAUTION` | false | false | one 100 ms pulse when the band is entered |
| Otherwise | false | false | off |

Fan and beacon never turn on in `IDLE` or `FAULT`.

### Command input from Serve

Serve may submit three commands. Decide accepts them only with the PIN in `config.h`.

| Command | Effect |
|---|---|
| `ack` | Same as a second RFID tap |
| `fan` on or off | Manual override while a shift is open. Fault clears the override. |
| `threshold` | Replaces the pot-derived threshold until reboot. Clamped to 30–90. |

A wrong PIN returns `401` and changes nothing.

---

## Stage 6 — Act

Act is the only stage that writes GPIO, other than Boot.

| Output | Drive | On | Off |
|---|---|---|---|
| RGB | GPIO 25, 13, 14 through 220 Ω | green idle, amber caution, red danger or fall | all low in fault blink |
| Vibration | GPIO 16 → 1 kΩ → 2N2222A base. Motor from 5 V, emitter to GND. 1N4007 across the motor, cathode at 5 V | pattern from Decide | base low |
| Fan relay | GPIO 17 | active level from `RELAY_ACTIVE_LOW` | inactive, and inactive in fault |
| Beacon relay | GPIO 26 | on fall alarm | inactive in fault and after ack |
| OLED | I2C `0x3C` | see frame below | — |

OLED frame, four lines:

```text
RAKSHA  <mode>
<worker_id or NO WORKER>
HI 34.2  RISK 61
FAN OFF  FALL IDLE
```

In fault, line 3 is `FAULT <reason>` and line 4 is blank.

Refresh the OLED on the slow clock and immediately on a fall alarm. Fall detection itself does not wait for the display.

---

## Stage 7 — Record

**Store.** LittleFS, file `/shift.csv`. Create the file with the header row at boot if it is missing.

**When.**

- Every 15 s during `SHIFT` or `ALERT`.
- On every edge listed under the log-row section.

**Failure.** If the append fails, set a `log_warn` flag, show `LOG FULL` on the OLED for one frame, and keep alarming. Logging must not block the fall path. A full filesystem is not a reason to stop the fan.

The file remains readable after Wi-Fi drops. Serve exposes the last 20 rows.

---

## Stage 8 — Serve

**Bring-up.**

1. Try the STA SSID in `config.h` for 10 s.
2. On failure, start an access point named `RAKSHA`, password in `config.h`, address `192.168.4.1`.

The demo uses the access point so a dead campus network is not a failed pitch.

### `GET /api/state`

Returns the current Decision, Features, and the last 20 log rows as JSON. No PIN.

```json
{
  "mode": "SHIFT",
  "worker_id": "A1B2C3D4",
  "heat_index_c": 34.2,
  "heat_valid": true,
  "heat_band": "EXTREME_CAUTION",
  "risk_score": 61,
  "risk_threshold": 55,
  "present": true,
  "fall_state": "IDLE",
  "fan_on": false,
  "beacon_on": false,
  "exposure_s": 480,
  "danger_s": 0,
  "fault_reason": "NONE"
}
```

### `POST /api/ack`, `POST /api/fan`, `POST /api/threshold`

Body includes `pin`. Fan body also includes `on`. Threshold body also includes `value`. Wrong PIN: HTTP 401 and no state change.

### Page

One page at `/`. It polls `/api/state` every 2 s and shows worker, heat index, risk score, fall state, fan, beacon, and exposure minutes. Three buttons send the three commands. The page is stored in the sketch as a raw string so the node works with no internet.

### Offline

Unplugging the router, or never joining STA, leaves Act and Record running. The access point still serves the page. RFID acknowledge still works because it does not pass through Serve.

---

## `config.h` contract

Single file. No threshold copied into a `.cpp` file.

```text
WIFI_SSID, WIFI_PASS, AP_PASS, CMD_PIN

PIN_SDA 21, PIN_SCL 22
PIN_DHT 4
PIN_SOUND 32, PIN_POT 33, PIN_NTC 36
PIN_PIR 34, PIN_IR 35
PIN_VIBE 16, PIN_FAN 17, PIN_BEACON 26
PIN_LED_R 25, PIN_LED_G 13, PIN_LED_B 14
PIN_RFID_SS 5, PIN_RFID_RST 27
RFID SPI default: SCK 18, MISO 19, MOSI 23

I2C_OLED 0x3C, I2C_MPU 0x68, I2C_BARO 0x76

PIR_ACTIVE_HIGH 1, IR_ACTIVE_HIGH 1, RELAY_ACTIVE_LOW 1

NTC_R_FIXED 10000, NTC_R0 10000, NTC_BETA 3950
TEMP_VOTE_C 8

HEAT_CAUTION_C 27, HEAT_EXTREME_CAUTION_C 32
HEAT_DANGER_C 39, HEAT_EXTREME_C 51

RISK_W_SOUND 0.50, RISK_W_STAG 0.30, RISK_W_HEAT 0.20
RISK_TAU_S 10, RISK_TH_MIN 30, RISK_TH_MAX 90

FALL_G_LOW 0.4, FALL_LOW_MS 80
FALL_G_HIGH 2.5, FALL_IMPACT_MS 500
FALL_TILT_DEG 60, FALL_STILL_MS 2000

PRESENCE_PAUSE_MS 60000
LOG_PERIOD_MS 15000
SLOW_PERIOD_MS 2000
FAST_PERIOD_MS 10
WIFI_STA_TIMEOUT_MS 10000

KNOWN_UIDS { "A1B2C3D4" }
```

## Pin map

| GPIO | Function | Electrical note |
|---|---|---|
| 21 / 22 | I2C SDA / SCL | OLED, MPU6050, barometer |
| 18, 19, 23, 5, 27 | SPI SCK, MISO, MOSI, SS, RST | RC522 |
| 4 | DHT22 | 10 kΩ to 3.3 V |
| 32 | KY-037 analog | ADC1 |
| 33 | Potentiometer | ADC1, wiper. Ends at 3.3 V and GND |
| 34 | PIR via 74HC14 | Input only |
| 35 | IR proximity | Input only |
| 36 | NTC divider | Input only, ADC1 |
| 16 | Vibration transistor base | 1 kΩ series |
| 17 | Fan relay input | Pull-up to the inactive level |
| 26 | Beacon relay input | Pull-up to the inactive level |
| 25, 13, 14 | RGB R, G, B | 220 Ω, common cathode |

## Timing budget

| Path | Period | Budget |
|---|---|---|
| Fall read and state step | 10 ms | Under 2 ms. No DHT, no flash, no HTTP. |
| Environment and Decide slow half | 2 s | DHT22 needs up to 2 s on its own. |
| OLED and relays | On change, and every 2 s | Relays update in the same slow tick as the decision. Fall beacon updates inside the fast tick. |
| CSV | 15 s and edges | Append. Do not rewrite the file. |
| HTTP | On request | Handled outside the fast tick. |

The 10 ms path is what “responsiveness” means for the fall objective. A blocked DHT read must not sit inside it.

## Measured evidence to keep

Write these four numbers into the pitch slide before the demo.

| Measurement | Pass |
|---|---|
| Buck output, unloaded then with ESP32 connected | 4.8–5.2 V both times |
| NTC at room vs warm hand | Warm hand reads higher °C, both recorded |
| Fall trial | Table shake for 10 s stays `IDLE`. Foam drop from ~30 cm reaches `ALARM`. |
| Relay at reset | Both relays off while the OLED is still on the self-test line |
