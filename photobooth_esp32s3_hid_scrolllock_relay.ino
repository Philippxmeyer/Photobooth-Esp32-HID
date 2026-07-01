/*
  Photobooth ESP32-S3 HID Button Box + Scroll-Lock Relay

  Funktion:
  - ESP32-S3 meldet sich per USB als HID-Tastatur.
  - 5 Taster senden Tastendrücke an Photobooth.
  - Der Raspberry Pi kann per Scroll-Lock-LED-Status einen Ausgang schalten.
  - Scroll Lock ON  -> Ausgang an
  - Scroll Lock OFF -> Ausgang aus
  - Der Ausgang kann als Relais oder als PWM-gesteuerter MOSFET betrieben werden.
  - Im MOSFET-Modus kann der PWM-Duty-Cycle per Potentiometer oder über
    einen gespeicherten Festwert vorgegeben werden.
  - Der Festwert kann per serieller Schnittstelle gesetzt und im Flash
    dauerhaft gespeichert werden.

  Hardware:
  - ESP32-S3 DevKit mit nativem USB
  - Buttons jeweils zwischen GPIO und GND
  - Relaismodul oder MOSFET-Gate an GPIO 16
  - Potentiometer-Schleifer für MOSFET-PWM an GPIO 1 / ADC1_CH0
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
#include <Preferences.h>

USBHIDKeyboard Keyboard;
Preferences preferences;

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

// Schaltausgang.
// Scroll Lock ON  -> Ausgang an
// Scroll Lock OFF -> Ausgang aus
const uint8_t OUTPUT_PIN = 16;

// Betriebsart des Ausgangs:
// OUTPUT_MODE_RELAY  = klassisches digitales Relais
// OUTPUT_MODE_MOSFET = MOSFET per PWM, Duty-Cycle über Potentiometer
enum OutputMode {
  OUTPUT_MODE_RELAY,
  OUTPUT_MODE_MOSFET
};

const OutputMode OUTPUT_MODE = OUTPUT_MODE_RELAY;

// Viele Arduino-Relaismodule sind active LOW:
// LOW  = Relais an
// HIGH = Relais aus
const bool RELAY_ACTIVE_LOW = true;

// MOSFET-PWM-Konfiguration.
// Das MOSFET-Gate wird aktiv HIGH angesteuert.
const uint8_t POTI_PIN = 1;
const uint32_t MOSFET_PWM_FREQUENCY_HZ = 1000;
const uint8_t MOSFET_PWM_RESOLUTION_BITS = 8;
const uint16_t MOSFET_PWM_MAX_DUTY = (1 << MOSFET_PWM_RESOLUTION_BITS) - 1;

// Duty-Cycle-Steuerung im MOSFET-Modus.
// true  = Duty-Cycle laufend vom Potentiometer lesen
// false = gespeicherten Festwert verwenden
const bool MOSFET_USE_POTI_DUTY_CYCLE = false;

// Fest programmierter Startwert, falls noch kein Wert gespeichert wurde.
// Der Wert liegt im Bereich 0..MOSFET_PWM_MAX_DUTY.
const uint16_t MOSFET_DEFAULT_FIXED_DUTY = 128;

// Einfache serielle Befehle bei 115200 Baud:
//   duty?       -> aktuellen Festwert anzeigen
//   duty 128    -> Festwert 0..255 speichern
//   duty 50%    -> Festwert als Prozentwert speichern
const uint32_t SERIAL_BAUD_RATE = 115200;
const char *PREFERENCES_NAMESPACE = "mosfet";
const char *PREFERENCES_DUTY_KEY = "duty";

uint16_t fixedMosfetDutyCycle = MOSFET_DEFAULT_FIXED_DUTY;
String serialCommandBuffer;

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
bool outputEnabled = false;

void writeMosfetPwm(uint16_t dutyCycle);

// ------------------------------------------------------------
// Hilfsfunktionen
// ------------------------------------------------------------

uint16_t constrainMosfetDutyCycle(uint16_t dutyCycle) {
  return dutyCycle > MOSFET_PWM_MAX_DUTY ? MOSFET_PWM_MAX_DUTY : dutyCycle;
}

uint16_t readPotiMosfetDutyCycle() {
  uint16_t potiValue = analogRead(POTI_PIN);

  return map(potiValue, 0, 4095, 0, MOSFET_PWM_MAX_DUTY);
}

uint16_t readMosfetDutyCycle() {
  if (MOSFET_USE_POTI_DUTY_CYCLE) {
    return readPotiMosfetDutyCycle();
  }

  return fixedMosfetDutyCycle;
}

void printMosfetDutyCycle() {
  Serial.print(F("MOSFET fixed duty: "));
  Serial.print(fixedMosfetDutyCycle);
  Serial.print(F(" / "));
  Serial.print(MOSFET_PWM_MAX_DUTY);
  Serial.print(F(" ("));
  Serial.print((fixedMosfetDutyCycle * 100UL) / MOSFET_PWM_MAX_DUTY);
  Serial.println(F("%)"));
}

bool parseDutyCycleValue(String value, uint16_t &dutyCycle) {
  value.trim();

  if (value.length() == 0) {
    return false;
  }

  bool isPercent = value.endsWith("%");
  if (isPercent) {
    value.remove(value.length() - 1);
    value.trim();
  }

  for (uint16_t i = 0; i < value.length(); i++) {
    if (!isDigit(value.charAt(i))) {
      return false;
    }
  }

  unsigned long parsedValue = value.toInt();

  if (isPercent) {
    if (parsedValue > 100) {
      return false;
    }

    dutyCycle = (parsedValue * MOSFET_PWM_MAX_DUTY) / 100;
    return true;
  }

  if (parsedValue > MOSFET_PWM_MAX_DUTY) {
    return false;
  }

  dutyCycle = parsedValue;
  return true;
}

void handleSerialCommand(String command) {
  command.trim();

  if (command.length() == 0) {
    return;
  }

  command.toLowerCase();

  if (command == "duty?" || command == "duty") {
    printMosfetDutyCycle();
    return;
  }

  if (command.startsWith("duty ")) {
    uint16_t newDutyCycle = 0;

    if (!parseDutyCycleValue(command.substring(5), newDutyCycle)) {
      Serial.println(F("ERROR: use duty 0..255 or duty 0..100%"));
      return;
    }

    fixedMosfetDutyCycle = constrainMosfetDutyCycle(newDutyCycle);
    preferences.putUShort(PREFERENCES_DUTY_KEY, fixedMosfetDutyCycle);

    if (OUTPUT_MODE == OUTPUT_MODE_MOSFET && outputEnabled) {
      writeMosfetPwm(fixedMosfetDutyCycle);
    }

    Serial.println(F("OK: duty saved"));
    printMosfetDutyCycle();
    return;
  }

  Serial.println(F("ERROR: unknown command. Use duty? or duty <0..255|0..100%>"));
}

void handleSerialInput() {
  while (Serial.available() > 0) {
    char incomingChar = Serial.read();

    if (incomingChar == '\r' || incomingChar == '\n') {
      handleSerialCommand(serialCommandBuffer);
      serialCommandBuffer = "";
    } else if (serialCommandBuffer.length() < 64) {
      serialCommandBuffer += incomingChar;
    }
  }
}

void writeMosfetPwm(uint16_t dutyCycle) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(OUTPUT_PIN, dutyCycle);
#else
  ledcWrite(0, dutyCycle);
#endif
}

void setOutput(bool on) {
  outputEnabled = on;

  if (OUTPUT_MODE == OUTPUT_MODE_RELAY) {
    if (RELAY_ACTIVE_LOW) {
      digitalWrite(OUTPUT_PIN, on ? LOW : HIGH);
    } else {
      digitalWrite(OUTPUT_PIN, on ? HIGH : LOW);
    }
  } else {
    writeMosfetPwm(on ? readMosfetDutyCycle() : 0);
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
// Scroll Lock ON  -> Ausgang an
// Scroll Lock OFF -> Ausgang aus

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

  setOutput(scrollLockOn);
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  // Ausgang sofort sicher ausschalten.
  pinMode(OUTPUT_PIN, OUTPUT);

  Serial.begin(SERIAL_BAUD_RATE);

  preferences.begin(PREFERENCES_NAMESPACE, false);
  fixedMosfetDutyCycle = constrainMosfetDutyCycle(
    preferences.getUShort(PREFERENCES_DUTY_KEY, MOSFET_DEFAULT_FIXED_DUTY)
  );

  if (OUTPUT_MODE == OUTPUT_MODE_MOSFET) {
    if (MOSFET_USE_POTI_DUTY_CYCLE) {
      pinMode(POTI_PIN, INPUT);
      analogReadResolution(12);
    }
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(OUTPUT_PIN, MOSFET_PWM_FREQUENCY_HZ, MOSFET_PWM_RESOLUTION_BITS);
#else
    ledcSetup(0, MOSFET_PWM_FREQUENCY_HZ, MOSFET_PWM_RESOLUTION_BITS);
    ledcAttachPin(OUTPUT_PIN, 0);
#endif
  }

  setOutput(false);

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

  handleSerialInput();

  if (OUTPUT_MODE == OUTPUT_MODE_MOSFET) {
    writeMosfetPwm(outputEnabled ? readMosfetDutyCycle() : 0);
  }

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
