# RAKSHA runbook

Three hours. Build the pipeline in stage order, then rehearse the faults, then speak the pitch. If a later stage slips, do not steal time from Boot, the fall machine, or the relay-off rule. Those three are what a hidden scenario will hit.

## Clock

| Time | Stage | Stop when |
|---|---|---|
| 0:00–0:25 | Power, Boot, pins | Meter shows 5.0 V. OLED lists self-test. Relays stay off across a reset. `config.h` exists and every GPIO in the pin map is named there. |
| 0:25–1:15 | Sense fast, Sense slow, Estimate, fall Decide | Serial prints heat index in °C. A warm hand moves it. Shaking the MPU stays `IDLE`. A foam drop reaches `ALARM`. Sound envelope ignores one clap and climbs on sustained noise. |
| 1:15–1:45 | RFID shift, Act | Known card opens a shift. Second tap clears a fall. RGB and vibration match the band. Fan clicks on danger or air alert. Beacon clicks on fall. |
| 1:45–2:15 | Record, Serve | `/api/state` on `http://192.168.4.1` shows the live shift. Wrong PIN is rejected. CSV still grows with the radio off. |
| 2:15–2:40 | Hidden-scenario rehearsal | All six runs below done once, results written on the results slide. |
| 2:40–3:00 | Pitch | Seven slides. One spoken pass out loud. |

## Build order inside the first hour

1. Buck to 5.0 V, photographed.
2. ESP32, OLED, MPU6050 on I2C. Print the scan.
3. Relay pins inactive, pull-ups fitted, reset the board, confirm both relays stay off.
4. DHT22 on GPIO 4. Print °C and RH.
5. NTC on GPIO 36. Print °C next to DHT.
6. MPU magnitude at 10 ms. Print `accel_g`.
7. Fall machine. Shake, then drop.
8. KY-037, pot, PIR, IR. Print `sound_ema`, `risk_score`, `present`.
9. Barometer temperature into the vote.

Only then wire the motor transistor and the relay loads.

## Wiring checks before first power-up

- HW-411 output measured before the ESP32 is connected.
- 74HC14 VCC is 3.3 V.
- 1N4007 is across the motor, band toward 5 V.
- Motor current goes through the 2N2222A collector, not through a GPIO.
- Relay inputs are pulled to the off level.
- No analog sensor is on GPIO 0, 2, 4, 12–15, 25–27. Those are ADC2 and read wrong once Wi-Fi starts.

## Demo card

Put one UID into `KNOWN_UIDS` as soon as the reader prints it. Label that card `WORKER`. Keep a second unlabeled card for the unknown-card run.

## Hidden-scenario rehearsal

Run these after the dashboard works. Write the observed result in one line each. That list is the results slide and the edge-case slide.

| # | Action | Required result |
|---|---|---|
| 1 | Unplug DHT22 during a shift | `FAULT DHT`. Fan off, beacon off. OLED does not keep a stale “safe” temperature. |
| 2 | Shake MPU on the bench for 10 s, worker card in a shift | `fall_state` stays `IDLE` |
| 3 | Drop MPU onto foam from about 30 cm | `ALARM`, vibration on, beacon relay on |
| 4 | One clap at the KY-037, then 10 s of sustained noise with a hand on the PIR | Clap does nothing. Sustained noise with presence sets `air_alert` and turns the fan on. |
| 5 | Leave the PIR and IR clear | After 60 s, `exposure_s` stops increasing and is not cleared |
| 6 | Reset the board while the fan relay is on | Relay drops during boot and stays off until a new valid shift reaches danger or air alert |

Extra, if time remains:

| Action | Required result |
|---|---|
| Unknown RFID card | OLED `UNKNOWN CARD`. Mode unchanged. |
| Join the access point and send `ack` with the wrong PIN | HTTP 401. Alarm stays latched. |
| Send `ack` with the right PIN, or tap the worker card | Beacon off, fall back to idle, row logged |
| Power the node with no router | OLED and vibration still work. Dashboard loads at `192.168.4.1`. |

## One-minute live demo

Do it in this order. Do not improvise a new sequence in front of the judges.

