#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <FlashStorage.h>

// ------------------- USB MIDI Setup -------------------
// Eigene Settings: größere SysEx-Nachrichten (ein Preset inkl. Namen)
// und ganze Nachrichten pro read() parsen statt nur ein Byte
struct KontrollerSettings : public midi::DefaultSettings {
  static const unsigned SysExMaxSize = 700;
  static const bool Use1ByteParsing = false;
};

Adafruit_USBD_MIDI usb_midi;
midi::SerialMIDI<Adafruit_USBD_MIDI> serialMIDI(usb_midi);
midi::MidiInterface<midi::SerialMIDI<Adafruit_USBD_MIDI>, KontrollerSettings> MIDI(serialMIDI);

// ------------------- Pin-Definitionen -------------------
#define AXIS_Y_PIN       A0
#define AXIS_X_PIN       A1
#define AXIS_ROT_PIN     A2
#define BUTTON_PIN       3
#define LED_PIN          LED_BUILTIN
#define LED_ON           LOW    // XIAO: LED leuchtet bei LOW

// ------------------- Konfiguration -------------------
#define DEBOUNCE_DELAY       50
#define TAP_WINDOW           400    // ms zwischen zwei Drücken für "3x drücken"
#define SMOOTHING_WINDOW     10
#define ADC_MAX              4095   // 12 Bit
#define ADC_MID              2048
#define DEADZONE_PB          200    // entspricht 50 bei 10 Bit
#define DEADZONE_CC          90     // entspricht ~22 bei 10 Bit
#define PITCHBEND_THRESHOLD  16
#define HIRES_THRESHOLD      16     // 14-Bit-Werte erst ab dieser Änderung senden
#define MONITOR_INTERVAL     30     // ms zwischen Live-Positionen an die Weboberfläche
#define MONITOR_TIMEOUT      3000   // ohne Lebenszeichen der Seite: Live-Positionen aus
#define MONITOR_THRESHOLD    12     // ADC-Rauschen in Ruhe nicht senden

// ------------------- Web-Konfiguration (SysEx) -------------------
// Nachrichten: F0 7D 4B 4B <cmd> <daten...> F7
//   7D = Herstellerkennung "nicht-kommerziell", 4B 4B = "KK" (Kosmos Kontroller)
#define SYSEX_ID         0x7D
#define SYSEX_TAG1       0x4B
#define SYSEX_TAG2       0x4B
#define CMD_GET          0x01   // Browser -> Controller: alles anfragen
#define CMD_PRESET       0x02   // Controller -> Browser: [index, preset]
#define CMD_SET          0x03   // Browser -> Controller: [index, preset] übernehmen (nur RAM)
#define CMD_SAVE         0x04   // Browser -> Controller: alles in Flash speichern
#define CMD_RESET        0x05   // Browser -> Controller: Werkseinstellungen
#define CMD_STATE        0x06   // Controller -> Browser: [aktives Preset, Geste, Dauer/10ms]
#define CMD_SELECT       0x07   // Browser -> Controller: [index] Preset aktivieren
#define CMD_GLOBAL       0x08   // Browser -> Controller: [Geste, Dauer/10ms]
#define CMD_MONITOR      0x09   // Browser -> Controller: Lebenszeichen, Live-Positionen senden
#define CMD_LIVE         0x0A   // Controller -> Browser: 3 x Achse (2 x 6 Bit), Taster

#define CONFIG_MAGIC     0x4B4B0007
#define PB_RANGE         12     // Halbtöne: Pitchbend-Bereich im Synth (eine Oktave)
#define NAME_LEN         16     // Bytes (UTF-8), über SysEx als Nibbles übertragen
#define PRESET_COUNT     4
#define AXIS_COUNT       3
#define AXIS_TARGETS     4      // Ziele pro Achse
#define BUTTON_TARGETS   4      // Ziele des Tasters

// Was ein Achsen-Ziel sendet
enum AxisType {
  AT_PB = 0,        // Pitchbend, ganze Achse
  AT_PB_PLUS,       // Pitchbend, nur Richtung +
  AT_PB_MINUS,      // Pitchbend, nur Richtung -
  AT_CC,            // CC, ganze Achse, Mitte = 64
  AT_CC_PLUS,       // CC 0-127 über Richtung +
  AT_CC_MINUS,      // CC 0-127 über Richtung -
  AT_COUNT
};

