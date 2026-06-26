# Photobooth-Esp32-HID

# Photobooth ESP32-S3 HID Button Box mit Scroll-Lock-Relais
Dieses Projekt ersetzt die alte GPIO-Button-/Relais-Funktion von Photobooth durch einen externen ESP32-S3.
Der ESP32-S3 arbeitet als USB-HID-Tastatur:
- Taster senden normale Tastendrücke an Photobooth.
- Der Raspberry Pi sendet über den Scroll-Lock-LED-Status ein Signal zurück an den ESP32-S3.
- Der ESP32-S3 schaltet damit ein Relais oder eine Statuslampe.
## Funktionsprinzip
```text
ESP32-S3 Button
    ↓
USB-HID-Tastatur
    ↓
Raspberry Pi / Chromium / Photobooth
    ↓
Photobooth startet Bildaufnahme
Photobooth "Script vor Bildaufnahme"
    ↓
Scroll Lock ON
    ↓
ESP32-S3 empfängt HID-LED-Status
    ↓
Relais an
Photobooth "Script nach Bildaufnahme"
    ↓
Scroll Lock OFF
    ↓
ESP32-S3 empfängt HID-LED-Status
    ↓
Relais aus

Hardware

Verwendet wurde ein ESP32-S3 DevKit mit nativem USB.

Geeignet sind z. B.:

* ESP32-S3 DevKitC
* ESP32-S3-WROOM Dev Board
* ESP32-S3 N16R8 Dev Board

Wichtig: Der ESP32-S3 muss per nativer USB-Buchse als HID-Gerät betrieben werden können.

Pinbelegung

Buttons jeweils zwischen GPIO und GND anschließen.

Funktion	GPIO	Taste
Bildaufnahme	GPIO 4	t
Collage	GPIO 5	c
Drucken	GPIO 6	p
Video	GPIO 7	v
Custom / frei	GPIO 15	x
Relais	GPIO 16	Scroll Lock

Die Buttons verwenden interne Pullups:

GPIO ---- Button ---- GND

Das Relaismodul ist im Sketch als active LOW konfiguriert:

const bool RELAY_ACTIVE_LOW = true;

Bei einem active-high Relaismodul muss dieser Wert auf false geändert werden.

Achtung bei 230 V

Wenn mit dem Relais Netzspannung geschaltet wird:

* nur geeignete Relais-/SSR-Module verwenden
* Netzspannung berührungssicher einhausen
* Zugentlastung verwenden
* Schutzleiter korrekt führen
* Kleinspannung und Netzspannung räumlich sauber trennen
* keine offenen 230-V-Klemmen im Gehäuse zugänglich lassen

Die ESP32-Seite ist Kleinspannung. Die Netzspannungsseite muss separat sicher aufgebaut werden.

Arduino IDE Einstellungen

Empfohlene Einstellungen:

Board:            ESP32S3 Dev Module
USB Mode:         USB-OTG / TinyUSB
USB CDC On Boot:  Enabled oder Disabled
Flash Size:       passend zum Board, z. B. 16MB bei N16R8
PSRAM:            passend zum Board, z. B. OPI PSRAM bei N16R8

Zum Flashen wird bei vielen Boards die COM-USB-Buchse verwendet.

Für den späteren Betrieb am Raspberry Pi wird die native USB-Buchse verwendet.

Photobooth-Tasten

In Photobooth können die normalen Browser-Keycodes verwendet werden.

Taste	Browser-Keycode
t	84
c	67
p	80
v	86
x	88

Beispiel:

Bildaufnahme: 84
Collage:      67
Drucken:      80
Video:        86
Custom:       88

Scroll-Lock-Helferscript auf dem Raspberry Pi

Auf dem Raspberry Pi wird ein kleines Script angelegt, das den Scroll-Lock-LED-Status des ESP32-S3-Keyboards setzt.

Datei anlegen:

sudo nano /usr/local/bin/scrolllock-relay

Inhalt:

#!/usr/bin/env python3
import sys
from evdev import InputDevice, ecodes
DEVICE = "/dev/input/by-id/usb-Espressif_Systems_ESP32S3_DEV_E83DC1F2DA08-event-kbd"
if len(sys.argv) != 2 or sys.argv[1] not in ("on", "off"):
    print("Usage: scrolllock-relay on|off")
    sys.exit(1)
state = 1 if sys.argv[1] == "on" else 0
dev = InputDevice(DEVICE)
dev.write(ecodes.EV_LED, ecodes.LED_SCROLLL, state)
dev.syn()

Den Gerätepfad DEVICE anpassen.

Der richtige Pfad kann mit folgendem Befehl gefunden werden:

ls -l /dev/input/by-id/

Gesucht wird ein Eintrag mit -event-kbd, z. B.:

/dev/input/by-id/usb-Espressif_Systems_ESP32S3_DEV_E83DC1F2DA08-event-kbd

Script ausführbar machen:

sudo chmod +x /usr/local/bin/scrolllock-relay

Benötigte Pakete

sudo apt update
sudo apt install python3-evdev

Test

sudo /usr/local/bin/scrolllock-relay on
sleep 2
sudo /usr/local/bin/scrolllock-relay off

Das Relais sollte einschalten und danach wieder ausschalten.

Ausführung durch Photobooth erlauben

Photobooth läuft typischerweise als User www-data.

Damit Photobooth das Script mit den nötigen Rechten ausführen darf, wird eine sudoers-Regel angelegt:

sudo visudo

Am Ende einfügen:

www-data ALL=(root) NOPASSWD: /usr/local/bin/scrolllock-relay

Test:

sudo -u www-data sudo /usr/local/bin/scrolllock-relay on
sleep 2
sudo -u www-data sudo /usr/local/bin/scrolllock-relay off

Wenn das Relais klickt, kann Photobooth das Script verwenden.

Photobooth konfigurieren

Im Photobooth-Adminpanel unter den Befehlen:

Script / Befehl vor Bildaufnahme

sudo /usr/local/bin/scrolllock-relay on

Script / Befehl nach Bildaufnahme

sudo /usr/local/bin/scrolllock-relay off

Damit wird das Relais direkt vor der eigentlichen Aufnahme eingeschaltet und nach der Aufnahme wieder ausgeschaltet.

Optional: Debug

Prüfen, ob der ESP32-S3 als Eingabegerät erkannt wird:

lsusb
ls -l /dev/input/by-id/

Prüfen, wohin der by-id-Link zeigt:

readlink -f /dev/input/by-id/usb-Espressif_Systems_ESP32S3_DEV_E83DC1F2DA08-event-kbd

Direkter Scroll-Lock-Test:

sudo python3 - <<'PY'
from evdev import InputDevice, ecodes
import time
dev = InputDevice("/dev/input/by-id/usb-Espressif_Systems_ESP32S3_DEV_E83DC1F2DA08-event-kbd")
dev.write(ecodes.EV_LED, ecodes.LED_SCROLLL, 1)
dev.syn()
time.sleep(2)
dev.write(ecodes.EV_LED, ecodes.LED_SCROLLL, 0)
dev.syn()
PY

Hinweis

Dieser Weg benötigt keinen Photobooth-GPIO-Support und keinen Remote-Buzzer-Server.

Der ESP32-S3 übernimmt die Hardwareseite, Photobooth sieht nur eine normale USB-Tastatur.

---
Eine kleine Anpassung musst Du noch machen: In der README steht aktuell Dein konkreter Gerätepfad:
```text
usb-Espressif_Systems_ESP32S3_DEV_E83DC1F2DA08-event-kbd

Für ein öffentliches Repo würde ich den vielleicht als Beispiel markieren, weil die ID bei anderen Boards anders aussehen kann.