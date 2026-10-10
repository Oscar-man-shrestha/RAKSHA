# RAKSHA

## Heat and air-safety monitor for informal workers

**SDG 8 — Decent Work and Economic Growth. Problem 8-A.**

RAKSHA is a workstation node for one bench in a brick kiln, a tailoring unit, a welding shop, or a small construction site. It measures heat stress, a process-air risk, and falls. It warns the person who is standing there, and it writes down how long that named person was in those conditions.

This report is the description of the system: the problem, the stack, the architecture, the pipeline, the dashboard, and the results of the scripted run.

![Boot on the simulator. OLED reads RAKSHA IDLE, NO WORKER, HI ---, FAN OFF IDLE.](figures/simulation-idle.png)

---

## 1. Problem statement

Informal and small-workshop workers spend full shifts in heat, dust, and fumes. There is no instrument on the bench, so a dangerous hour is noticed only after someone is already unwell. The employer has no record of which worker was present, for how long, and in what conditions.

A useful answer has to be affordable, has to warn the worker without asking them to look at a phone, and has to keep a record that an empty room cannot fake.

The objectives are:

1. Continuously monitor temperature, humidity, and a usable air-risk signal with the parts in the kit.
2. Detect a sudden fall or an abnormal movement with the MPU6050.
3. Warn immediately, with a signal the worker can feel and one the room can see.
4. Record exposure duration and the environmental conditions against a worker identity.
5. Stay affordable, and keep working when the workshop network is down.

## 2. Solution

RAKSHA sits on the bench. A known RFID card opens the shift. While a person is in front of the sensors, the node computes a heat index and a process-risk score, and it watches the accelerometer for a fall. The OLED, the RGB LED, and a vibration motor tell the worker. A fan relay and a beacon relay act on the room. A CSV file on the ESP32 is the exposure record. A phone on the node’s own access point shows the same shift.

Three rules keep the record honest.

- **Presence gate.** Exposure seconds increment only while the PIR or the IR sensor sees a person. An empty bench adds nothing.
- **Free-fall gate.** A fall is a short free-fall, then an impact, then a still posture. Vibration from the kiln never produces the free-fall dip, so a shake stays idle.
- **Relays fail off.** Both relays are off at boot and off on a sensor fault. The fan closes on danger heat or on process risk, and only while someone is present. The beacon closes on a latched fall.

The kit has no gas or dust sensor. The air result is a process-risk score made from sustained noise, humidity that is not clearing, and heat. It is not a concentration in ppm. The KY-037 is a sound sensor used as a proxy for a loud, stagnant process, not as an air-quality meter.

## 3. Tech stack

### Hardware

| Function | Part | Note |
|---|---|---|
| Controller | ESP32 DevKit V1 | Arduino C++. Analog only on ADC1, because Wi-Fi uses ADC2 |
| Heat and humidity | DHT22 | GPIO 4, 10 kΩ to 3.3 V |
| Second thermometer | NTC, 10 kΩ, β 3950 | Divider on GPIO 36. Used in the temperature vote |
| Third thermometer | Barometer module | I2C. Its temperature votes. Pressure is logged and does not alert |
| Motion | MPU6050 | I2C 0x68, ±2 g, mounted +Z up. Raw acceleration, no DMP |
| Presence | PIR, then IR proximity | PIR through a 74HC14 on GPIO 34. IR on GPIO 35 |
| Process noise | KY-037 | Analog envelope on GPIO 32 |
| Threshold | Potentiometer | GPIO 33, mapped to a risk threshold of 30–90 |
| Identity | RC522 RFID | SPI. A known card opens the shift. The same card acknowledges |
| Display | SSD1306 OLED, 128×64 | I2C 0x3C. The I2C LCD stays in the bag |
| Body alarm | Vibration motor, RGB LED | Motor from 5 V through a 2N2222A. 1N4007 across the motor, cathode at 5 V. RGB through 220 Ω |
| Room loads | 2-channel relay | Active-low. Fan on GPIO 17, beacon on GPIO 26 |
| Power | HW-411 buck | 12 V adapter to a measured 5.0 V (pass band 4.8–5.2 V) before the ESP32 is attached. 5 V feeds VIN, relay VCC, and the motor. The 74HC14 is powered at 3.3 V |
| Level clean-up | 74HC14 | One gate on the PIR. Powered at 3.3 V so a 5 V high cannot reach an ESP32 pin |