enum Curve {
  CURVE_LINEAR = 0,
  CURVE_EXP,         // t²: feiner um die Mitte
  CURVE_EXP_STRONG,  // t³
  CURVE_LOG,         // 1-(1-t)²: schnell am Anfang
  CURVE_S,           // weich an beiden Enden
  CURVE_COUNT
};

// Was ein Taster-Ziel sendet
enum ButtonType {
  BT_CC_HOLD = 0,    // CC 127 beim Drücken, 0 beim Loslassen
  BT_CC_TOGGLE,      // CC abwechselnd 127 / 0
  BT_NOTE_HOLD,      // Note an beim Drücken, aus beim Loslassen
  BT_NOTE_TOGGLE,    // Note abwechselnd an / aus
  BT_PROGRAM,        // Program Change
  BT_COUNT
};

// Wie am Taster das Preset gewechselt wird
enum Gesture { GESTURE_OFF = 0, GESTURE_LONG, GESTURE_TRIPLE, GESTURE_COUNT };

struct AxisTarget {
  byte on;
  byte type;
  byte cc;               // 0-127
  byte channel;          // 1-16
  byte curve;
  byte invert;
  byte hiRes;            // 14 Bit (CC n + CC n+32), nur CC 0-31
  // Wertebereich: CC 0-127, Pitchbend in Halbtönen -PB_RANGE ... PB_RANGE.
  // Ganze Achse: Min bei voll -, Max bei voll + (Pitchbend: Mitte immer 0).
  // Nur eine Richtung: Min in Ruhe, Max bei voller Auslenkung.
  int8_t rangeMin;
  int8_t rangeMax;
  byte muted;            // stumm: eingestellt, sendet aber nichts (z. B. für MIDI-Learn)
  char name[NAME_LEN];
};

struct ButtonTarget {
  byte on;
  byte type;
  byte number;           // CC / Note / Programm
  byte channel;          // 1-16
  byte muted;
  char name[NAME_LEN];
};

struct Preset {
  char name[NAME_LEN];
  AxisTarget axis[AXIS_COUNT][AXIS_TARGETS];
  ButtonTarget button[BUTTON_TARGETS];
};

struct Config {
  uint32_t magic;
  byte activePreset;     // Start-Preset
  byte gesture;
  byte longPressTime;    // x 10 ms
  Preset preset[PRESET_COUNT];
};

FlashStorage(configStore, Config);
Config config;

void setDefaultRange(AxisTarget& t) {
  switch (t.type) {
    case AT_PB:       t.rangeMin = -PB_RANGE; t.rangeMax = PB_RANGE; break;
    case AT_PB_PLUS:  t.rangeMin = 0;         t.rangeMax = PB_RANGE; break;
    case AT_PB_MINUS: t.rangeMin = 0;         t.rangeMax = -PB_RANGE; break;
    default:          t.rangeMin = 0;         t.rangeMax = 127; break;
  }
}

void setAxisTarget(AxisTarget& t, byte type, byte cc, byte channel) {
  memset(&t, 0, sizeof(t));
  t.on = 1;
  t.type = type;
  t.cc = cc;
  t.channel = channel;
  setDefaultRange(t);
}

// Werkseinstellungen = Verhalten des alten Sketches
void loadDefaultPreset(Preset& p, int index) {
  memset(&p, 0, sizeof(p));
  snprintf(p.name, NAME_LEN, "Preset %d", index + 1);
  setAxisTarget(p.axis[0][0], AT_PB, 0, 2);       // Y-Achse: Pitchbend
  setAxisTarget(p.axis[1][0], AT_CC_PLUS, 1, 2);  // X-Achse: CC 1 nach +
  ButtonTarget& b = p.button[0];
  b.on = 1;
  b.type = BT_CC_HOLD;
  b.number = 64;
  b.channel = 2;
}

void loadDefaults() {
  config.magic = CONFIG_MAGIC;
  config.activePreset = 0;
  config.gesture = GESTURE_LONG;
  config.longPressTime = 60;
  for (int i = 0; i < PRESET_COUNT; i++) loadDefaultPreset(config.preset[i], i);
}

Preset& active() { return config.preset[config.activePreset]; }

// ------------------- Button-Variablen -------------------
bool lastButtonReading   = HIGH;
bool debouncedButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long pressTime = 0;
unsigned long lastTapTime = 0;
int tapCount = 0;
bool buttonHeld = false;
bool longPressFired = false;
bool suppressRelease = false;
bool toggleState[BUTTON_TARGETS] = {false};

