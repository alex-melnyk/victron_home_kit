---
description: Compile, flash, and monitor the Victron HomeKit bridge on the Nano ESP32
argument-hint: "[compile|flash|monitor|all]  (default: all)"
allowed-tools: Bash(./flash.sh:*), Bash(arduino-cli board list:*)
---

Run the Victron HomeKit bridge build/flash workflow for the Arduino Nano ESP32.

Mode requested: `$ARGUMENTS` (if empty, use `all` = compile + flash + monitor).

Steps:
1. Run `./flash.sh $ARGUMENTS` from the project root (`flash.sh` auto-detects the
   board's serial port, uses FQBN `esp32:esp32:nano_nora`, and — for `monitor`/`all` —
   tails the serial output for `MON_SECS` seconds).
2. Read the output and report:
   - **Compile**: flash/RAM usage %, and any errors or warnings.
   - **Flash**: that the DFU download reached 100% and printed `Done!`.
   - **Monitor**: confirm the bridge is healthy by checking the serial log for
     `[MQTT] ... connected`, live values for `Dc/Battery/{Soc,Voltage,Power}`,
     `vebus/275/Mode`, `[MQTT] Keepalive sent`, and any `Pair Setup` / `Client Connected`
     HomeKit activity. Call out anything missing (e.g. stuck on `[WiFi] Connecting...`
     means Wi-Fi creds/SSID; no `[MQTT]` lines means the GX broker is unreachable).
3. If the board isn't detected, tell the user to plug in the Nano ESP32 (or pass
   `PORT=/dev/cu.usbmodemXXXX`), then re-run.

Notes:
- The Nano ESP32 uses native USB-CDC for `Serial`; the monitor reads without toggling
  DTR/RTS on purpose (toggling resets the S3 and drops the port).
- vebus instance for this unit is **275** (verified against the live GX). If the Multi
  is ever swapped, re-verify with `arduino-cli` gone and instead:
  `python3 victron_probe.py` in scratchpad, or subscribe to `N/<VRM_ID>/vebus/#`.
