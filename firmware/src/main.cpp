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
     PLAY [0|1] [step]        -> start playback, 1 = loop (default 1), optional start step
     STOP                     -> stop playback
     SEEK <step>              -> jump while playing
     LOOP <0|1>               -> update loop while playing
     VOL <0..100>             -> master volume (PWM drive)
     DUTY <1..100>            -> pulse width / timbre
     BUTTON <0|1>             -> enable/disable the local D14 button
     BEEP <freqHz> <ms> [duty] -> one-off test tone (duty 0 = song timbre)

   Note volume and timbre ride along on each note:
     N <step> <len> <freqHz> [vol 0..100] [duty 1..100]
     duty 0 means "follow the song DUTY"

   Unsolicited reports sent back to the browser:
     STATE 1 <loop>           -> playback started
     STATE 0                  -> playback stopped (STOP, button, or end)
     P <step>                 -> current step, once per step (drives playhead)

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
  uint8_t  vol;          /* per-note volume 0..100 */
  uint8_t  duty;         /* per-note timbre 1..100, 0 = follow the song DUTY */
};

static Note     notes[MAX_NOTES];
static uint16_t noteCount  = 0;
static uint16_t songSteps  = 32;
static uint16_t tempoBpm   = 150;
static uint8_t  gatePct    = 85;
static uint8_t  volumePct  = 75;    /* master volume 0..100 */
static uint8_t  dutyPct    = 50;    /* pulse width / timbre 1..100 */
static bool     buttonOn   = true;  /* D14 local button enabled */

/* ---- playback state (non-blocking) ---- */
static bool     playing      = false;
static bool     loopPlay     = true;
static uint32_t playStartUs  = 0;
static uint32_t stepUs       = 0;
static uint32_t totalUs      = 0;
static uint32_t lastArpUs    = 0;
static uint16_t arpIndex     = 0;
static uint16_t lastFreq     = 0;
static uint8_t  lastVol      = 0;
static uint8_t  lastDutyPct  = 0;       /* effective timbre last applied */
static uint8_t  lastNoteDuty = 0;       /* raw per-note duty, 0 = follow song */
static uint16_t lastSentStep = 0xFFFF;  /* last step reported to the browser */

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

/* --------------------------------------------------------------------------
   Passive buzzer drive.  The ESP32's tone() is always a 50 % square and has
   no volume control, so we drive LEDC channel 0 directly: frequency sets the
   pitch, duty sets loudness/timbre.  Effective duty is the product of the
   master volume, the DUTY/timbre setting, and the note's own volume.
   -------------------------------------------------------------------------- */
static uint16_t dutyFor(uint8_t noteVol, uint8_t noteDuty)
{
  if (!volumePct || !noteVol) return 0;
  uint8_t timbre = noteDuty ? noteDuty : dutyPct;   /* note overrides the song */
  /* A passive piezo gets much louder near 50 % duty, so a straight linear
     volume would feel dead near the top.  Squaring the master volume spreads
     the change evenly across the slider (0 -> silence, 100 -> 50 % drive). */
  uint32_t d = (uint32_t)511 * volumePct * volumePct * noteVol / 1000000UL;
  d = d * timbre / 50UL;                        /* 50 % is the reference square */
  if (d == 0) d = 1;                            /* keep tiny volumes audible */
  return (uint16_t)(d > 1023 ? 1023 : d);
}

static void buzOff()
{
  ledcWrite(0, 0);
  lastFreq    = 0;
  lastVol     = 0;
  lastDutyPct = 0;
}

static void buzTone(uint16_t freq, uint8_t noteVol, uint8_t noteDuty)
{
  if (!freq || !noteVol) {
    if (lastFreq || lastVol) buzOff();
    return;
  }
  uint8_t timbre = noteDuty ? noteDuty : dutyPct;
  bool freqChanged = (freq != lastFreq);
  if (freqChanged) {
    ledcWriteTone(0, freq);                     /* sets frequency + 50 % duty */
    lastFreq = freq;
  }
  if (freqChanged || noteVol != lastVol || timbre != lastDutyPct) {
    ledcWrite(0, dutyFor(noteVol, noteDuty));
    lastVol     = noteVol;
    lastDutyPct = timbre;
  }
  lastNoteDuty = noteDuty;      /* remember the raw override for VOL/DUTY updates */
}