// ------------------- LED-Blinken (Preset-Nummer) -------------------
int blinkRemaining = 0;
unsigned long blinkNext = 0;
bool blinkLit = false;

// ------------------- Achsen -------------------
const int axisPins[AXIS_COUNT] = {AXIS_Y_PIN, AXIS_X_PIN, AXIS_ROT_PIN};
int axisBuffer[AXIS_COUNT][SMOOTHING_WINDOW] = {{0}};
int axisSmooth[AXIS_COUNT] = {ADC_MID, ADC_MID, ADC_MID};
int bufferIndex = 0;

// Letzter gesendeter Wert pro Ziel (in dessen Auflösung)
#define NO_VALUE  -99999
int lastValue[AXIS_COUNT][AXIS_TARGETS];

// Live-Positionen für die Weboberfläche
unsigned long monitorUntil = 0;
unsigned long lastMonitorSend = 0;
int lastMonitor[AXIS_COUNT] = {NO_VALUE, NO_VALUE, NO_VALUE};
bool lastMonitorButton = false;

// Nach Konfigurationsänderung alle Werte neu senden
void resetLastValues() {
  for (int a = 0; a < AXIS_COUNT; a++)
    for (int t = 0; t < AXIS_TARGETS; t++) lastValue[a][t] = NO_VALUE;
}

// =======================================================
//                 MIDI SENDEN (mit Flusskontrolle)
// =======================================================

// Ein Byte über USB schreiben, bei vollem Sendepuffer warten (max. 50 ms).
// Wichtig: der TinyUSB-Puffer fasst nur ~96 SysEx-Bytes. Ohne Warten gehen
// Bytes (auch das F7) verloren und danach hängt der Ausgang im SysEx-Modus.
bool usbWriteWait(byte b) {
  unsigned long start = millis();
  while (usb_midi.write(b) == 0) {
    if (millis() - start > 50) return false;
    yield();  // lässt TinyUSB den Puffer an den Rechner schicken
  }
  return true;
}

void sendBytes(const byte* data, int n) {
  if (!TinyUSBDevice.mounted()) return;
  for (int i = 0; i < n; i++) {
    if (!usbWriteWait(data[i])) return;
  }
}

void sendCC(byte channel, byte cc, byte value) {
  byte m[3] = {(byte)(0xB0 | ((channel - 1) & 0x0F)), (byte)(cc & 0x7F), (byte)(value & 0x7F)};
  sendBytes(m, 3);
}

void sendPitchBend(byte channel, int value) {   // -8192 ... 8191
  unsigned v = value + 8192;
  byte m[3] = {(byte)(0xE0 | ((channel - 1) & 0x0F)), (byte)(v & 0x7F), (byte)((v >> 7) & 0x7F)};
  sendBytes(m, 3);
}

void sendNote(byte channel, byte note, bool on) {
  byte m[3] = {(byte)((on ? 0x90 : 0x80) | ((channel - 1) & 0x0F)), (byte)(note & 0x7F), (byte)(on ? 127 : 0)};
  sendBytes(m, 3);
}

void sendProgram(byte channel, byte program) {
  byte m[2] = {(byte)(0xC0 | ((channel - 1) & 0x0F)), (byte)(program & 0x7F)};
  sendBytes(m, 2);
}

// =======================================================
//                 SYSEX / WEB-KONFIGURATION
// =======================================================
// Preset-Daten (CMD_PRESET und CMD_SET):
//   Name (NAME_LEN x 2 Nibbles)
//   AXIS_COUNT x AXIS_TARGETS x [an, Art, CC, Kanal 0-15, Kurve, Umkehren, 14 Bit, Min, Max, stumm, Name]
//     (Min/Max bei Pitchbend um PB_RANGE verschoben, damit sie 7 Bit bleiben)
//   BUTTON_TARGETS x [an, Art, Nummer, Kanal 0-15, stumm, Name]
#define AXIS_TARGET_BYTES    (10 + NAME_LEN * 2)
#define BUTTON_TARGET_BYTES  (5 + NAME_LEN * 2)
#define PRESET_BYTES  (NAME_LEN * 2 + AXIS_COUNT * AXIS_TARGETS * AXIS_TARGET_BYTES + BUTTON_TARGETS * BUTTON_TARGET_BYTES)

