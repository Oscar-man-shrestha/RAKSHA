# RAKSHA

Workstation safety node for informal workshops: brick kilns, welding shops, tailoring units, and construction sites. It watches heat, process-air risk, and falls, warns the worker on their body, turns on a fan or beacon, and keeps a per-worker exposure record.

SDG 8 — Decent Work and Economic Growth. Problem 8-A.

## Pipeline

```text
POWER → BOOT → SENSE → ESTIMATE → DECIDE → ACT → RECORD → SERVE
```

| Stage | When | Result |
|---|---|---|
| Power | Once, before logic | 12 V bucked to a measured 5.0 V |
| Boot | Every reset | Relays off, self-test, fault or ready |
| Sense | 100 Hz and 2 s | Raw samples from the kit |
| Estimate | Same cadence | Heat index, process-risk, presence, motion features |
| Decide | Same cadence | Shift, band, fall state, fault |
| Act | Immediately on change | RGB, vibration, fan relay, beacon relay, OLED |
| Record | Every 15 s and on every edge | CSV row in flash |
| Serve | Continuous | Local dashboard, PIN-checked commands, works with no router |

Full stage contracts, formulas, pins, and failure behavior: [docs/pipeline.md](docs/pipeline.md).

Hour-by-hour build, demo script, and the 7-slide pitch: [docs/runbook.md](docs/runbook.md).

Printable document, including the Logicraft simulation procedure: [docs/RAKSHA.pdf](docs/RAKSHA.pdf).

Open the simulator at [logicraftstudios.tech/editor](https://logicraftstudios.tech/editor/) and load [simulation/raksha-logicraft.zip](simulation/raksha-logicraft.zip). Type `RUN` in the serial monitor.

## What the judges are scoring

| Points | Criterion | Where it lives in the pipeline |
|---|---|---|
| 30 | Core behaviour and algorithms | Estimate and Decide |
| 25 | Hardware design and measured evidence | Power, Sense, Act |
| 15 | Fault handling and hidden-scenario run | Boot, Decide, the rehearsal list |
| 15 | Telemetry, validated commands, offline behaviour | Record and Serve |
| 10 | Responsiveness, single config, safe boot | Boot, `config.h`, 100 Hz fall path |
| 5 | Documentation and 3-minute defence | This repo and the runbook pitch |

## Product rules

- Exposure time counts only while a worker is in the zone.
- A fall requires free-fall, then impact, then stillness. Shaking the bench is not a fall.
- Air is a process-risk score from sustained noise, humidity, and heat. This kit has no gas sensor. Say that in the pitch.
- The worker feels the alarm through the vibration motor. The room hears it through the beacon relay.
- Both relays are off at boot and off on any sensor fault.
- A second RFID tap acknowledges an alarm. An unknown card cannot start a shift.
- One `config.h` holds every pin, threshold, and timing constant.

## Parts that stay in the bag

4x4 keypad, I2C LCD, spare 1-channel relay, 2N3906, IRF9540. RFID second-tap is the offline acknowledge. The OLED is the only display.

## Firmware map to build next

```text
firmware/
  config.h            pins, thresholds, known RFID UIDs, PIN
  raksha.ino          timers, boot, loop
  sense.cpp           DHT22, NTC, barometer, KY-037, PIR, IR, MPU6050, RFID
  estimate.cpp        heat index, vote, process-risk, features
  decide.cpp          shift, bands, fall machine, faults
  act.cpp             LED, motor, relays, OLED
  record.cpp          LittleFS CSV
  serve.cpp           AP/STA, /api/state, /api/ack, /api/fan, /api/threshold
```
# RAKSHA
