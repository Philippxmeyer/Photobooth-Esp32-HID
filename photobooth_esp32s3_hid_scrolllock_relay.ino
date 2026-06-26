/*
  Photobooth ESP32-S3 HID Button Box + Scroll-Lock Relay

  Funktion:
  - ESP32-S3 meldet sich per USB als HID-Tastatur.
  - 5 Taster senden Tastendrücke an Photobooth.
  - Der Raspberry Pi kann per Scroll-Lock-LED-Status ein Relais schalten.
  - Scroll Lock ON  -> Relais an
  - Scroll Lock OFF -> Relais aus

  Hardware:
  - ESP32-S3 DevKit mit nativem USB
  - Buttons jeweils zwischen GPIO und GND
  - Relaismodul an GPIO 16
  - Viele Relaismodule sind active LOW.

  Arduino IDE:
  - Board: ESP32S3 Dev Module
  - USB Mode: USB-OTG / TinyUSB
  - USB CDC On Boot: Enabled oder Disabled, je nach Bedarf
  - Flash Size passend zum Board, z. B. 16MB bei N16R8
  - PSRAM passend zum Board, z. B. OPI PSRAM bei N16R8
*/

#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

USBHIDKeyboard Keyboard;

// ------------------------------------------------------------
// Pinbelegung
// ------------------------------------------------------------

// Buttons jeweils zwischen GPIO und GND.
// Interner Pullup ist aktiv:
// nicht gedrückt = HIGH
// gedrückt       = LOW

const uint8_t BUTTON_PINS[] = {
  4,   // Button 1: Bildaufnahme
  5,   // Button 2: Collage
  6,   // Button 3: Drucken
  7,   // Button 4: Video
  15   // Button 5: Custom / frei belegbar
};

const char BUTTON_KEYS[] = {
  't', // Bildaufnahme
  'c', // Collage
  'p', // Drucken
  'v', // Video
  'x'  // Custom / frei belegbar
};

const uint8_t BUTTON_COUNT = sizeof(BUTTON_PINS) / sizeof(BUTTON_PINS[0]);

// Relais-Ausgang.
// Scroll Lock ON  -> Relais an
// Scroll Lock OFF -> Relais aus
const uint8_t RELAY_PIN = 16;

// Viele Arduino-Relaismodule sind active LOW:
// LOW  = Relais an
// HIGH = Relais aus
const bool RELAY_ACTIVE_LOW = true;

// Optionaler Debug-LED-Pin.
// 255 bedeutet: keine Debug-LED verwenden.
const uint8_t DEBUG_LED_PIN = 255;

// Entprellzeit für Buttons
const unsigned long DEBOUNCE_MS = 35;

// ------------------------------------------------------------
// Button-Status
// ------------------------------------------------------------

struct ButtonState {
  bool stableState;
  bool lastReading;
  unsigned long lastChange;
};

ButtonState buttons[BUTTON_COUNT];

// ------------------------------------------------------------
// Hilfsfunktionen
// ------------------------------------------------------------

void setRelay(bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  }

  if (DEBUG_LED_PIN != 255) {
    digitalWrite(DEBUG_LED_PIN, on ? HIGH : LOW);
  }
}

void sendKey(char key) {
  Keyboard.press(key);
  delay(25);
  Keyboard.releaseAll();
}

void handleButtonPress(uint8_t index) {
  if (index >= BUTTON_COUNT) {
    return;
  }

  sendKey(BUTTON_KEYS[index]);
}

// ------------------------------------------------------------
// HID Keyboard LED Callback
// ------------------------------------------------------------
// Der Host, also Raspberry Pi / Windows / Linux, kann einer
// USB-Tastatur LED-Zustände senden:
// Num Lock, Caps Lock, Scroll Lock.
//
// Wir verwenden Scroll Lock als Rückkanal:
// Scroll Lock ON  -> Relais an
// Scroll Lock OFF -> Relais aus

void keyboardLedEventCallback(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data
) {
  if (event_base != ARDUINO_USB_HID_KEYBOARD_EVENTS) {
    return;
  }

  if (event_id != ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
    return;
  }

  arduino_usb_hid_keyboard_event_data_t *data =
    (arduino_usb_hid_keyboard_event_data_t *)event_data;

  bool scrollLockOn = data->scrolllock;

  setRelay(scrollLockOn);
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  // Relais sofort sicher ausschalten.
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  if (DEBUG_LED_PIN != 255) {
    pinMode(DEBUG_LED_PIN, OUTPUT);
    digitalWrite(DEBUG_LED_PIN, LOW);
  }

  // Buttons vorbereiten.
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);

    bool reading = digitalRead(BUTTON_PINS[i]);
    buttons[i].stableState = reading;
    buttons[i].lastReading = reading;
    buttons[i].lastChange = millis();
  }

  // HID-LED-Rückkanal registrieren.
  Keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT, keyboardLedEventCallback);

  // USB-HID starten.
  Keyboard.begin();
  USB.begin();
}

// ------------------------------------------------------------
// Main Loop
// ------------------------------------------------------------

void loop() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    bool reading = digitalRead(BUTTON_PINS[i]);

    if (reading != buttons[i].lastReading) {
      buttons[i].lastReading = reading;
      buttons[i].lastChange = now;
    }

    if ((now - buttons[i].lastChange) > DEBOUNCE_MS) {
      if (reading != buttons[i].stableState) {
        buttons[i].stableState = reading;

        // Button gedrückt = LOW
        if (reading == LOW) {
          handleButtonPress(i);
        }
      }
    }
  }

  delay(1);
}