void putName(byte* m, int& n, const char* name) {
  for (int c = 0; c < NAME_LEN; c++) {
    byte b = (byte)name[c];
    m[n++] = b >> 4;
    m[n++] = b & 0x0F;
  }
}

void getName(const byte* p, int& n, char* name) {
  for (int c = 0; c < NAME_LEN; c++) {
    name[c] = (char)(((p[n] & 0x0F) << 4) | (p[n + 1] & 0x0F));
    n += 2;
  }
}

byte clampByte(byte v, byte count) { return v < count ? v : 0; }

void sendHeader(byte cmd) {
  byte h[5] = {0xF0, SYSEX_ID, SYSEX_TAG1, SYSEX_TAG2, cmd};
  sendBytes(h, 5);
}

void sendEnd() {
  if (TinyUSBDevice.mounted()) usbWriteWait(0xF7);  // SysEx in jedem Fall abschließen
}

void sendPreset(int index) {
  const Preset& p = config.preset[index];
  byte msg[1 + PRESET_BYTES];
  int n = 0;
  msg[n++] = index;
  putName(msg, n, p.name);
  for (int a = 0; a < AXIS_COUNT; a++) {
    for (int i = 0; i < AXIS_TARGETS; i++) {
      const AxisTarget& t = p.axis[a][i];
      msg[n++] = t.on ? 1 : 0;
      msg[n++] = t.type;
      msg[n++] = t.cc & 0x7F;
      msg[n++] = (t.channel - 1) & 0x0F;
      msg[n++] = t.curve;
      msg[n++] = t.invert ? 1 : 0;
      msg[n++] = t.hiRes ? 1 : 0;
      int offset = t.type <= AT_PB_MINUS ? PB_RANGE : 0;
      msg[n++] = (t.rangeMin + offset) & 0x7F;
      msg[n++] = (t.rangeMax + offset) & 0x7F;
      msg[n++] = t.muted ? 1 : 0;
      putName(msg, n, t.name);
    }
  }
  for (int i = 0; i < BUTTON_TARGETS; i++) {
    const ButtonTarget& t = p.button[i];
    msg[n++] = t.on ? 1 : 0;
    msg[n++] = t.type;
    msg[n++] = t.number & 0x7F;
    msg[n++] = (t.channel - 1) & 0x0F;
    msg[n++] = t.muted ? 1 : 0;
    putName(msg, n, t.name);
  }
  sendHeader(CMD_PRESET);
  sendBytes(msg, n);
  sendEnd();
}

int8_t readRange(byte b, byte type) {
  if (type <= AT_PB_MINUS) return constrain((int)b - PB_RANGE, -PB_RANGE, PB_RANGE);
  return b & 0x7F;
}

void readPreset(Preset& p, const byte* d) {
  int n = 0;
  getName(d, n, p.name);
  for (int a = 0; a < AXIS_COUNT; a++) {
    for (int i = 0; i < AXIS_TARGETS; i++) {
      AxisTarget& t = p.axis[a][i];
      t.on      = d[n++] ? 1 : 0;
      t.type    = clampByte(d[n++], AT_COUNT);
      t.cc      = d[n++] & 0x7F;
      t.channel = (d[n++] & 0x0F) + 1;
      t.curve   = clampByte(d[n++], CURVE_COUNT);
      t.invert  = d[n++] ? 1 : 0;
      t.hiRes   = d[n++] ? 1 : 0;
      t.rangeMin = readRange(d[n++], t.type);
      t.rangeMax = readRange(d[n++], t.type);
      t.muted   = d[n++] ? 1 : 0;
      getName(d, n, t.name);
    }
  }
  for (int i = 0; i < BUTTON_TARGETS; i++) {
    ButtonTarget& t = p.button[i];
    t.on      = d[n++] ? 1 : 0;
    t.type    = clampByte(d[n++], BT_COUNT);
    t.number  = d[n++] & 0x7F;
    t.channel = (d[n++] & 0x0F) + 1;
    t.muted   = d[n++] ? 1 : 0;
    getName(d, n, t.name);
  }
}

void sendState() {
  byte m[3] = {config.activePreset, config.gesture, (byte)(config.longPressTime & 0x7F)};
  sendHeader(CMD_STATE);
  sendBytes(m, 3);
  sendEnd();
}

void sendAll() {
  for (int i = 0; i < PRESET_COUNT; i++) sendPreset(i);
  sendState();
}

