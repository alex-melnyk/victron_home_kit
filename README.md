# Victron MultiPlus-II → Apple HomeKit Bridge

An [Arduino Nano ESP32](https://docs.arduino.cc/hardware/nano-esp32/) (u-blox NORA-W106 / ESP32-S3)
that reads a **Victron MultiPlus-II GX** over MQTT and exposes it to **Apple HomeKit** using
[HomeSpan](https://github.com/HomeSpan/HomeSpan). No cloud, no Victron VRM account — everything
runs on your local network.

## What it exposes

The bridge publishes **two HomeKit accessories**:

| Accessory | Reading | Apple Home | Eve / Controller for HomeKit / Home+ |
|-----------|---------|-----------|--------------------------------------|
| **Victron Battery** | State of charge | `96 %` (humidity + Battery Level) | `96 %` |
| | Charging | `Charging: Yes/No` (Status) | ✓ |
| | Battery voltage | `53.4 lux` | **53.4 V** |
| **Victron Inverter** | Inverter AC output power | `675 lux` | **675 W** |
| | Grid (AC input) power | `720 lux` | **720 W** |

### Why "lux"?
HomeKit has **no unit for volts or watts** — the only sensor types whose value the Apple Home app
shows on a tile are temperature (°), humidity (%) and light (lux). So volts/watts are carried on a
**Light Sensor** (the number is exact; only the label is a stand-in — read `lux` as `V`/`W`).
The same values are *also* published as **Eve custom characteristics** (`Voltage`, `Consumption`),
so third-party apps — **Eve**, **Controller for HomeKit**, **Home+** — display the real `V`/`W` units.
See the long discussion of this limitation in the header comment of the sketch.

## Hardware

- Arduino Nano ESP32 (NORA-W106 / ESP32-S3) — powered from any USB-C source
- Victron MultiPlus-II **GX** (or any Venus OS device) with **MQTT enabled** on port 1883
  (Remote Console → Settings → Services → MQTT on LAN (plaintext))
- Same Wi-Fi/LAN as the GX

## Setup

1. **Enable MQTT** on the Victron GX (Settings → Services → *MQTT on LAN (Plaintext)*).
2. **Create your secrets file** from the template and fill in your values:
   ```sh
   cp arduino_secrets.example.h arduino_secrets.h
   # then edit arduino_secrets.h
   ```
   `arduino_secrets.h` is git-ignored, so your credentials never get committed.
3. **Find your VRM Portal ID:** GX → Settings → VRM online portal → *VRM Portal ID*.
4. **Verify the `vebus` instance** (device-specific; it's `275` on the author's unit):
   ```sh
   mosquitto_sub -h <GX_IP> -t 'N/<VRM_ID>/vebus/#' -v
   ```
   Look at the number after `/vebus/` and, if different, update the `vebus/275/...` topics in the sketch.
5. **Flash** (see below), then add it in the Home app: **+ → Add Accessory → More options… → Victron Bridge**
   and enter your pairing code.

## Build & flash

Uses the **Espressif** ESP32 core (HomeSpan needs core ≥ 3.3.0), FQBN `esp32:esp32:nano_nora`.

One-time toolchain setup:
```sh
brew install arduino-cli
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index && arduino-cli core install esp32:esp32
arduino-cli lib install HomeSpan PubSubClient ArduinoJson
python3 -m pip install --user pyserial   # only for ./flash.sh monitor
```

Then use the helper script:
```sh
./flash.sh            # compile + flash + monitor (auto-detects the board port)
./flash.sh compile    # compile only
./flash.sh flash      # compile + flash
./flash.sh monitor    # tail the serial log (115200 baud)
```

## MQTT topics used

Read (subscribed):
```
N/<VRM_ID>/system/0/Dc/Battery/Soc
N/<VRM_ID>/system/0/Dc/Battery/Voltage
N/<VRM_ID>/system/0/Dc/Battery/Power        (sign → charging indicator)
N/<VRM_ID>/vebus/<INSTANCE>/Ac/Out/P        (inverter AC output power)
N/<VRM_ID>/vebus/<INSTANCE>/Ac/ActiveIn/P   (grid / AC input power)
```
Keepalive (Venus OS drops silent clients after ~60 s):
```
R/<VRM_ID>/keepalive     (empty payload, every 30 s)
```

## Notes

- **Persistence:** HomeKit pairing + Wi-Fi are stored in flash; the device auto-resumes after any
  power cycle (values repopulate within a few seconds). No re-pairing needed.
- **Uncertified warning:** the Home app shows "not certified" for any DIY HomeKit device (no Apple MFi
  credential). It's cosmetic — everything works.
- **Matter:** the ESP32-S3 *can* run Matter over Wi-Fi, but Apple Home doesn't display Matter power/energy
  data (as of mid-2026), so it wouldn't help show watts today. See **[Roadmap: a possible Matter v2](#roadmap-a-possible-matter-v2)**.

## Roadmap: a possible Matter v2

*Why this uses classic HomeKit today, and what would justify a Matter rewrite later.*
**Assessment current as of July 2026 — revisit when iOS 27 ships.**

HomeKit (HAP) has **no volts/watts/energy characteristic**, so in Apple's Home app the electrical
readings ride on a borrowed type (`lux`); real `V`/`W` are only available via the Eve-style custom
characteristics this sketch adds, viewed in Eve / Controller for HomeKit / Home+.

**Matter** *does* define proper energy device types (Matter 1.4: Electrical Power/Energy Measurement,
Battery Storage, Solar Power; inverters are on the CSA roadmap). But as of mid-2026, **Apple Home
still does not display Matter energy data**, and Apple's [EnergyKit](https://developer.apple.com/energykit/)
(iOS 26) only pushes grid/rate/clean-energy forecasts *out* to EV-charger and thermostat apps (US-only) —
it does **not** ingest device power *into* Home. Energy monitoring in the Home app is *reported* to be
coming in **iOS 27** (fall 2026), but that's unshipped as of this writing.

A Matter rewrite only makes sense once **all three** are true:

1. **Apple Home actually renders Matter energy device types** — verify iOS 27 displays
   Battery-Storage / Solar / Inverter device types, not just EV/thermostat scheduling.
2. **ESP32 tooling exposes the energy clusters** — Electrical Power Measurement (`0x0090`) and
   Electrical Energy Measurement (`0x0091`) are not yet wrapped by the Arduino Matter library
   (open feature request); otherwise it needs raw ESP-Matter (ESP-IDF) C++.
3. **You accept rewriting off HomeSpan** — a device is *either* a HAP accessory *or* a Matter one,
   not both. Matter-over-Wi-Fi works on this ESP32-S3 (no Thread radio).

Until then, this HomeSpan build is the right call for Apple Home. Note: **Home Assistant / SmartThings
already display Matter energy today**, so a Matter v2 would benefit those platforms immediately,
independent of Apple.

## Libraries

[HomeSpan](https://github.com/HomeSpan/HomeSpan) · [PubSubClient](https://github.com/knolleary/pubsubclient) · [ArduinoJson](https://arduinojson.org/)

## License

[MIT](LICENSE) © 2026 Alex Melnyk
