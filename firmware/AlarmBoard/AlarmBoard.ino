/*
 * AlarmBoard.ino - Team 8's board B, the alarm. Board A ("Room board", firmware/RoomBoard/)
 * sends a Boolean alarm command for high or low temperature (ProtoFlow.sendCommand).
 * A web page can send the same command.
 *
 * On true, the buzzer beeps 6 times, 100 ms on and 100 ms off (about 1.2 s), and the RGB LED
 * blinks red until false comes. Another true while the alarm is on changes nothing, so board A
 * can repeat its state. The pattern is timed here, with millis(), because 100 ms beeps can't be
 * timed over the network.
 *
 * Actuators: "alarm" (boolean, this sketch's own, as in docs/guides/add-custom-actuator.md) and
 * the components' "buzzer" and "rgb".
 *
 * WIRING on the Grove-Base Shield for ESP32 v1.1. Every module goes into a Grove port with a
 * Grove cable. A port's pins are GND, VCC, white, yellow; the yellow wire (a module's signal) is
 * the GPIO printed over the last pin: the right number in the right columns.
 *
 *   Module           Port (printed numbers)          GPIO
 *   Chainable RGB    right column 2, top (17 16)     16 CI (yellow), 17 DI (white); into the LED's IN
 *   Buzzer           right column 1, 2nd (14 13)     13
 *
 * The buzzer stays off GPIO 2, 5, 14 and 15: they change during startup, and the buzzer would
 * chirp at every reset.
 *
 * SETUP: create config.h from config.example.h (TEAM-GUIDE.md, part 2). It holds your WiFi
 * password, so .gitignore keeps it out of git.
 */

#include <ProtoFlow.h>
#include "config.h"

// The name this board shows in Node-RED and the web app. Board A sends its alarm to this name.
#define DEVICE_NAME "Alarm board"

// ---------------------------------------------------------------------------------------------
// Modules (docs/components.md), in the shield's Grove ports (WIRING above)
// ---------------------------------------------------------------------------------------------
#include <components/GroveChainableRGB.h>   // library: Grove - Chainable RGB LED
#include <components/GroveBuzzer.h>

//                        name   data_pin  clock_pin  leds
GroveChainableRGB rgb = {"rgb",  17,       16,        1};   // port (17 16), right column 2, top

//                    name      pin
GroveBuzzer buzzer = {"buzzer", 13};                        // port (14 13), right column 1, 2nd

// ---------------------------------------------------------------------------------------------
// Alarm
// ---------------------------------------------------------------------------------------------
const int BEEPS = 6;
const unsigned long BEEP_MS = 100;    // on for this long, then off for as long
const unsigned long BLINK_MS = 500;

bool alarmOn = false;
int beepStepsLeft = 0;                // two steps per beep: on, off
unsigned long lastBeepStepMs = 0;
bool ledOn = false;

void setup() {
    Serial.begin(115200);
    delay(1000);

    ProtoFlow.configureDevice(DEVICE_NAME, "ESP32");

    rgb.setup();
    buzzer.setup();

    //                    name     type     data_type  min  max  unit  description
    ProtoFlow.addActuator("alarm", "alarm", "boolean", 0,   1,   "",   "Room too hot: beeps, then blinks red");
    ProtoFlow.onActuator("alarm", [](JsonVariant value) {
        bool on = value.as<bool>();
        if (on && !alarmOn) {
            beepStepsLeft = BEEPS * 2;
            lastBeepStepMs = millis() - BEEP_MS;   // the first beep at once
        }
        if (!on && alarmOn) {
            beepStepsLeft = 0;
            buzzer.off();
        }
        if (on != alarmOn) PF_INFO(String("alarm: ") + (on ? "on" : "off"));
        alarmOn = on;
    });

    ProtoFlow.configureWiFi(WIFI_SSID, WIFI_PASSWORD);
    ProtoFlow.configureWebSocket(WEBSOCKET_URL, ROOT_CA_CERT);

    ProtoFlow.onConnect([]() {
        ProtoFlow.sendDeviceRegistration();
    });

    ProtoFlow.onRegistration([](bool accepted, String message) {
        if (!accepted) {
            PF_ERROR("Registration failed: " + message);
        }
    });

    ProtoFlow.begin();
}

void loop() {
    // Must run often: it keeps the connection alive and delivers commands to the modules
    ProtoFlow.update();
    rgb.loop();
    buzzer.loop();

    // Beeps: each step switches the buzzer, every BEEP_MS (odd steps on, even steps off)
    if (beepStepsLeft > 0 && millis() - lastBeepStepMs >= BEEP_MS) {
        lastBeepStepMs = millis();
        beepStepsLeft--;
        if (beepStepsLeft % 2 == 1) buzzer.on();
        else buzzer.off();
    }

    // LED: blinks red while the alarm is on, off otherwise
    bool blinkOn = alarmOn && (millis() / BLINK_MS) % 2 == 0;
    if (blinkOn != ledOn) {
        ledOn = blinkOn;
        if (ledOn) rgb.setColor(255, 0, 0);
        else rgb.off();
    }

    if (!ProtoFlow.isOperational()) {
        delay(10);
        return;
    }
}