Not fitted: 4×4 keypad, I2C LCD, spare 1-channel relay, 2N3906, IRF9540. Eight keypad pins collide with the SPI and sensor map. A second RFID tap is the acknowledge.

### Firmware and software

| Piece | Choice |
|---|---|
| Language | Arduino C++ for ESP32 |
| Sensor libraries | DHT sensor library for ESPx (`DHTesp`), Adafruit GFX, Adafruit SSD1306, Adafruit BusIO |
| Motion read | Wake `PWR_MGMT_1`, read six bytes from `0x3B`, divide by 16384 for ±2 g |
| Storage | LittleFS, file `/shift.csv` |
| Network | Wi-Fi STA for 10 s, then an access point named `RAKSHA` at `192.168.4.1` |
| Dashboard | `dashboard/index.html`. One file. CSS and script are inline. No fonts and no scripts are loaded from the network, so the page works from the ESP32 with the router unplugged |
| Simulation | Logicraft Studios Velxio editor at logicraftstudios.tech. The project is `simulation/raksha-logicraft.zip` (Wokwi diagram plus the sketch). The board type in the diagram is `board-velxio-esp32` |
| Configuration | One `config.h` holds every pin, threshold, timing constant, known card, and the command PIN |

### Pin map

| GPIO | Function |
|---|---|
| 21 / 22 | I2C SDA / SCL. OLED 0x3C, MPU6050 0x68, barometer 0x76 or 0x77 |
| 4 | DHT22 |
| 32 | KY-037 analog |
| 33 | Threshold potentiometer |
| 34 | PIR, after the 74HC14. Input only |
| 35 | IR proximity. Input only |
| 36 | NTC divider. Input only |
| 16 | Vibration transistor base, through 1 kΩ |
| 17 | Fan relay input, pulled to the inactive level |
| 26 | Beacon relay input, pulled to the inactive level |
| 25, 13, 14 | RGB red, green, blue, 220 Ω, common cathode |
| 18, 19, 23, 5, 27 | RFID SCK, MISO, MOSI, SS, RST |

Strapping pins 0, 2, 12, and 15 are not used as outputs. Analog inputs stay on ADC1.

## 4. Architecture

![Architecture. The buck feeds the ESP32. Sense, estimate, decide, record, and serve run on the node. Act drives the worker interface. The dashboard is a phone on the RAKSHA access point.](figures/architecture.svg)

The node is a single ESP32. There is no cloud in the demo path. A dead campus network is not a failed system: the access point still serves the page, the OLED and the vibration motor still run, and the CSV still grows.

Data moves in one direction.

```text
Sample  →  Features  →  Decision  →  outputs and a log row
```

- **Sample** is what the pins just said.
- **Features** are the heat index, the risk score, presence, acceleration magnitude, and tilt.
- **Decision** is the shift, the heat band, the fall state, the two relays, and the fault reason.

The fast path and the slow path do not share a blocking call. A DHT read can take as long as its own 2-second period. That read is not inside the fall loop.

## 5. Pipeline

```text
POWER → BOOT → SENSE → ESTIMATE → DECIDE → ACT → RECORD → SERVE
```

