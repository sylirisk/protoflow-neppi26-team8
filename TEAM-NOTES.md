# Team notes

What is true only for team 8: wiring, modules without a component, extra libraries and decisions. AI tools read this before changing a sketch or a page (AGENTS.md); add new facts here, not to AGENTS.md.

## Server

`gamma.proto.aalto.fi`: boards connect to `wss://gamma.proto.aalto.fi/ws/device`, pages to `wss://gamma.proto.aalto.fi/ws/app`. The sketches need ProtoFlow library 1.4.0 or newer (`ProtoFlow.zip` from proto.aalto.fi/dist) for `ProtoFlow.sendCommand`.

## The shield: Grove-Base Shield for ESP32 v1.1 (Aalto, for the ESP32-DevKitC V4)

The pins of every port go GND, VCC, white wire, yellow wire, and each label is printed over its pin, as on the I²C ports (`GND 3V3 SDA SCL`: SCL is the yellow wire). So **a module's signal (yellow wire) is the number over the last pin**: the right one of the two in the right columns, the upper one in the left and middle columns. Checked against the PCB render in the platform repo's `dev/hardware/Grove_shield_for_ESP32/`; the traces themselves are not checked.

| Ports, top to bottom | Signal (yellow) / second pin (white) | VCC |
|---|---|---|
| Left column | 26/27, 27/32, 32/33, 33/26 | 3V3 or 5V, set by the slide switch |
| Middle column | I²C, I²C, 34/35, 35/36, 36/39, 39/34 (all four input only: analog modules) | 3V3 |
| Right column 1 | 14/15, 13/14, 5/13, 4/5, 2/4 | 3V3 |
| Right column 2 | 16/17, 17/18, 18/19, 19/23, 23/25 | 3V3 |
| Top right | I²C, UART | 3V3 |
| SERVO header | GPIO 26, 27, 32, 33 (GND / 5V / SIG) | 5V |

GPIO 15 and 25 are only second pins, so no module's signal can use them. The I²C ports' GPIOs aren't printed (the ESP32's defaults are SDA 21, SCL 22).

## The two boards

Each sketch's top comment has its wiring table; this is the same.

**Board A, `Room board`** (`firmware/RoomBoard/`), tested on hardware on 2026-11-20:

| Module | Port (printed numbers) | Wires | GPIO |
|---|---|---|---|
| Slider | middle column, 1st below I²C (34 / 35) | Grove cable | 34 |
| Chainable RGB LED | right column 2, top (17 16) | Grove cable into the LED's IN | 16 CI, 17 DI |
| E-ink, cable 1 | right column 2, 3rd (19 18) | yellow SCK, red VIN, black GND | 18 |
| E-ink, cable 2 | right column 2, bottom (25 23) | yellow MOSI | 23 |
| E-ink, cable 3 | left column, 3rd (32 / 33) | yellow ECS, white D/C | 32, 33 |
| E-ink, cable 4 | left column, top (26 / 27) | yellow BUSY, white RST | 26, 27 |

"(34 / 35)" means 34 is printed above 35. The e-ink cables are Grove-to-jumper cables to the eInk Breakout Friend; its MISO stays unconnected (no SRAM is used), and the wires that go nowhere are insulated.

**Board B, `Alarm board`** (`firmware/AlarmBoard/`), not yet tested on hardware:

| Module | Port (printed numbers) | GPIO |
|---|---|---|
| Chainable RGB LED | right column 2, top (17 16) | 16 CI, 17 DI |
| Buzzer | right column 1, 2nd (14 13) | 13 |

The buzzer stays off GPIO 2, 5, 14 and 15: they change during startup, so it would chirp at every reset.

- Board A: the slider gives the room temperature, `18 + slider % × 27 / 100` °C, sent as `room_temp`. `overheat` turns true above 30.0 °C and false again below 29.5 °C. While it is true, A's LED blinks red. A sends `alarm` = `overheat` to `Alarm board` whenever it changes and every 10 s. The e-ink shows the whole degrees and "TOO HOT!".
- Board B: on `alarm` = true, 6 beeps of 100 ms with 100 ms pauses, and the LED blinks red until `alarm` = false. A repeated true doesn't beep again.

## Test board (`firmware/EinkBoard/`)

The first test, before the two-board plan and before the shield's port map was known: all four modules on one ESP32, partly on jumper wires, tested together on 2026-11-20 against the staff server. Its wiring differs from board A's (BUSY 14, buzzer 25).

| Module | In the sketch | Pins |
|---|---|---|
| E-ink display: Adafruit eInk Breakout Friend with a bare 2.66" 296x152 panel (SSD1680) | no component; actuator `eink`, library **Adafruit EPD** | CS 32, DC 33, RST 27, BUSY 14, SPI SCK 18 / MOSI 23 / MISO 19, no SRAM |
| Grove Slide Potentiometer | `GroveSlider`, sensor `slider` (0-100) | SIG 34, VCC **3V3** (analog: GPIO 32-39 only) |
| Grove Buzzer | `GroveBuzzer`, actuator `buzzer` | SIG 25. Hard to reach from the shield's Grove ports: choose another pin next time |
| Grove Chainable RGB LED | `GroveChainableRGB`, actuator `rgb`, library **Grove - Chainable RGB LED** | the LED's IN connector: DI 17 (white), CI 16 (yellow), VCC 5V |

Each sketch's `config.h` holds the WiFi password and stays out of git.

## E-ink display

- Black-and-white mode: `ThinkInk_266_Grayscale4_MFGN` with `THINKINK_MONO`; the panel shows only black. `setRotation(2)` gives landscape, 296x152.
- A full refresh takes 2.3 s (13 s in grayscale) and flashes the panel. Adafruit EPD 4.6.14 has no partial refresh for this panel; the GxEPD2 library might have one (not tried).
- A FreeRTOS task of its own draws the panel, so `loop()` keeps running during a refresh (longest gap measured: 3 ms). The command handler only stores the text; the task draws the first text at once, then the latest at most once per `MIN_DRAW_INTERVAL_MS` (1 s while testing).
- Command: `{"t": ["line 1", "line 2", ...]}`, at most 6 lines of 23 characters (text size 2). ä ö å Ä Ö Å are converted to the font's code page 437; other non-ASCII characters show as `?`. Page: `html/eink.html`.

## Decisions (2026-11-20)

- Two boards instead of one; board A tells board B directly with `ProtoFlow.sendCommand`, so no page needs to be open.
- Both boards have a Grove chainable RGB LED. The alarm beeps once per rise above 30 °C (6 × 100 ms); the LEDs blink red as long as it is above.
- Still open: the e-ink refresh interval (wanted: about 1 s; a full refresh takes 2.3 s and flashes, partial refresh would need the GxEPD2 library); the web app.
