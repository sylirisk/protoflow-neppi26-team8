# Team notes

What is true only for team 8: wiring, modules without a component, extra libraries and decisions. AI tools read this before changing a sketch or a page (AGENTS.md); add new facts here, not to AGENTS.md.

## Server

`gamma.proto.aalto.fi`: boards connect to `wss://gamma.proto.aalto.fi/ws/device`, pages to `wss://gamma.proto.aalto.fi/ws/app`. The sketches need ProtoFlow library 1.4.0 or newer (`ProtoFlow.zip` from proto.aalto.fi/dist) for `ProtoFlow.sendCommand`.

## Test board (`firmware/EinkBoard/`)

All four modules on one ESP32 on the custom Grove shield, tested together on 2026-11-20 against the staff server.

| Module | In the sketch | Pins |
|---|---|---|
| E-ink display: Adafruit eInk Breakout Friend with a bare 2.66" 296x152 panel (SSD1680) | no component; actuator `eink`, library **Adafruit EPD** | CS 32, DC 33, RST 27, BUSY 14, SPI SCK 18 / MOSI 23 / MISO 19, no SRAM |
| Grove Slide Potentiometer | `GroveSlider`, sensor `slider` (0-100) | SIG 34, VCC **3V3** (analog: GPIO 32-39 only) |
| Grove Buzzer | `GroveBuzzer`, actuator `buzzer` | SIG 25. Hard to reach from the shield's Grove ports: choose another pin next time |
| Grove Chainable RGB LED | `GroveChainableRGB`, actuator `rgb`, library **Grove - Chainable RGB LED** | the LED's IN connector: DI 17 (white), CI 16 (yellow), VCC 5V |

`firmware/EinkBoard/config.h` holds the WiFi password and stays out of git.

## E-ink display

- Black-and-white mode: `ThinkInk_266_Grayscale4_MFGN` with `THINKINK_MONO`; the panel shows only black. `setRotation(2)` gives landscape, 296x152.
- A full refresh takes 2.3 s (13 s in grayscale) and flashes the panel. Adafruit EPD 4.6.14 has no partial refresh for this panel; the GxEPD2 library might have one (not tried).
- A FreeRTOS task of its own draws the panel, so `loop()` keeps running during a refresh (longest gap measured: 3 ms). The command handler only stores the text; the task draws the first text at once, then the latest at most once per `MIN_DRAW_INTERVAL_MS` (1 s while testing).
- Command: `{"t": ["line 1", "line 2", ...]}`, at most 6 lines of 23 characters (text size 2). ä ö å Ä Ö Å are converted to the font's code page 437; other non-ASCII characters show as `?`. Page: `html/eink.html`.

## Plan: two boards (from the students, 2026-11-20)

- **Board A**: e-ink display, slider, LED. The slider simulates the room temperature, 18-45 °C, shown on the e-ink display and in a web app (not designed yet).
- **Board B**: LED and buzzer. Above 30 °C the LED blinks red and the buzzer gives groups of five 100 ms beeps with a 1 s pause, for about 5 s.
- Board A tells board B directly with `ProtoFlow.sendCommand("<board B's DEVICE_NAME>", ...)`, so no page needs to be open. Each board needs its own `DEVICE_NAME`.
- Still open: which LED modules (chainable RGB or plain Grove LED); whether the alarm repeats while the temperature stays above 30 °C; which GPIOs the custom shield's Grove ports have; the e-ink refresh interval (wanted: about 1 s).