| Stage | When | Result | If it fails |
|---|---|---|---|
| Power | Once, before logic | 12 V bucked to 5.0 V on VIN, the relay board, and the motor supply | Do not connect the ESP32 until the buck is inside 4.8–5.2 V |
| Boot | Every reset | Relay pins driven inactive first, then I2C and the DHT are checked | A failed critical sensor stays in fault. Relays stay off |
| Sense | 10 ms and 2 s | MPU6050 every 10 ms. Climate, sound, pot, PIR, and IR every 2 s. RFID on tap | A missed DHT is flagged. The previous temperature is not reused as “good” |
| Estimate | Same clocks | Heat index, temperature vote, 0–100 risk, presence, magnitude, tilt | Heat is invalid if the DHT misses the other thermometers by more than 8 °C |
| Decide | Same clocks | Shift, band, air alert, fall state, fault | Heat invalid forces the relays off |
| Act | On change | OLED, RGB, vibration, fan, beacon | Fault: red blink, both relays off, no stale temperature on the OLED |
| Record | 15 s and every edge | One CSV row in flash | A full filesystem sets a warning and does not stop the fan or the fall path |
| Serve | On request | Dashboard and three PIN-checked commands | No router: the access point still serves the page. RFID acknowledge does not need the network |

### Heat bands

| Heat index | Band | What the worker gets |
|---|---|---|
| below 27 °C | none | Green. Exposure does not count as heat time |
| 27–32 °C | caution | Amber. One short vibration pulse on entry. Exposure seconds count |
| 32–39 °C | extreme caution | Amber. Same pulse on entry. Exposure seconds count |
| 39–51 °C | danger | Red. Vibration pulses. Fan on. Danger seconds count |
| above 51 °C | extreme | Red. Vibration pulses. Fan on. Danger seconds count |

The fan also closes when the process-risk score is at or above the threshold, the shift is open, a person is present, and the heat reading is valid. That can happen while the band is still caution. The fan opens again when the person leaves, because the air alert requires presence.

### Heat index

Temperature is converted from Celsius to Fahrenheit. A simple screening value is computed first. If the average of that screening value and the air temperature is under 80 °F, the screening value is the index. Otherwise the full Rothfusz regression is used, including the low-humidity and high-humidity corrections, and the result is converted back to Celsius.

### Temperature vote

On the bench, the DHT22 temperature is compared with the NTC and with the barometer temperature. A fault is declared only when the DHT misses both of the others by more than 8 °C. In the simulator there is no barometer, so the vote is DHT against NTC. An invalid heat reading forces both relays off and clears the displayed temperature.

### Process risk

```text
stagnation   = clamp((RH − 65) / 20, 0, 1)
heat_couple  = clamp((heat_index − 27) / 12, 0, 1)   if heat is valid, else 0
score        = 100 × (0.50 × sound_ema + 0.30 × stagnation + 0.20 × heat_couple)
threshold    = 30 + (potentiometer / 4095) × 60, clamped to 30–90
```

`sound_ema` is a 10-second exponential envelope of the KY-037 reading, scaled to 0–1. A clap dies inside that window. The pot has to be held up, on the bench, for the score to climb. The scripted simulator run injects the settled envelope so the demonstration fits in one pass. The live node really uses the 10-second average.

An air alert requires all of: shift open, person present, heat valid, and score at or above the threshold.

### Fall

| Gate | Condition |
|---|---|
| Free-fall | Magnitude below 0.4 g for 80 ms |
| Impact | Magnitude above 2.5 g within 500 ms of the free-fall |
| Still | Tilt more than 60° from upright, and only while magnitude is between 0.7 g and 1.3 g, held with the PIR quiet for 2000 ms |
| Held alarm | Latched until the same worker’s card, or a fault |

If the posture never appears, the impact state times out after 1500 ms. If the worker moves during the still check, the sequence breaks and returns toward idle. The alarm does not clear itself.

### Record

Columns, written every 15 seconds during a shift and on every edge:

```text
ms, worker_id, t_dht, rh, heat_index_c, t_ntc, t_baro, sound_ema,
risk_score, present, heat_band, fall_state, fan_on, beacon_on, fault_reason
```

