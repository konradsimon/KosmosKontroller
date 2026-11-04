# KosmosKontroller

Joystick-MIDI-Controller (Seeed XIAO SAMD21, USB-MIDI) mit Weboberfläche zum Einstellen.

**Weboberfläche:** https://konrad-simon.com/KosmosKontroller/ – in Chrome öffnen, Controller anstecken, „Verbinden“.
Über „Installieren“ in der Adressleiste lässt sie sich als App ins Dock legen und startet dann auch offline.

## Inhalt

- `index.html` – Weboberfläche (Web MIDI, nur Chrome/Edge)
- `firmware/joystick_webconfig/` – Arduino-Sketch für den Controller
- `manifest.webmanifest`, `sw.js`, `icons/` – App-Installation und Offline-Cache

## Firmware

Benötigt in der Arduino IDE:
- Board: Seeeduino XIAO (Seeed SAMD), USB Stack: TinyUSB
- Bibliotheken: Adafruit TinyUSB, MIDI Library (FortySevenEffects), FlashStorage

Nach jedem Upload ist der Flash-Speicher leer und es gelten die Werkseinstellungen.
Vorher in der Weboberfläche **Exportieren**, danach **Importieren** und **Auf Controller speichern**.

## Funktionen

- 3 Achsen (Y, X, Rotation) mit je bis zu 4 Zielen: Pitchbend, CC ganze Achse, CC / Pitchbend nur in eine Richtung
- Pro Ziel: Kurve, Stärke, Umkehren, 14 Bit (CC 0–31)
- Taster mit bis zu 4 Zielen: CC/Note halten oder umschalten, Program Change
- 4 Presets, Wechsel am Taster per langem Druck oder 3× drücken
- Export / Import als JSON