/* re-apply duty after the master volume or song timbre changes */
static void refreshDuty()
{
  if (!lastFreq) return;
  ledcWrite(0, dutyFor(lastVol, lastNoteDuty));
  lastDutyPct = lastNoteDuty ? lastNoteDuty : dutyPct;
}

static void stopPlayback(bool quiet)
{
  bool wasPlaying = playing;
  playing    = false;
  manualFreq = 0;
  if (quiet) buzOff();
  if (wasPlaying) {
    lastSentStep = 0xFFFF;
    Serial.println("STATE 0");
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
  manualFreq  = 0;
  buzOff();
  playing     = true;
  lastSentStep = 0xFFFF;

  Serial.print("STATE 1 ");
  Serial.println(loop ? 1 : 0);

  Serial.print("OK PLAY notes=");
  Serial.print(noteCount);
  Serial.print(" steps=");
  Serial.print(songSteps);
  Serial.print(" bpm=");
  Serial.print(tempoBpm);
  Serial.print(" loop=");
  Serial.println(loop ? 1 : 0);
}

/* change tempo without stopping; keep the current musical position */
static void setTempo(uint16_t bpm)
{
  bpm = (uint16_t)constrain(bpm, 20, 400);
  uint32_t now = micros();
  if (playing && stepUs) {
    uint32_t pos = (now - playStartUs) / stepUs;
    if (pos >= songSteps) pos = (uint32_t)(songSteps - 1);
    tempoBpm = bpm;
    stepUs   = computeStepUs();
    totalUs  = stepUs * (uint32_t)songSteps;
    playStartUs = now - pos * stepUs;
    lastArpUs   = now;
    lastSentStep = 0xFFFF;
  } else {
    tempoBpm = bpm;
    stepUs   = computeStepUs();
    totalUs  = stepUs * (uint32_t)songSteps;
  }
}

/* jump to a step while playing (used for live edits / ruler clicks) */
static void seekPlayback(uint16_t step)
{
  if (!playing) return;
  if (step >= songSteps) step = (uint16_t)(songSteps - 1);
  playStartUs  = micros() - (uint32_t)step * stepUs;
  lastArpUs    = micros();
  arpIndex     = 0;
  lastSentStep = 0xFFFF;
  Serial.print("OK SEEK ");
  Serial.println(step);
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

  unsigned int a, b, c, d, e;
  if (sscanf(s, "BPM %u", &a) == 1) {
    setTempo((uint16_t)constrain(a, 20, 400));
    Serial.print("OK BPM "); Serial.println(tempoBpm);
    return;
  }
  if (sscanf(s, "STEPS %u", &a) == 1) {
    songSteps = (uint16_t)constrain(a, 1, 512);
    totalUs   = stepUs * (uint32_t)songSteps;
    Serial.print("OK STEPS "); Serial.println(songSteps);
    return;
  }
  if (sscanf(s, "GATE %u", &a) == 1) {
    gatePct = (uint8_t)constrain(a, 5, 100);
    Serial.print("OK GATE "); Serial.println(gatePct);
    return;
  }
  if (sscanf(s, "VOL %u", &a) == 1) {
    volumePct = (uint8_t)constrain(a, 0, 100);
    refreshDuty();
    Serial.print("OK VOL "); Serial.println(volumePct);
    return;
  }
  if (sscanf(s, "DUTY %u", &a) == 1) {
    dutyPct = (uint8_t)constrain(a, 1, 100);
    refreshDuty();
    Serial.print("OK DUTY "); Serial.println(dutyPct);
    return;
  }
  if (sscanf(s, "LOOP %u", &a) == 1) {
    loopPlay = (a != 0);
    Serial.print("OK LOOP "); Serial.println(loopPlay ? 1 : 0);
    return;
  }
  if (sscanf(s, "BUTTON %u", &a) == 1) {
    bool on = (a != 0);
    if (!on) stopPlayback(true);
    buttonOn = on;
    btnRaw = digitalRead(BUTTON_PIN);
    btnStable = btnRaw;
    btnChangeMs = millis();
    Serial.print("OK BUTTON "); Serial.println(buttonOn ? 1 : 0);
    return;
  }
  if (sscanf(s, "SEEK %u", &a) == 1) {
    if (!playing) { Serial.println("ERR NOT_PLAYING"); return; }
    seekPlayback((uint16_t)a);
    return;
  }
  {
    int got = sscanf(s, "N %u %u %u %u %u", &a, &b, &c, &d, &e);
    if (got >= 3) {
      if (noteCount < MAX_NOTES) {
        Note &nt = notes[noteCount++];
        nt.step = (uint16_t)constrain(a, 0, 511);
        nt.len  = (uint16_t)constrain(b, 1, 512);
        nt.freq = (uint16_t)constrain(c, 0, 20000);
        nt.vol  = (got >= 4) ? (uint8_t)constrain(d, 0, 100) : 100;
        nt.duty = (got >= 5) ? (uint8_t)constrain(e, 0, 100) : 0;
      }
      return;
    }
  }

  if (!strcmp(s, "PLAY")) {
    startPlayback(true);
    return;
  }
  {
    unsigned int st = 0;
    int got = sscanf(s, "PLAY %u %u", &a, &st);
    if (got >= 1) {
      startPlayback(a != 0);
      if (got >= 2 && playing) seekPlayback((uint16_t)st);
      return;
    }
  }
  {
    unsigned int bd = 0;
    int got = sscanf(s, "BEEP %u %u %u", &a, &b, &bd);
    if (got >= 2) {
      stopPlayback(true);
      uint16_t f = (uint16_t)constrain(a, 20, 20000);
      if (f) {
        buzTone(f, 100, (uint8_t)constrain(bd, 0, 100));
        manualFreq  = f;
        manualOffMs = millis() + constrain(b, 10, 5000);
      }
      Serial.println("OK BEEP");
      return;
    }
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
      buzOff();
      manualFreq = 0;
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

  /* report the current step so the browser can move its playhead */
  uint16_t stepNow = (uint16_t)(t / stepUs);
  if (stepNow >= songSteps) stepNow = (uint16_t)(songSteps - 1);
  if (stepNow != lastSentStep) {
    lastSentStep = stepNow;
    Serial.print("P ");
    Serial.println(stepNow);
  }

  /* collect every voice sounding at logical time t */
  struct Voice { uint16_t freq; uint8_t vol; uint8_t duty; };
  Voice    voices[MAX_VOICES];
  uint8_t  nf = 0;
  for (uint16_t i = 0; i < noteCount && nf < MAX_VOICES; i++) {
    uint32_t on  = (uint32_t)notes[i].step * stepUs;
    uint32_t off = on + ((uint32_t)notes[i].len * stepUs * gatePct) / 100UL;
    if (t >= on && t < off && notes[i].freq > 0 && notes[i].vol > 0) {
      voices[nf].freq = notes[i].freq;
      voices[nf].vol  = notes[i].vol;
      voices[nf].duty = notes[i].duty;
      nf++;
    }
  }

  if (nf == 0) {
    if (lastFreq) buzOff();
    return;
  }
  if (nf == 1) {
    buzTone(voices[0].freq, voices[0].vol, voices[0].duty);
    return;
  }

  /* chord: flip through the voices every ARP_US on the single pin */
  if ((uint32_t)(now - lastArpUs) >= ARP_US) {
    lastArpUs = now;
    arpIndex  = (uint16_t)((arpIndex + 1) % nf);
    buzTone(voices[arpIndex].freq, voices[arpIndex].vol, voices[arpIndex].duty);
  }
}

static void buttonTick()
{
  if (!buttonOn) return;

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
    buzTone(1000, 100, 0);
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

  /* direct LEDC drive so we can control volume as PWM duty */
  ledcSetup(0, 2000, 10);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 0);

  Serial.begin(115200);
  delay(80);
  Serial.println("READY BEEP1");
  Serial.flush();

  /* audible boot confirmation */
  buzTone(1200, 100, 0);
  delay(120);
  buzOff();

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