The simulator prints the same row on the serial line, prefixed with `CSV`.

### Serve

1. Try the configured STA network for 10 seconds.
2. If that fails, start the access point `RAKSHA` at `192.168.4.1`.

`GET /api/state` needs no PIN. It returns the current decision: mode, worker, heat index, band, risk, threshold, presence, fall state, fan, beacon, exposure seconds, danger seconds, and the fault reason.

`POST /api/ack`, `POST /api/fan`, and `POST /api/threshold` require the PIN from `config.h`. A wrong PIN is HTTP 401 and the node does not change. Fan also takes `on`. Threshold also takes `value`.

The page itself is stored with the firmware so it does not depend on the internet.

## 6. Dashboard

The page is the employer’s view of one bench, drawn as a single instrument rather than a grid of generic charts. It is `dashboard/index.html`.

![Dashboard at boot. No worker, exposure 0:00:00, both relays off, heat index empty.](figures/dashboard-boot.png)

### How to read it

**Exposure chronometer.** This is the largest figure on the page because it is the record. It shows the worker id, the mode line, total exposure as hours:minutes:seconds, and a smaller danger-time clock. A presence lamp says whether someone is in the zone. When the bench is empty the clock pauses. The line under the clock states the rule: an empty bench does not add time.

**Heat capillary.** A vertical scale with marks at 51, 39, 32, and 27 °C, next to the heat-index number. The band label follows the index. If the sensor fails, the number becomes a dash and the label says there is no reading. If the index is still valid but the person has left, the label says the band is paused. The page does not keep a previous “safe” temperature on screen.

**Process-air risk.** The score is out of 100, next to the fan threshold. Under it, three bars show where the points came from: sound at 50%, still air at 30%, heat at 20%. The panel says, on the glass, that this node has no gas sensor and the score is not a ppm reading. Two relay lamps show fan and beacon.

**Fall machine.** Four stages in order: free-fall (below 0.4 g), impact (above 2.5 g), still, and alarm held. Completed stages light. The held stage stays lit, in red, until the alarm is acknowledged. The caption under the title restates the rule that a shake is not a fall.

**Shift tape.** A short log of edges, newest first, with a timestamp. On the live node this is the recent decision trail. In rehearsal it is the script as it plays.

**Counter.** PIN, Acknowledge, fan on or off, and a threshold from 30 to 90. The three commands are the three posts above. A wrong PIN prints a refusal and the bench does not change. During a DHT fault, a fan command is refused because the fault is holding both relays off. A second card tap at the bench still acknowledges with no network; the button is the same action from the phone.

### Rehearsal and live

On load, the page tries `GET /api/state`. If the node answers, the link line says the page is live and the rehearsal buttons hide. The page then polls every 2 seconds.

If the node does not answer, the page stays in rehearsal. **Play the shift** runs the same story as the serial command `RUN`: card, caution, a clap that does nothing, sustained noise that closes the fan, a shake that stays idle, and a drop that holds an alarm. The script stops on the held alarm so the counter can be tried. The rehearsal PIN is `1947`. **Unplug DHT** then shows the fault: the heat number clears and both relays are off.

![Held alarm after the scripted drop. Worker SIM-01, exposure 0:00:07, heat index 30.7 °C paused because the bench is empty, process risk 59, beacon on, fall stage 4 held.](figures/dashboard-alarm.png)

## 7. Simulation