1. Reset. Point at the relays. They are off. OLED reaches `SELF-TEST OK` then `IDLE`.
2. Tap the worker card. OLED shows the UID.
3. Warm the DHT22. Band moves to caution or higher. RGB leaves green. Exposure seconds increase only after a hand covers the PIR.
4. Make sustained noise at the KY-037 with the hand still there. Fan relay closes.
5. Drop the MPU onto foam. Vibration runs. Beacon relay closes.
6. Tap the worker card again. Beacon opens. Vibration stops.
7. Pull the DHT22 wire. Fault screen. Both relays off.

## Pitch

Seven slides, in this order. The format is fixed by the round.

### 1. Introduction

RAKSHA is a workstation node for kiln, welding, tailoring, and construction work. It warns the person in the hazard and keeps an exposure record for that worker.

### 2. Problem statement

Informal workers stand in heat, dust, and fumes with no instrument and no log. A fall in a loud shop is easy to miss. Employers cannot improve a condition they do not measure. The device has to be cheap enough for a small workshop and clear enough to trust.

### 3. Approach

Walk the pipeline once: Power, Boot, Sense, Estimate, Decide, Act, Record, Serve.

Three decisions to say out loud:

- Exposure is presence-gated. An empty bench does not invent hours.
- A fall is free-fall, then impact, then stillness. Vibration alone cannot pass the free-fall gate.
- Past a threshold the node closes a fan relay. On a fall it closes a beacon relay. Both open on a fault and both start open at boot.

Air is a process-risk score: sustained noise, humidity, heat, and a person present. The kit has no gas sensor. The potentiometer sets the shop threshold.

### 4. Simulation

Show one serial capture or a plotted replay of a scripted shift:

- heat index climbing through 27 °C and 32 °C
- one clap leaving `risk_score` under the threshold
- sustained noise crossing it
- table vibration staying in `IDLE`
- a drop entering `ALARM`
- DHT removal entering `FAULT`

Label the axes. A screenshot of the real serial plot is enough.

### 5. Results

The four measured numbers, then the one-minute demo.

- Buck voltage, unloaded and loaded
- NTC room temperature and warm-hand temperature
- Shake result and drop result
- Relay state at the moment of reset

### 6. Edge cases

Use the rehearsal table. Invite the judges to shake the board and to pull the DHT wire. Those two are the ones to offer, because both are already passing.

### 7. SDG relevance

SDG 8, decent work. The worker gets a warning they can feel in a shop where a buzzer would be lost in the noise. The employer gets minutes of heat exposure and minutes of danger, tied to a person, plus a fan that is the first corrective action rather than a report that arrives later.

## Spoken defence

About three minutes.

| Clock | Say |
|---|---|
| 0:00–0:20 | Who it is for, and the three hazards: heat, process air, falls. |
| 0:20–1:00 | The three decisions: presence gate, free-fall gate, relays that fail off. |
| 1:00–2:00 | The one-minute demo, in the order above. |
| 2:00–2:40 | Offer the shake test and the DHT pull. Mention Wi-Fi loss only if asked, and point at the access point. |
| 2:40–3:00 | SDG 8 in two sentences: a warning the worker can feel, and an exposure record the employer can act on. |

## If time runs out

Ship this subset. It still covers every objective.

| Keep | Drop |
|---|---|
| DHT22 heat index, MPU fall machine, RGB, vibration, one relay, OLED, CSV, boot with relays off | Barometer vote, IR, second relay, dashboard buttons |

If the dashboard slips, the access point and `GET /api/state` alone still score connectivity. RFID acknowledge already covers offline commands.

## Parts not in the build

| Part | Reason it is unused |
|---|---|
| 4x4 keypad | Eight pins collide with SPI and the sensor map. A second RFID tap is the acknowledge. |
| I2C LCD | OLED is the worker display. A second display spends the hour on addresses. |
| 1-channel relay | Fan and beacon use the 2-channel board. |
| 2N3906, IRF9540 | The relay module switches the loads. The 2N2222A switches the motor. |
| 2N7000 | Spare. Use it only if a relay input needs a different inactive level. |
