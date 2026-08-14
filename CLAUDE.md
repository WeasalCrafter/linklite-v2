# CLAUDE.md — LinkLite v2 Firmware

Guidance for working on firmware in this project. Read this before generating or editing sketches.

## Role

You are assisting an embedded firmware developer who specializes in **ESP32-C6** firmware. Assume familiarity with the ESP-IDF/Arduino-ESP32 stack, the LEDC (PWM) peripheral, GPIO interrupts, light sleep, and the C6's radios (Wi-Fi 6, BLE 5, 802.15.4). Be precise about hardware; don't hand-wave pin behavior or timing.

## Project goal

**LinkLite v2** is a modular LED bar light. Each bar is an independent, ESP32-C6-based module that drives a strip of LEDs and can **sync its brightness with other identical bars**. Bars can also be driven by a separate **ESP32-C3 remote controller** built around an EC11 rotary encoder.

The defining design constraint is **relative, conflict-free brightness control**: multiple controllers and multiple bars must all stay in agreement without any one device being an authority. Brightness changes propagate as *deltas* (increase/decrease), never as absolute "set to X" commands — see [Sync model](#sync-model).

## Repository layout

Sketches are managed with **arduino-cli** and live under:

```
documents/linklite-v2/
```

Each sketch is its own directory (Arduino requires the sketch's `.ino` to match its folder name). The current test sketch is `esp-blink`.

## Build & upload

Compile and flash in a single command (compile then upload on success):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c6 -u -p /dev/ttyACM0 esp-blink
```

- **FQBN:** `esp32:esp32:esp32c6` (requires arduino-esp32 core **3.x+** — the C6 landed in 3.0.0).
- **Port:** The C6-MINI enumerates over its native USB Serial/JTAG as `/dev/ttyACM0`. If a separate USB-UART bridge is in use it will be `/dev/ttyUSBx` instead — confirm with `arduino-cli board list`.
- Swap `esp-blink` for whichever sketch directory you're building.
- If upload stalls on `Connecting...`, hold **BOOT**, tap **RESET**, release **BOOT** to force the bootloader.

## Hardware

Two board types in this project.

### Light bar — ESP32-C6-MINI-1

Drives the LEDs and participates in brightness sync.

| Function       | GPIO | Notes |
|----------------|------|-------|
| User button 1  | 20   | Input, needs pull-up + debounce |
| User button 2  | 21   | Input, needs pull-up + debounce |
| User LED       | 22   | Status/indicator output |
| Main LEDs      | 23   | **PWM via LEDC**, gates an N-channel MOSFET (AO3400A) driving the LED array |

Board context (from the PCB design): 28× 2835 LEDs in parallel with per-LED series resistors, MOSFET low-side switched, USB-C PD at 15 V stepped to 3.3 V via a TPS563201 buck. WAGO chaining connectors carry power between modules.

**Main LED dimming:** Use the **LEDC** peripheral on GPIO23, not `analogWrite` bit-banging. Pick a PWM frequency high enough to be flicker-free (≥ 1 kHz, commonly a few kHz) and a resolution that gives smooth low-end dimming (12-bit is a good default). The MOSFET is a switch — brightness is duty cycle. Consider gamma-correcting the duty curve so perceived brightness scales linearly with the encoder.

### Controller — ESP32-C3

A low-power remote. Sits in **light sleep** and wakes only on user interaction.

| Function            | GPIO | Notes |
|---------------------|------|-------|
| EC11 encoder pin A  | 10   | Quadrature; wake source |
| EC11 encoder pin B  | 20   | Quadrature |
| EC11 push button    | 21   | Momentary; wake source |

*(GPIO assignments not yet fixed — choose wake-capable pins and record them here once chosen.)*

**Behavior:**
- Rotate **clockwise → brightness up**, **counter-clockwise → brightness down**.
- The push button's role is undefined so far (on/off toggle? pairing? scene?). Decide and document here.
- The device stays in **light sleep** to preserve battery; encoder A (and the button) are configured as **GPIO wake sources**. On wake, read the encoder, emit the appropriate delta(s), then return to sleep after an idle timeout. Light sleep retains RAM/CPU state so resume is fast and the encoder position isn't lost.

## Sync model

This is the heart of the firmware. **Brightness is controlled by relative deltas, never absolute values.**

- A controller turn emits an event like `+step` or `−step`, broadcast to the bars.
- Each bar applies the delta to its **own** brightness state and re-broadcasts/converges so all bars land on the same value.
- Because every device only ever *adjusts* a shared value, two controllers acting at once compose cleanly (a `+2` and a `+1` net to `+3`) instead of fighting over "last writer wins," which is what absolute set-points would cause.
- Clamp at 0% and 100% consistently on every node so they don't drift apart at the rails.
- A bar joining late should query a peer for the current level rather than assuming a default, so it converges instead of resetting everyone.

Keep the brightness state authoritative **on the bars**; controllers are stateless emitters of intent. This keeps controllers cheap, sleepy, and hot-swappable.

## Wireless transport — open decision

There's an unresolved architecture question here; flag it, don't silently pick one.

- **Bar-to-bar sync:** prior decision was **ESP-NOW** - although other options are available
- **Controller-to-bar:** the **ESP32-C3 has no 802.15.4 radio** — it cannot speak Zigbee/Thread. A C3 controller must reach the bars over a transport both chips share: **ESP-NOW** (connectionless, low-latency, low-power, Wi-Fi-radio based) or **BLE**. ESP-NOW pairs well with the sleep-wake-emit-sleep controller loop.

Until this is settled, treat the transport as an abstraction: the firmware emits/consumes *brightness delta events*, and the link layer underneath is swappable. Don't hard-couple sync logic to a specific radio.

## Conventions & gotchas

- **Core version:** C6 support needs arduino-esp32 **3.x** (ESP-IDF v5.1+). On older cores the C6 FQBN won't even appear in `arduino-cli board listall esp32`.
- **Native USB CDC:** The C6/C3 expose USB Serial/JTAG directly. If serial output goes missing after flashing, confirm "USB CDC On Boot" is enabled for the target.
- **Strapping/boot pins:** The chosen bar pins (20–23) are clear of the C6's strapping pins, but verify any *new* pin against the datasheet before wiring firmware to it.
- **Debounce everything mechanical:** both user buttons and the encoder button need debouncing; the encoder A/B lines benefit from either hardware RC filtering or careful edge handling.
- Keep sketches self-contained per directory so the single-command build above stays valid.

## Reference docs

- arduino-cli: https://docs.arduino.cc/arduino-cli/
- ESP32-C6-MINI-1 datasheet: https://documentation.espressif.com/esp32-c6-mini-1_mini-1u_datasheet_en.pdf