// =======================================================
//                      TASTER
// =======================================================

bool isHoldType(byte type) { return type == BT_CC_HOLD || type == BT_NOTE_HOLD; }
bool isToggleType(byte type) { return type == BT_CC_TOGGLE || type == BT_NOTE_TOGGLE; }

void sendButtonTarget(int i, bool on) {
  const ButtonTarget& t = active().button[i];
  if (t.muted) return;
  switch (t.type) {
    case BT_CC_HOLD:
    case BT_CC_TOGGLE:   sendCC(t.channel, t.number, on ? 127 : 0); break;
    case BT_NOTE_HOLD:
    case BT_NOTE_TOGGLE: sendNote(t.channel, t.number, on); break;
    case BT_PROGRAM:     if (on) sendProgram(t.channel, t.number); break;
  }
}

// Eingeschaltete Umschalter ausschalten (vor Preset- oder Konfigurationswechsel),
// damit keine Noten hängen bleiben
void releaseToggles() {
  for (int i = 0; i < BUTTON_TARGETS; i++) {
    if (toggleState[i] && active().button[i].on && isToggleType(active().button[i].type)) {
      sendButtonTarget(i, false);
    }
    toggleState[i] = false;
  }
}

void startBlink(int count) {
  blinkRemaining = count * 2;
  blinkNext = millis();
  blinkLit = false;
}

void activatePreset(int index) {
  releaseToggles();
  config.activePreset = index % PRESET_COUNT;
  resetLastValues();
  startBlink(config.activePreset + 1);
  sendState();  // Weboberfläche mitziehen
}

// Umschalten / Programm: bei "langer Druck" erst beim Loslassen auslösen,
// damit ein Presetwechsel nicht zusätzlich die Aktion auslöst
bool tapsOnRelease() { return config.gesture == GESTURE_LONG; }

void buttonTap(int i) {
  if (isToggleType(active().button[i].type)) {
    toggleState[i] = !toggleState[i];
    sendButtonTarget(i, toggleState[i]);
  } else {
    sendButtonTarget(i, true);
  }
}

void onButtonPress() {
  buttonHeld = true;
  longPressFired = false;
  suppressRelease = false;
  pressTime = millis();

  if (config.gesture == GESTURE_TRIPLE) {
    tapCount = (pressTime - lastTapTime < TAP_WINDOW) ? tapCount + 1 : 1;
    lastTapTime = pressTime;
    if (tapCount >= 3) {
      tapCount = 0;
      suppressRelease = true;  // dritter Druck löst keine Aktion aus
      activatePreset(config.activePreset + 1);
      return;
    }
  }

  for (int i = 0; i < BUTTON_TARGETS; i++) {
    const ButtonTarget& t = active().button[i];
    if (!t.on) continue;
    if (isHoldType(t.type)) sendButtonTarget(i, true);
    else if (!tapsOnRelease()) buttonTap(i);
  }
}

void onButtonRelease() {
  buttonHeld = false;
  if (suppressRelease || longPressFired) return;  // schon beim Presetwechsel erledigt
  for (int i = 0; i < BUTTON_TARGETS; i++) {
    const ButtonTarget& t = active().button[i];
    if (!t.on) continue;
    if (isHoldType(t.type)) sendButtonTarget(i, false);
    else if (tapsOnRelease()) buttonTap(i);
  }
}

void checkLongPress() {
  if (config.gesture != GESTURE_LONG || !buttonHeld || longPressFired) return;
  if (millis() - pressTime < (unsigned long)config.longPressTime * 10) return;

  longPressFired = true;
  for (int i = 0; i < BUTTON_TARGETS; i++) {  // gehaltene Aktionen beenden
    const ButtonTarget& t = active().button[i];
    if (t.on && isHoldType(t.type)) sendButtonTarget(i, false);
  }
  activatePreset(config.activePreset + 1);
}

void updateBlink() {
  if (blinkRemaining == 0 || millis() < blinkNext) return;
  blinkLit = !blinkLit;
  digitalWrite(LED_PIN, blinkLit ? LED_ON : !LED_ON);
  blinkRemaining--;
  blinkNext = millis() + (blinkLit ? 120 : 200);
}

// =======================================================
//                 SYSEX EMPFANGEN
// =======================================================

