# Smart Room Controller (ESP32)

An IoT system that controls a fan and light only when they are useful:
the room must be occupied, and the temperature or darkness must justify it.

## Features

- Fan turns ON when room is occupied and temperature >= 26°C, OFF at <= 25°C (hysteresis)
- Light turns ON when room is occupied and dark
- Everything turns OFF after a timeout with no motion (saves electricity)
- Non-blocking timers using `millis()`

## Hardware

ESP32 DevKit, DHT22, PIR (HC-SR501), LDR module, 2-channel relay module

## Live Demo

Dashboard: https://jewelh9.github.io/smart-room-controller/dashboard/
Simulation: https://wokwi.com/projects/480946137911089153

![Simulation](docs/wokwi-simulation.png)

## Architecture

ESP32 --(MQTT publish)--> HiveMQ broker --(MQTT over WebSockets)--> Web dashboard
ESP32 <--(MQTT subscribe)-- HiveMQ broker <--(commands)-- Web dashboard

## Key Concepts Implemented

- Occupancy-based automation with a non-blocking `millis()` timeout
- Hysteresis (26°C on / 25°C off) to prevent relay chatter
- Manual override (ON/OFF/AUTO) layered over automation
- MQTT retained messages and Last Will and Testament for device status
- Remote configuration with input validation on the device
- Automation keeps working if WiFi or the broker is down
- Responsive dashboard (CSS Grid, SVG gauges, canvas chart), no frameworks

## Limitations and Future Work

- Uses a public MQTT broker. A production version would use a private broker with authentication and TLS.
- Energy figures are estimates based on assumed appliance wattage.
- Fan speed control is not supported (relay is ON/OFF only).
- Future: OLED display, OTA firmware updates, per-room support.

## Status

- [x] Stage 2: Simulation
- [x] Stage 3: Breadboard prototype
- [ ] Stage 4: Real fan/light via relay
- [x] Stage 5: MQTT + web dashboard
