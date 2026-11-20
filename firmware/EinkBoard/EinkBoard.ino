/*
 * EinkBoard.ino - Team 8's test board: the 2.66" e-ink display, a Grove slider, a Grove buzzer
 * and a Grove chainable RGB LED on one ESP32, all tested together on 2026-11-20.
 *
 * The slider is the sensor "slider" (0-100 %), the buzzer and the LED are the actuators "buzzer"
 * and "rgb" (docs/components.md). The display is the actuator "eink" (type text_panel, data_type
 * object), so a web page can put up to 6 lines of text on it:
 *     ProtoFlow.sendCommand(uuid, 'eink', { t: ['line 1', 'line 2'] });
 * html/eink.html is that page. Missing lines stay empty, and each line is cut to 23 characters.
 * A refresh takes 2.3 s, so the command handler only stores the text, and a FreeRTOS task of its
 * own draws it: the first text at once, later ones at most once per MIN_DRAW_INTERVAL_MS, always
 * the latest. loop() keeps running while the panel refreshes, so ProtoFlow and the other modules
 * don't wait for it.
 *
 * The display (Adafruit eInk Breakout Friend, bare 2.66" 296x152 SSD1680 panel, used in
 * black-and-white mode) has no Grove component, so it is registered as in
 * docs/guides/add-custom-actuator.md and drawn with its own library, Adafruit EPD.
 * The wiring of everything is in TEAM-NOTES.md.
 *
 * Add a module: look it up in docs/components.md, under "In a sketch". Copy its #include and
 * its declaration below, change the pin to where the module is wired, and add its calls in the
 * places marked ADD. GPIO 32 and 33 are the display's: give other modules other pins.
 *
 * SETUP: create config.h from config.example.h (TEAM-GUIDE.md, part 2). It holds your WiFi
 * password, so .gitignore keeps it out of git.
 */

#include <ProtoFlow.h>
#include "config.h"

// The name this board shows in Node-RED and the web app
#define DEVICE_NAME "E-ink board"

// ---------------------------------------------------------------------------------------------
// Modules. ADD: the #include and the declaration of each module (docs/components.md)
// ---------------------------------------------------------------------------------------------
#include <components/GroveSlider.h>

//                     name      pin  ema_alpha  update_rate_ms  change_threshold
GroveSlider slider = {"slider", 34,  0.2,       100,            0.5};   // analog: GPIO 34-39 only

#include <components/GroveBuzzer.h>

//                    name      pin
GroveBuzzer buzzer = {"buzzer", 25};   // switches itself off after 5 s (maxOnMs)

#include <components/GroveChainableRGB.h>   // library: Grove - Chainable RGB LED

//                        name   data_pin  clock_pin  leds
GroveChainableRGB rgb = {"rgb",  17,       16,        1};   // the LED's IN connector: DI = white, CI = yellow

// ---------------------------------------------------------------------------------------------
// E-ink display: no Grove component, so it is driven with the Adafruit EPD library
// ---------------------------------------------------------------------------------------------
#include "Adafruit_ThinkInk.h"

#define EPD_CS     32
#define EPD_DC     33
#define SRAM_CS    -1    // no SRAM chip used
#define EPD_RESET  27
#define EPD_BUSY   14
// SPI uses the board's default pins: SCK 18, MOSI 23, MISO 19

ThinkInk_266_Grayscale4_MFGN display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
#define EPD_MODE THINKINK_MONO   // black and white only: faster than THINKINK_GRAYSCALE4

const int PANEL_LINES = 6;          // lines that fit on the panel at text size 2
const int PANEL_LINE_CHARS = 23;    // characters that fit on one line at text size 2
// TEST VALUE: 1 s, to measure how fast the panel can follow. The team's test sketch said to
// avoid refreshing more often than about every 3 minutes; decide the final value before the demo.
const unsigned long MIN_DRAW_INTERVAL_MS = 1000;

// The latest text from a command. Written by the command handler (loop's task) and read by the
// drawing task, so both take panelLock first, and hold it only to copy.
String panelText[PANEL_LINES];
bool panelChanged = false;
SemaphoreHandle_t panelLock;

// Pages send UTF-8, but the display's built-in font uses the old PC character set (code page
// 437). Keeps plain ASCII, converts the Finnish and Swedish letters, shows any other character
// as '?', and cuts the line to PANEL_LINE_CHARS.
String toPanelLine(const char* text) {
    String line;
    const unsigned char* p = (const unsigned char*)text;
    while (*p && line.length() < PANEL_LINE_CHARS) {
        if (*p < 0x80) {
            line += (char)*p++;
            continue;
        }
        char shown = '?';
        if (p[0] == 0xC3) {          // first byte of ä ö å Ä Ö Å in UTF-8
            switch (p[1]) {
                case 0xA4: shown = (char)0x84; break;   // ä
                case 0xB6: shown = (char)0x94; break;   // ö
                case 0xA5: shown = (char)0x86; break;   // å
                case 0x84: shown = (char)0x8E; break;   // Ä
                case 0x96: shown = (char)0x99; break;   // Ö
                case 0x85: shown = (char)0x8F; break;   // Å
            }
        }
        line += shown;
        p++;
        while ((*p & 0xC0) == 0x80) p++;   // skip the rest of this character's bytes
    }
    return line;
}

