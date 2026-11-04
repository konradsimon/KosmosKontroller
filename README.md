# KosmosKontroller

Joystick MIDI controller (Seeed XIAO SAMD21, USB MIDI) with a web interface for configuration.

**Web interface:** https://konrad-simon.com/KosmosKontroller/ – open in Chrome, plug in the controller, click "Connect".
Use "Install" in the address bar to add it as an app; it then also starts offline.

## Contents

- `index.html` – web interface (Web MIDI, Chrome/Edge only)
- `firmware/joystick_webconfig/` – Arduino sketch for the controller
- `manifest.webmanifest`, `sw.js`, `icons/` – app installation and offline cache

## Firmware

Required in the Arduino IDE:
- Board: Seeeduino XIAO (Seeed SAMD), USB stack: TinyUSB
- Libraries: Adafruit TinyUSB, MIDI Library (FortySevenEffects), FlashStorage

Every upload clears the flash memory and the factory defaults apply.
**Export** in the web interface first, then **Import** and **Save to controller** afterwards.

## Features

- 3 axes (Y, X, rotation) with up to 4 targets each: pitch bend, CC full axis, CC / pitch bend in one direction only
- Per target: curve, amount (pitch bend in semitones, relative to a ±12 bend range), invert, 14-bit (CC 0–31), mute
- Button with up to 4 targets: CC / note momentary or toggle, program change
- 4 presets, switched on the button by long press or triple press
- Export / import as JSON
