/*
 * RoomBoard.ino - Team 8's board A: a Grove slider simulates the room temperature, 18-45 °C,
 * which the 2.66" e-ink display shows. Above 30 °C the board's RGB LED blinks red, and it tells
 * board B ("Alarm board", firmware/AlarmBoard/) to sound the alarm.
 *
 * Sends to the web app: "room_temp" (°C, 18-45), "overheat" (true above 30 °C, false again only
 * below 29.5 °C, so that a slider resting at 30 °C doesn't toggle it), and the slider component's
 * own "slider" (0-100 %). Sends to board B: alarm = true / false whenever overheat changes, and
 * the current value again every 10 s, so that board B catches up after a restart
 * (ProtoFlow.sendCommand, library 1.4.0 or newer). No web page needs to be open.
 *
 * The display shows the temperature in whole degrees and "TOO HOT!". A refresh takes 2.3 s, so a
 * FreeRTOS task of its own draws it and loop() keeps running. The display, the LED and the alarm
 * work without a connection too.
 *
 * WIRING on the Grove-Base Shield for ESP32 v1.1. Every cable goes into a Grove port. A port's
 * pins are GND, VCC, white, yellow; the yellow wire (a module's signal) is the GPIO printed over
 * the last pin: the right number in the right columns, the upper one in the left and middle
 * columns. "(34 / 35)" below means 34 is printed above 35.
 *
 *   Module           Port (printed numbers)                  Wires                          GPIO
 *   Slider           middle column, 1st below I2C (34 / 35)  Grove cable                    34
 *   Chainable RGB    right column 2, top (17 16)             Grove cable into the LED's IN  16 CI, 17 DI
 *   E-ink, cable 1   right column 2, 3rd (19 18)             yellow SCK, red VIN, black GND 18
 *   E-ink, cable 2   right column 2, bottom (25 23)          yellow MOSI                    23
 *   E-ink, cable 3   left column, 3rd (32 / 33)              yellow ECS, white D/C          32, 33
 *   E-ink, cable 4   left column, top (26 / 27)              yellow BUSY, white RST         26, 27
 *
 * The e-ink cables are Grove-to-jumper cables to the pins of the Adafruit eInk Breakout Friend.
 * Leave its MISO unconnected (no SRAM is used), and insulate the wires that go nowhere: the white
 * wires of cables 1 and 2 and the red and black wires of cables 2-4 (those carry power).
 *
 * SETUP: create config.h from config.example.h (TEAM-GUIDE.md, part 2). It holds your WiFi
 * password, so .gitignore keeps it out of git.
 */

#include <ProtoFlow.h>
#include "config.h"

// The name this board shows in Node-RED and the web app
#define DEVICE_NAME "Room board"
// The DEVICE_NAME of board B, which sounds the alarm
#define ALARM_BOARD "Alarm board"

// ---------------------------------------------------------------------------------------------
// Modules (docs/components.md), in the shield's Grove ports (WIRING above)
// ---------------------------------------------------------------------------------------------
#include <components/GroveSlider.h>
#include <components/GroveChainableRGB.h>   // library: Grove - Chainable RGB LED

//                     name      pin  ema_alpha  update_rate_ms  change_threshold
GroveSlider slider = {"slider", 34,  0.2,       100,            0.5};   // port (34 / 35), middle column

//                        name   data_pin  clock_pin  leds
GroveChainableRGB rgb = {"rgb",  17,       16,        1};               // port (17 16), right column 2, top

// ---------------------------------------------------------------------------------------------
// E-ink display: no Grove component, driven with the Adafruit EPD library (WIRING above)
// ---------------------------------------------------------------------------------------------
#include "Adafruit_ThinkInk.h"

#define EPD_CS     32    // cable 3, yellow: port (32 / 33), left column
#define EPD_DC     33    // cable 3, white
#define SRAM_CS    -1    // no SRAM chip used
#define EPD_RESET  27    // cable 4, white: port (26 / 27), left column
#define EPD_BUSY   26    // cable 4, yellow
// SPI uses the board's default pins: SCK 18 (cable 1, yellow), MOSI 23 (cable 2, yellow)

ThinkInk_266_Grayscale4_MFGN display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
const unsigned long MIN_DRAW_INTERVAL_MS = 1000;   // a refresh itself takes 2.3 s

// ---------------------------------------------------------------------------------------------
// Temperature and alarm
// ---------------------------------------------------------------------------------------------
const float TEMP_MIN = 18.0;              // slider at 0 %
const float TEMP_MAX = 45.0;              // slider at 100 %
const float ALARM_ON_ABOVE = 30.0;
const float ALARM_OFF_BELOW = 29.5;
const unsigned long ALARM_RESEND_MS = 10000;
const unsigned long BLINK_MS = 500;
const unsigned long TEMP_SEND_MIN_MS = 200;

float roomTemp = TEMP_MIN;
bool overheat = false;
float lastSentTemp = -100;
unsigned long lastTempSendMs = 0;
unsigned long lastAlarmSendMs = 0;
bool ledOn = false;

// What the display should show. Written by loop() and read by drawTask, both under panelLock.
int panelTemp = -100;          // whole degrees; -100 = nothing yet
bool panelOverheat = false;
bool panelChanged = false;
SemaphoreHandle_t panelLock;

float temperatureFromSlider() {
    return TEMP_MIN + slider.getValue() * (TEMP_MAX - TEMP_MIN) / 100.0f;
}

