/* ============================================================================
   BEEP//1  —  ESP32 device player for the Beep Lab browser studio
   ----------------------------------------------------------------------------
   Hardware (ESP32 WROOM dev board):
     passive piezo / buzzer   +  ->  D4  (GPIO4)
     passive piezo / buzzer   -  ->  GND
     push button              leg1 -> D14 (GPIO14)
     push button              leg2 -> GND        (internal pull-up, active LOW)

   The browser sends the song over USB serial at 115200 baud, one command per
   line.  After PLAY the ESP32 plays it on its own (and can loop).  The D14
   button toggles play/stop without the browser; if no song was loaded it
   makes a short test beep.

   Serial protocol (newline separated):
     PING                     -> PONG
     RESET                    -> clears the loaded song
     BPM <20..400>            -> tempo
     STEPS <1..512>           -> song length in 16th steps
     GATE <5..100>            -> note gate / length in %
     N <step> <len> <freqHz>  -> add one note (up to MAX_NOTES)
     PLAY [0|1]               -> start playback, 1 = loop (default 1)
     STOP                     -> stop playback
     BEEP <freqHz> <ms>       -> one-off test tone

   This file is intentionally dependency-free (Arduino core only).
   ========================================================================== */

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define BUZZER_PIN   4
#define BUTTON_PIN   14

#define MAX_NOTES    256
#define MAX_VOICES   8
#define ARP_US       6000UL          /* 6 ms per chord voice (matches studio) */
#define DEBOUNCE_MS  30UL
#define CMD_LINE_MAX     80

struct Note {
  uint16_t step;
  uint16_t len;
  uint16_t freq;
};

static Note     notes[MAX_NOTES];
static uint16_t noteCount  = 0;
static uint16_t songSteps  = 32;
static uint16_t tempoBpm   = 150;
static uint8_t  gatePct    = 85;

/* ---- playback state (non-blocking) ---- */
static bool     playing      = false;
static bool     loopPlay     = true;
static uint32_t playStartUs  = 0;
static uint32_t stepUs       = 0;
static uint32_t totalUs      = 0;
static uint32_t lastArpUs    = 0;
static uint16_t arpIndex     = 0;
static uint16_t lastFreq     = 0;

/* ---- one-off BEEP state ---- */
static uint16_t manualFreq   = 0;
static uint32_t manualOffMs  = 0;

/* ---- button debounce ---- */
static bool     btnRaw       = HIGH;
static bool     btnStable    = HIGH;
static uint32_t btnChangeMs  = 0;

/* ---- serial line buffer ---- */
static char    lineBuf[CMD_LINE_MAX + 1];
static uint8_t lineLen       = 0;
static bool    lineDropped   = false;

/* ========================================================================== */

static uint32_t computeStepUs()
{
  uint32_t bpm = tempoBpm < 20 ? 20 : (tempoBpm > 400 ? 400 : tempoBpm);
  return 60000000UL / (bpm * 4UL);              /* 16th note in microseconds */
}

static void stopPlayback(bool quiet)
{
  playing    = false;
  manualFreq = 0;
  if (quiet) {
    noTone(BUZZER_PIN);
    lastFreq = 0;
  }
}

static void startPlayback(bool loop)
{
  if (noteCount == 0) {
    Serial.println("ERR NO_NOTES");
    return;
  }
  loopPlay    = loop;
  stepUs      = computeStepUs();
  totalUs     = stepUs * (uint32_t)songSteps;
  playStartUs = micros();
  lastArpUs   = micros();
  arpIndex    = 0;
  lastFreq    = 0;
  manualFreq  = 0;
  playing     = true;
}