The editor is the Velxio editor at [logicraftstudios.tech/editor](https://logicraftstudios.tech/editor/). It compiles the ESP32 sketch and runs it against the wired canvas.

Open `simulation/raksha-logicraft.zip` with **File → Open project…**. If a “start a new project” panel is showing, cancel it. That panel replaces the workspace. Install the DHT ESP library and the Adafruit SSD1306 stack if the first compile asks for the headers. Press run. In the serial monitor, type `RUN`.

Canvas stand-ins:

| On the canvas | GPIO | What it stands in for |
|---|---|---|
| SSD1306 OLED | 21, 22 | The worker display |
| MPU6050 | 21, 22 | The fall sensor, +Z up |
| DHT22 | 4 | Temperature and humidity |
| NTC module | 36 | The second thermometer |
| PIR | 34 | Presence |
| Potentiometer at 15% | 32 | KY-037 envelope |
| Potentiometer at 45% | 33 | Threshold. 45% maps to 57 |
| Green CARD button | 27, pull-up | RFID tap. Press once to open the shift, again to acknowledge |
| Relay | 17 | Fan |
| Relay | 26 | Beacon |
| Red, green, blue LEDs, 220 Ω | 25, 13, 14 | RGB |
| Buzzer | 16 | Vibration motor |

The barometer, the IR sensor, the buck, and the 74HC14 stay on the physical bench. They are not on this canvas.

Other serial commands, for a single step instead of the whole script: `CARD`, `DROP`, `SHAKE`, `UNPLUG`, `HELP`.

## 8. Results

One `RUN`, threshold forced to 57. The serial line is the measurement.

| Time | Input | Observed |
|---|---|---|
| Boot | Power-on | `BOOT relays=OFF`, OLED ok, MPU ok |
| 0.5 s | Card, 30 °C, 55% RH, sound 0.10, present | `SHIFT worker=SIM-01`. Heat index 31.9 °C. CAUTION. Risk 13.1. Fan off. Beacon off. Fall idle |
| 2.5 s | 28 °C, 70% RH, sound 0.12 | Heat index 30.7 °C. CAUTION. Risk 19.6. Fan still off. A short noise does not trip the fan |
| 5 s | Same climate, sound 0.90 | Risk 58.6. Air alert on. Fan on. Band still CAUTION, so the fan closed from process risk, not from the heat band |
| 8 s | Shake, magnitude kept above 1 g | No free-fall line. Fall stays IDLE |
| 10.5 s | Drop: about 0.15 g, then about 3.2 g, then a tilt near 75° and presence lost | `FALL FREEFALL`, `IMPACT`, `CONFIRM`, `ALARM`. Beacon on. Fan off, because presence is gone |
| 15 s | Second card | `ACK`. Fall idle. Beacon off |
| 17 s | DHT removed | `FAULT DHT relays=OFF`. Heat fields cleared in the CSV. Both relays off |

The same story is what **Play the shift** shows on the dashboard. Exposure on that run stopped at a few seconds because the person left at the drop, which is the presence rule, not a missed tick.

### Hidden cases the node is built to survive

| Case | Expected |
|---|---|
| DHT unplugged during a shift | Fault, relays off, no stale safe temperature |
| MPU shaken for about 10 s | Fall stays idle |
| A short clap, then sustained noise, with a hand on the PIR | The clap does nothing. Sustained noise with presence closes the fan |
| Person leaves | After the pause window, exposure pauses. It is not wiped |
| Reset while the fan is on | Relay off at boot until a new valid shift |
| Unknown card | Shift does not open |
| Wrong PIN on the dashboard | HTTP 401. Nothing changes |
| No router | OLED, vibration, and the CSV continue. The page is on the access point |

## 9. Repository map

```text
README.md                 entry point
dashboard/index.html      the instrument page
docs/REPORT.md            this report
docs/REPORT.pdf           printable copy
docs/pipeline.md          stage contracts, formulas, and the config.h list
docs/runbook.md           build order, wiring checks, demo order
docs/figures/             the figures in this report
simulation/sketch.ino     simulator firmware
simulation/diagram.json   canvas
simulation/raksha-logicraft.zip
simulation/libraries.txt  DHT ESP, Adafruit GFX, SSD1306, BusIO
```

The bench firmware follows the same pipeline, in `config.h` plus one unit per stage: sense, estimate, decide, act, record, serve. The simulator sketch is the version that runs on the canvas today. It uses the same heat-index function, the same risk weights, and the same fall gates.