void drawPanel(int temp, bool hot) {
    display.clearBuffer();
    display.setTextColor(EPD_BLACK);
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.print("Room temperature");

    display.setTextSize(7);            // 42 x 56 pixels per character
    display.setCursor(10, 40);
    display.print(temp);
    display.setTextSize(3);
    display.write(0xF8);               // degree sign in the built-in font (code page 437)
    display.print("C");

    if (hot) {                         // white on a black band
        display.fillRect(0, 112, display.width(), 40, EPD_BLACK);
        display.setTextColor(EPD_WHITE);
        display.setTextSize(3);
        display.setCursor(10, 120);
        display.print("TOO HOT!");
    }
    unsigned long startMs = millis();
    display.display();                 // blocks this task until the refresh is over
    PF_INFO("eink: " + String(temp) + " C" + (hot ? ", too hot" : "") + ", drawn in " + String(millis() - startMs) + " ms");
}

// The drawing task. Only it touches the display after setup(). While a refresh runs, the
// library waits for the BUSY pin with delay(), which lets loop() run on the other core.
void drawTask(void*) {
    unsigned long lastDrawMs = 0;   // 0 = nothing drawn yet
    for (;;) {
        int temp = 0;
        bool hot = false;
        bool draw = false;
        if (lastDrawMs == 0 || millis() - lastDrawMs >= MIN_DRAW_INTERVAL_MS) {
            xSemaphoreTake(panelLock, portMAX_DELAY);
            if (panelChanged) {
                temp = panelTemp;
                hot = panelOverheat;
                panelChanged = false;
                draw = true;
            }
            xSemaphoreGive(panelLock);
        }
        if (draw) {
            lastDrawMs = millis();
            drawPanel(temp, hot);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void sendAlarmState() {
    lastAlarmSendMs = millis();
    ProtoFlow.sendCommand(ALARM_BOARD, "alarm", overheat);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    ProtoFlow.configureDevice(DEVICE_NAME, "ESP32");

    slider.setup();
    rgb.setup();

    // Values this board computes itself (docs/guides/add-custom-sensor.md)
    //                  name         type           data_type  min       max       unit  description
    ProtoFlow.addSensor("room_temp", "temperature", "float",   TEMP_MIN, TEMP_MAX, "°C", "Room temperature (simulated by the slider)");
    ProtoFlow.addSensor("overheat",  "state",       "boolean", 0,        1,        "",   "Room above 30 °C");

    // E-ink display. It keeps its last image without power until the first reading is drawn.
    display.begin(THINKINK_MONO);   // black and white only: refresh 2.3 s instead of 13 s
    display.setRotation(2);         // landscape, 296x152
    display.cp437(true);            // the degree sign is 0xF8 in code page 437
    display.setTextWrap(false);
    panelLock = xSemaphoreCreateMutex();
    // Core 0, the other core than loop(); priority 1, below WiFi's
    xTaskCreatePinnedToCore(drawTask, "eink", 8192, nullptr, 1, nullptr, 0);

    ProtoFlow.configureWiFi(WIFI_SSID, WIFI_PASSWORD);
    ProtoFlow.configureWebSocket(WEBSOCKET_URL, ROOT_CA_CERT);

    ProtoFlow.onConnect([]() {
        ProtoFlow.sendDeviceRegistration();
    });

    ProtoFlow.onRegistration([](bool accepted, String message) {
        if (accepted) {
            slider.sendInitial();
            ProtoFlow.sendSensorData("room_temp", roomTemp);
            ProtoFlow.sendSensorData("overheat", overheat);
            lastSentTemp = roomTemp;
            sendAlarmState();
        } else {
            PF_ERROR("Registration failed: " + message);
        }
    });

    ProtoFlow.begin();
}

void loop() {
    // Must run often: it keeps the connection alive and delivers commands to the modules
    ProtoFlow.update();
    slider.loop();
    rgb.loop();

    // Temperature and overheat, with hysteresis
    roomTemp = temperatureFromSlider();
    bool wasOverheat = overheat;
    if (!overheat && roomTemp > ALARM_ON_ABOVE) overheat = true;
    if (overheat && roomTemp < ALARM_OFF_BELOW) overheat = false;

    // LED: blinks red while overheat, off otherwise
    bool blinkOn = overheat && (millis() / BLINK_MS) % 2 == 0;
    if (blinkOn != ledOn) {
        ledOn = blinkOn;
        if (ledOn) rgb.setColor(255, 0, 0);
        else rgb.off();
    }

    // Display: when the whole degrees or overheat change; drawTask draws the latest
    int wholeDegrees = (int)lroundf(roomTemp);
    if (wholeDegrees != panelTemp || overheat != panelOverheat) {
        xSemaphoreTake(panelLock, portMAX_DELAY);
        panelTemp = wholeDegrees;
        panelOverheat = overheat;
        panelChanged = true;
        xSemaphoreGive(panelLock);
    }

    if (!ProtoFlow.isOperational()) {
        delay(10);
        return;
    }

    // To the web app and board B
    if (overheat != wasOverheat) {
        PF_INFO(String("overheat: ") + (overheat ? "true" : "false") + " at " + String(roomTemp, 1) + " C");
        ProtoFlow.sendSensorData("overheat", overheat);
        sendAlarmState();
    } else if (millis() - lastAlarmSendMs >= ALARM_RESEND_MS) {
        sendAlarmState();
    }
    if (fabsf(roomTemp - lastSentTemp) >= 0.1f && millis() - lastTempSendMs >= TEMP_SEND_MIN_MS) {
        lastTempSendMs = millis();
        lastSentTemp = roomTemp;
        ProtoFlow.sendSensorData("room_temp", roomTemp);
    }
}