static void handleLine(char *s)
{
  /* trim trailing CR */
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
  if (n == 0) return;

  if (!strcmp(s, "PING")) {
    Serial.println("PONG");
    return;
  }

  if (!strcmp(s, "RESET")) {
    stopPlayback(true);
    noteCount = 0;
    Serial.println("OK RESET");
    return;
  }

  if (!strcmp(s, "STOP")) {
    stopPlayback(true);
    Serial.println("OK STOP");
    return;
  }

  unsigned int a, b, c;
  if (sscanf(s, "BPM %u", &a) == 1) {
    tempoBpm = (uint16_t)constrain(a, 20, 400);
    Serial.print("OK BPM "); Serial.println(tempoBpm);
    return;
  }
  if (sscanf(s, "STEPS %u", &a) == 1) {
    songSteps = (uint16_t)constrain(a, 1, 512);
    Serial.print("OK STEPS "); Serial.println(songSteps);
    return;
  }
  if (sscanf(s, "GATE %u", &a) == 1) {
    gatePct = (uint8_t)constrain(a, 5, 100);
    Serial.print("OK GATE "); Serial.println(gatePct);
    return;
  }
  if (sscanf(s, "N %u %u %u", &a, &b, &c) == 3) {
    if (noteCount < MAX_NOTES) {
      Note &nt = notes[noteCount++];
      nt.step = (uint16_t)constrain(a, 0, 511);
      nt.len  = (uint16_t)constrain(b, 1, 512);
      nt.freq = (uint16_t)constrain(c, 0, 20000);
    }
    return;
  }

  if (!strcmp(s, "PLAY")) {
    startPlayback(true);
    return;
  }
  if (sscanf(s, "PLAY %u", &a) == 1) {
    startPlayback(a != 0);
    return;
  }
  if (sscanf(s, "BEEP %u %u", &a, &b) == 2) {
    stopPlayback(true);
    uint16_t f = (uint16_t)constrain(a, 20, 20000);
    if (f) {
      tone(BUZZER_PIN, f);
      manualFreq  = f;
      manualOffMs = millis() + constrain(b, 10, 5000);
    }
    Serial.println("OK BEEP");
    return;
  }

  Serial.print("ERR UNKNOWN ");
  Serial.println(s);
}

static void pollSerial()
{
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen && !lineDropped) handleLine(lineBuf);
      lineLen     = 0;
      lineDropped = false;
    } else if (ch != '\r') {
      if (lineLen < CMD_LINE_MAX) lineBuf[lineLen++] = ch;
      else { lineLen = 0; lineDropped = true; }   /* overflow: drop the line */
    }
  }
}

static void playerTick()
{
  uint32_t now = micros();

  if (!playing) {
    if (manualFreq && (int32_t)(millis() - manualOffMs) >= 0) {
      noTone(BUZZER_PIN);
      manualFreq = 0;
      lastFreq   = 0;
    }
    return;
  }

  uint32_t t = now - playStartUs;
  if (t >= totalUs) {
    if (loopPlay) {
      playStartUs += totalUs;
      t = now - playStartUs;
    } else {
      stopPlayback(true);
      return;
    }
  }

  /* collect every voice sounding at logical time t */
  uint16_t voices[MAX_VOICES];
  uint8_t  nf = 0;
  for (uint16_t i = 0; i < noteCount && nf < MAX_VOICES; i++) {
    uint32_t on  = (uint32_t)notes[i].step * stepUs;
    uint32_t off = on + ((uint32_t)notes[i].len * stepUs * gatePct) / 100UL;
    if (t >= on && t < off && notes[i].freq > 0) {
      voices[nf++] = notes[i].freq;
    }
  }

  if (nf == 0) {
    if (lastFreq) { noTone(BUZZER_PIN); lastFreq = 0; }
    return;
  }
  if (nf == 1) {
    if (voices[0] != lastFreq) {
      tone(BUZZER_PIN, voices[0]);
      lastFreq = voices[0];
    }
    return;
  }

  /* chord: flip through the voices every ARP_US on the single pin */
  if ((uint32_t)(now - lastArpUs) >= ARP_US) {
    lastArpUs = now;
    arpIndex  = (uint16_t)((arpIndex + 1) % nf);
    uint16_t f = voices[arpIndex];
    if (f != lastFreq) {
      tone(BUZZER_PIN, f);
      lastFreq = f;
    }
  }
}

static void buttonTick()
{
  bool raw = digitalRead(BUTTON_PIN);
  uint32_t now = millis();

  if (raw != btnRaw) {
    btnRaw      = raw;
    btnChangeMs = now;
  }
  if ((uint32_t)(now - btnChangeMs) < DEBOUNCE_MS || raw == btnStable) return;

  btnStable = raw;
  if (btnStable != LOW) return;              /* only act on press */

  if (playing) {
    stopPlayback(true);
    Serial.println("BTN STOP");
  } else if (noteCount > 0) {
    startPlayback(loopPlay);
    Serial.println("BTN PLAY");
  } else {
    /* nothing loaded yet: short test beep so the wiring can be checked */
    tone(BUZZER_PIN, 1000);
    manualFreq  = 1000;
    manualOffMs = millis() + 180;
    Serial.println("BTN BEEP");
  }
}

/* ========================================================================== */

void setup()
{
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(80);
  Serial.println("READY BEEP1");
  Serial.flush();

  /* audible boot confirmation */
  tone(BUZZER_PIN, 1200);
  delay(120);
  noTone(BUZZER_PIN);

  btnRaw    = digitalRead(BUTTON_PIN);
  btnStable = btnRaw;
  btnChangeMs = millis();
}

void loop()
{
  pollSerial();
  buttonTick();
  playerTick();
  delay(1);
}