void drawPanel(const String lines[]) {
    display.clearBuffer();
    display.drawRect(0, 0, display.width(), display.height(), EPD_BLACK);
    display.setTextColor(EPD_BLACK);
    display.setTextSize(2);
    for (int i = 0; i < PANEL_LINES; i++) {
        display.setCursor(10, 8 + i * 24);
        display.print(lines[i]);
    }
    unsigned long startMs = millis();
    display.display();   // pushes the buffer to the panel: blocks this task until the refresh is over
    PF_INFO("eink: " + String(display.width()) + "x" + String(display.height()) + " drawn in " + String(millis() - startMs) + " ms");
}

// The drawing task. Only it touches the display after setup(). While a refresh runs, the
// library waits for the BUSY pin with delay(), which lets loop() run on the other core.
void drawTask(void*) {
    unsigned long lastDrawMs = 0;   // 0 = nothing drawn yet
    String lines[PANEL_LINES];
    for (;;) {
        bool draw = false;
        if (lastDrawMs == 0 || millis() - lastDrawMs >= MIN_DRAW_INTERVAL_MS) {
            xSemaphoreTake(panelLock, portMAX_DELAY);
            if (panelChanged) {
                for (int i = 0; i < PANEL_LINES; i++) lines[i] = panelText[i];
                panelChanged = false;
                draw = true;
            }
            xSemaphoreGive(panelLock);
        }
        if (draw) {
            lastDrawMs = millis();
            PF_INFO("eink: drawing");
            drawPanel(lines);
        }
        vTaskDelay(pdMS_TO_TICKS(20));   // check for new text 50 times a second
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    ProtoFlow.configureDevice(DEVICE_NAME, "ESP32");

    // ADD: <module>.setup(); for every module
    slider.setup();
    buzzer.setup();
    rgb.setup();

    // E-ink display. It keeps its last image without power, so nothing is drawn at startup.
    display.begin(EPD_MODE);
    display.setRotation(2);      // landscape, 296x152; each step turns the image 90 degrees clockwise
    display.cp437(true);         // the character codes toPanelLine() produces
    display.setTextWrap(false);  // a line too long for the panel is cut, not drawn over the next
    // The layout expects 296x152 (landscape). If this says 152x296, the rotation is wrong.
    PF_INFO("eink: " + String(display.width()) + "x" + String(display.height()) + " pixels");

    panelLock = xSemaphoreCreateMutex();
    // Core 0, the other core than loop(); priority 1, below WiFi's
    xTaskCreatePinnedToCore(drawTask, "eink", 8192, nullptr, 1, nullptr, 0);

    //                    name    type          data_type  min  max  unit  description
    ProtoFlow.addActuator("eink", "text_panel", "object",  0,   0,   "",   "E-ink display, 6 lines of text");

    // Runs when a command for "eink" arrives: {"t": ["line 1", "line 2", ...]}.
    // Only stores the text; drawTask draws it.
    ProtoFlow.onActuator("eink", [](JsonVariant value) {
        JsonArray lines = value["t"];
        if (lines.isNull()) {
            PF_WARN("eink expects {\"t\": [\"line 1\", \"line 2\"]}");
            return;
        }
        String converted[PANEL_LINES];
        for (int i = 0; i < PANEL_LINES; i++) {
            converted[i] = toPanelLine(lines[i] | "");   // copies the text; missing lines become empty
        }
        xSemaphoreTake(panelLock, portMAX_DELAY);
        for (int i = 0; i < PANEL_LINES; i++) panelText[i] = converted[i];
        panelChanged = true;
        xSemaphoreGive(panelLock);
        PF_INFO("eink: new text");
    });

    ProtoFlow.configureWiFi(WIFI_SSID, WIFI_PASSWORD);
    ProtoFlow.configureWebSocket(WEBSOCKET_URL, ROOT_CA_CERT);

    ProtoFlow.onConnect([]() {
        ProtoFlow.sendDeviceRegistration();
    });

    ProtoFlow.onRegistration([](bool accepted, String message) {
        if (accepted) {
            // ADD: <sensor>.sendInitial(); for every sensor (components.md says which have it)
            slider.sendInitial();
        } else {
            PF_ERROR("Registration failed: " + message);
        }
    });

    ProtoFlow.begin();
}

void loop() {
    // Must run often: it keeps the connection alive and delivers commands to the modules
    ProtoFlow.update();

    if (!ProtoFlow.isOperational()) {
        delay(10);
        return;
    }

    // ADD: <module>.loop(); for every module
    slider.loop();
    buzzer.loop();
    rgb.loop();
}