// array enthält F0 ... F7
void handleSysEx(byte* data, unsigned size) {
  if (size < 6) return;
  if (data[1] != SYSEX_ID || data[2] != SYSEX_TAG1 || data[3] != SYSEX_TAG2) return;

  switch (data[4]) {
    case CMD_GET:
      sendAll();
      break;

    case CMD_SET:
      if (size < 7 + PRESET_BYTES || data[5] >= PRESET_COUNT) return;
      if (data[5] == config.activePreset) releaseToggles();
      readPreset(config.preset[data[5]], data + 6);
      resetLastValues();
      sendState();
      break;

    case CMD_SELECT:
      if (size < 7 || data[5] >= PRESET_COUNT) return;
      activatePreset(data[5]);
      break;

    case CMD_GLOBAL:
      if (size < 8) return;
      config.gesture = clampByte(data[5], GESTURE_COUNT);
      config.longPressTime = max((int)data[6], 20);
      sendState();
      break;

    case CMD_SAVE:
      configStore.write(config);
      sendState();
      break;

    case CMD_RESET:
      releaseToggles();
      loadDefaults();
      configStore.write(config);
      resetLastValues();
      sendAll();
      break;

    case CMD_MONITOR:
      monitorUntil = millis() + MONITOR_TIMEOUT;
      break;
  }
}

// =======================================================
//                      SETUP
// =======================================================
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, !LED_ON);
  analogReadResolution(12);

  TinyUSBDevice.setManufacturerDescriptor("Konrad Simon");
  TinyUSBDevice.setProductDescriptor("Kosmos Kontroller");
  TinyUSBDevice.setSerialDescriptor("KODAMIDI");

  // Gespeicherte Konfiguration laden (nach dem Flashen ist der Speicher leer)
  config = configStore.read();
  if (config.magic != CONFIG_MAGIC || config.activePreset >= PRESET_COUNT) {
    loadDefaults();
  }
  resetLastValues();

  usb_midi.begin();
  MIDI.setHandleSystemExclusive(handleSysEx);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();  // Wichtig: eingehende Nachrichten nicht zurückspiegeln
}

// =======================================================
//                      ACHSEN
// =======================================================

// --- Glättung der analogen Werte ---
int smooth(int* buffer, int newValue) {
  buffer[bufferIndex] = newValue;
  int sum = 0;
  for (int i = 0; i < SMOOTHING_WINDOW; i++) {
    sum += buffer[i];
  }
  return sum / SMOOTHING_WINDOW;
}

// --- Auslenkung -1 ... 1 mit Deadzone um die Mitte ---
float axisPosition(int raw, int deadzone) {
  int delta = raw - ADC_MID;
  if (abs(delta) <= deadzone) return 0;
  float range = delta > 0 ? (ADC_MAX - ADC_MID - deadzone) : (ADC_MID - deadzone);
  float v = (abs(delta) - deadzone) / range;
  if (v > 1) v = 1;
  return delta > 0 ? v : -v;
}

// --- Kurve auf Betrag 0 ... 1 anwenden ---
float applyCurve(float t, byte curve) {
  switch (curve) {
    case CURVE_EXP:        return t * t;
    case CURVE_EXP_STRONG: return t * t * t;
    case CURVE_LOG:        return 1 - (1 - t) * (1 - t);
    case CURVE_S:          return t * t * (3 - 2 * t);
    default:               return t;
  }
}

float signedCurve(float v, byte curve) {
  return v < 0 ? -applyCurve(-v, curve) : applyCurve(v, curve);
}

// --- CC-Wert 0 ... 1 an ein Ziel senden ---
void sendTargetCC(int axis, int i, float u) {
  const AxisTarget& t = active().axis[axis][i];
  bool hiRes = t.hiRes && t.cc < 32;
  int maxValue = hiRes ? 16383 : 127;
  int value = (int)(u * maxValue + 0.5f);
  int& last = lastValue[axis][i];

  if (value == last) return;
  // 14 Bit: kleine Schwankungen ignorieren, Endpunkte immer senden
  if (hiRes && last != NO_VALUE && abs(value - last) < HIRES_THRESHOLD &&
      value != 0 && value != maxValue) return;
  last = value;

  if (hiRes) {
    sendCC(t.channel, t.cc, value >> 7);        // MSB
    sendCC(t.channel, t.cc + 32, value & 0x7F); // LSB
  } else {
    sendCC(t.channel, t.cc, value);
  }
}

