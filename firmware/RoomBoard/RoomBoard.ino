/*
 * RoomBoard.ino - Team 8's board A. The slider simulates 10–45 °C and the
 * e-ink display shows the temperature and a hot or cold warning.
 *
 * Publishes room_temp, temperature_alarm (-1 cold, 0 normal, 1 hot), the
 * legacy overheat flag, and slider. Sends the Alarm board a boolean command
 * when the alarm state changes and every 10 seconds. No web page is needed.
 *
 * E-ink refreshes on its own task so the board stays responsive.
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
const float TEMP_MIN = 10.0;              // slider at 0 %
const float TEMP_MAX = 45.0;              // slider at 100 %
const float COLD_ALARM_AT = 15.0;
const float COLD_CLEAR_AT = 15.5;
const float HOT_ALARM_AT = 27.0;
const float HOT_CLEAR_AT = 26.5;
enum class TemperatureAlarm { Cold = -1, Normal = 0, Hot = 1 };
const unsigned long ALARM_RESEND_MS = 10000;
const unsigned long BLINK_MS = 500;
const unsigned long TEMP_SEND_MIN_MS = 200;

float roomTemp = TEMP_MIN;
TemperatureAlarm temperatureAlarm = TemperatureAlarm::Normal;
float lastSentTemp = -100;
unsigned long lastTempSendMs = 0;
unsigned long lastAlarmSendMs = 0;
bool ledOn = false;

// What the display should show. Written by loop() and read by drawTask, both under panelLock.
int panelTemp = -100;          // whole degrees; -100 = nothing yet
TemperatureAlarm panelAlarm = TemperatureAlarm::Normal;
bool panelChanged = false;
SemaphoreHandle_t panelLock;

float temperatureFromSlider() {
    return TEMP_MIN + slider.getValue() * (TEMP_MAX - TEMP_MIN) / 100.0f;
}

TemperatureAlarm alarmForTemperature(float temperature, TemperatureAlarm previous) {
    if (temperature <= COLD_ALARM_AT) return TemperatureAlarm::Cold;
    if (temperature >= HOT_ALARM_AT) return TemperatureAlarm::Hot;
    if (previous == TemperatureAlarm::Cold && temperature < COLD_CLEAR_AT) return previous;
    if (previous == TemperatureAlarm::Hot && temperature > HOT_CLEAR_AT) return previous;
    return TemperatureAlarm::Normal;
}

void drawPanel(int temp, TemperatureAlarm alarm) {
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

    if (alarm != TemperatureAlarm::Normal) {                         // white on a black band
        display.fillRect(0, 112, display.width(), 40, EPD_BLACK);
        display.setTextColor(EPD_WHITE);
        display.setTextSize(3);
        display.setCursor(10, 120);
        display.print(alarm == TemperatureAlarm::Hot ? "TOO HOT!" : "TOO COLD!");
    }
    unsigned long startMs = millis();
    display.display();                 // blocks this task until the refresh is over
    PF_INFO("eink: " + String(temp) + " C" + ", drawn in " + String(millis() - startMs) + " ms");
}

// The drawing task. Only it touches the display after setup(). While a refresh runs, the
// library waits for the BUSY pin with delay(), which lets loop() run on the other core.
void drawTask(void*) {
    unsigned long lastDrawMs = 0;   // 0 = nothing drawn yet
    for (;;) {
        int temp = 0;
        TemperatureAlarm alarm = TemperatureAlarm::Normal;
        bool draw = false;
        if (lastDrawMs == 0 || millis() - lastDrawMs >= MIN_DRAW_INTERVAL_MS) {
            xSemaphoreTake(panelLock, portMAX_DELAY);
            if (panelChanged) {
                temp = panelTemp;
                alarm = panelAlarm;
                panelChanged = false;
                draw = true;
            }
            xSemaphoreGive(panelLock);
        }
        if (draw) {
            lastDrawMs = millis();
            drawPanel(temp, alarm);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void sendAlarmState() {
    lastAlarmSendMs = millis();
    ProtoFlow.sendSensorData("temperature_alarm", static_cast<float>(temperatureAlarm));
    ProtoFlow.sendSensorData("overheat", temperatureAlarm == TemperatureAlarm::Hot);
    ProtoFlow.sendCommand(ALARM_BOARD, "alarm", temperatureAlarm != TemperatureAlarm::Normal);
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
    ProtoFlow.addSensor("overheat", "state", "boolean", 0, 1, "", "High temperature alarm active");
    ProtoFlow.addSensor("temperature_alarm", "state", "float", -1, 1, "", "Temperature alarm: -1 cold, 0 normal, 1 hot");

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
            roomTemp = temperatureFromSlider();
            temperatureAlarm = alarmForTemperature(roomTemp, temperatureAlarm);
            ProtoFlow.sendSensorData("room_temp", roomTemp);
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

    roomTemp = temperatureFromSlider();
    const TemperatureAlarm previousAlarm = temperatureAlarm;
    temperatureAlarm = alarmForTemperature(roomTemp, temperatureAlarm);

    const bool blinkOn = temperatureAlarm != TemperatureAlarm::Normal && (millis() / BLINK_MS) % 2 == 0;
    if (blinkOn != ledOn) {
        ledOn = blinkOn;
        if (ledOn) rgb.setColor(255, 0, 0);
        else rgb.off();
    }

    // Refresh when the displayed temperature or warning changes.
    int wholeDegrees = (int)lroundf(roomTemp);
    if (wholeDegrees != panelTemp || temperatureAlarm != panelAlarm) {
        xSemaphoreTake(panelLock, portMAX_DELAY);
        panelTemp = wholeDegrees;
        panelAlarm = temperatureAlarm;
        panelChanged = true;
        xSemaphoreGive(panelLock);
    }

    if (!ProtoFlow.isOperational()) {
        delay(10);
        return;
    }

    const bool refreshReadings = millis() - lastAlarmSendMs >= ALARM_RESEND_MS;
    if (temperatureAlarm != previousAlarm || refreshReadings) {
        sendAlarmState();
    }
    if (refreshReadings || (fabsf(roomTemp - lastSentTemp) >= 0.1f &&
                            millis() - lastTempSendMs >= TEMP_SEND_MIN_MS)) {
        lastTempSendMs = millis();
        lastSentTemp = roomTemp;
        ProtoFlow.sendSensorData("room_temp", roomTemp);
    }
}