// --- Pitchbend -1 ... 1 an ein Ziel senden ---
void sendTargetPB(int axis, int i, float v) {
  const AxisTarget& t = active().axis[axis][i];
  int pitchBend = v >= 0 ? (int)(v * 8191 + 0.5f) : (int)(v * 8192 - 0.5f);
  int& last = lastValue[axis][i];
  // Nur senden bei signifikanter Änderung, Mitte immer exakt
  if (abs(pitchBend - last) > PITCHBEND_THRESHOLD || (pitchBend == 0 && last != 0)) {
    sendPitchBend(t.channel, pitchBend);
    last = pitchBend;
  }
}

void processAxis(int axis, int smoothValue) {
  float posPB = axisPosition(smoothValue, DEADZONE_PB);
  float posCC = axisPosition(smoothValue, DEADZONE_CC);

  for (int i = 0; i < AXIS_TARGETS; i++) {
    const AxisTarget& t = active().axis[axis][i];
    if (!t.on || t.muted) continue;

    bool isPB = t.type <= AT_PB_MINUS;
    float v = isPB ? posPB : posCC;
    if (t.invert) v = -v;
    // Wertebereich: CC 0-127, Pitchbend in Halbtönen (bezogen auf PB_RANGE im Synth)
    float lo = t.rangeMin, hi = t.rangeMax;
    float plus  = v > 0 ? applyCurve(v, t.curve) : 0;    // Auslenkung nach + (0 ... 1)
    float minus = v < 0 ? applyCurve(-v, t.curve) : 0;   // Auslenkung nach - (0 ... 1)

    switch (t.type) {
      case AT_PB:       sendTargetPB(axis, i, (plus * hi - minus * lo) / PB_RANGE); break;  // Mitte = 0
      case AT_PB_PLUS:  sendTargetPB(axis, i, (lo + plus * (hi - lo)) / PB_RANGE); break;
      case AT_PB_MINUS: sendTargetPB(axis, i, (lo + minus * (hi - lo)) / PB_RANGE); break;
      case AT_CC:       sendTargetCC(axis, i, (lo + (0.5f + 0.5f * (plus - minus)) * (hi - lo)) / 127); break;
      case AT_CC_PLUS:  sendTargetCC(axis, i, (lo + plus * (hi - lo)) / 127); break;
      case AT_CC_MINUS: sendTargetCC(axis, i, (lo + minus * (hi - lo)) / 127); break;
    }
  }
}

// --- Live-Positionen an die Weboberfläche (nur solange sie offen ist) ---
void sendMonitor() {
  if ((long)(monitorUntil - millis()) <= 0) return;
  if (millis() - lastMonitorSend < MONITOR_INTERVAL) return;

  bool pressed = debouncedButtonState == LOW;
  bool changed = pressed != lastMonitorButton;
  for (int a = 0; a < AXIS_COUNT; a++) {
    if (abs(axisSmooth[a] - lastMonitor[a]) > MONITOR_THRESHOLD) changed = true;
  }
  if (!changed) return;

  byte m[AXIS_COUNT * 2 + 1];
  int n = 0;
  for (int a = 0; a < AXIS_COUNT; a++) {
    lastMonitor[a] = axisSmooth[a];
    m[n++] = (axisSmooth[a] >> 6) & 0x3F;
    m[n++] = axisSmooth[a] & 0x3F;
  }
  m[n++] = pressed ? 1 : 0;
  lastMonitorButton = pressed;
  lastMonitorSend = millis();

  sendHeader(CMD_LIVE);
  sendBytes(m, n);
  sendEnd();
}

// =======================================================
//                      LOOP
// =======================================================
void loop() {
  // --- MIDI Eingang verarbeiten (SysEx von der Weboberfläche) ---
  while (MIDI.read()) {}

  // --- Achsen einlesen, glätten, senden ---
  for (int i = 0; i < AXIS_COUNT; i++) {
    axisSmooth[i] = smooth(axisBuffer[i], analogRead(axisPins[i]));
    processAxis(i, axisSmooth[i]);
  }
  bufferIndex = (bufferIndex + 1) % SMOOTHING_WINDOW;

  // ------------------- BUTTON HANDLING -------------------
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != debouncedButtonState) {
      debouncedButtonState = reading;
      if (debouncedButtonState == LOW) onButtonPress();
      else onButtonRelease();
    }
  }
  lastButtonReading = reading;
  checkLongPress();

  updateBlink();
  sendMonitor();
  delay(6);
}
