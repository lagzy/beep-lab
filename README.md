# beep-lab

`BEEP//1` — a single-file 1-bit **passive piezo** beeper studio for the ESP32 (open `index.html`).

## Simple & Advanced

The app opens in **Simple** mode, made for young kids: big buttons, friendly labels (Speed, Song length), a visual colour palette, and a big −/+ length stepper. Technical controls (tempo BPM fine-tuning, gate, duty, pin, rotate/octave, project library, `.ino`/`.json`/import/code export) are hidden until you press **⚙ ADVANCED** in the header. The toggle is remembered in `localStorage`, and everything stays fully available in Advanced mode.

## Play on the real ESP32 (USB)

The `firmware/` folder contains a ready-to-flash **device player** for an ESP32 WROOM dev board. It talks to the studio over USB serial, so no WiFi credentials are needed.

**Wiring**

| Part | ESP32 pin |
| --- | --- |
| Passive piezo/buzzer `+` | **D4** (GPIO4) |
| Passive piezo/buzzer `−` | **GND** |
| Push button leg 1 | **D14** (GPIO14) |
| Push button leg 2 | **GND** |

The button uses the internal pull-up, so no resistor is needed. Press it to **play/stop** the last song; with no song loaded it makes a short test beep.

**Flash**

```bash
pio run -d firmware                              # build
pio run -d firmware -t upload --upload-port COM4 # flash (use your port)
```

If upload fails, hold the **BOOT** button on the board while it connects. A 1200 Hz boot chirp confirms the buzzer wiring.

**Use it**

1. Open `index.html` in **Chrome or Edge** (Web Serial is not available in Firefox/Safari).
2. Press **▶ PLAY ON DEVICE** in the header and pick the ESP32's COM port.
3. The studio sends the current song and the ESP32 plays it on D4, looping if **LOOP** is on.
4. The button turns into **■ STOP DEVICE**; press it again to stop.

If you serve the page from GitHub Pages (HTTPS) Web Serial still works. If you use `file://` and your browser hides Web Serial, run a tiny local server (`python -m http.server`) and open `http://localhost:8000`.

**Serial protocol (115200 baud, one command per line)**

```
PING                     -> PONG
RESET                    -> clear song
BPM <20..400>
STEPS <1..512>
GATE <5..100>
N <step> <len> <freqHz>  -> add note
PLAY <0|1>               -> start (1 = loop)
STOP
BEEP <freqHz> <ms>
```

## What it does

- Piano-roll step editor for a **passive piezo / passive buzzer** (a real speaker tone, not a fixed-pitch active beeper).
- Paint notes by clicking / dragging; move notes by dragging the body; resize length by dragging either end; **shift+drag** sets per-note volume; right-click erases.
- Stack several pitches on the same step to build chords. Because one pin can only sound one tone at a time, chords are played back as a fast **arpeggio** (both in the browser preview and in the exported firmware).
- **NOTE** side panel: select a note on the grid and edit its sound (frequency in Hz), start step, length, volume and colour. Every note keeps its own colour and frequency; colours are stored with the project and drawn on the grid.
- Project library (save / duplicate / rename / delete, export / import `.json`), plus WAV / MP3 / Arduino `.ino` / `.json` export.

## Buzzer type

This build is **passive-only**. The active-buzzer mode was removed: a passive piezo/buzzer is driven with `tone()` on the selected GPIO. Legacy `.ino` files that were generated in active mode still import (their beeps become note rows at C5), and old projects keep their pitches — they now simply play as passive notes.

## Hardware (exported `.ino`)

- `BUZZER_PIN` → passive buzzer / piezo; other leg → GND.
- Tempo, gate, pin, and per-note frequencies are baked into the generated sketch.
- The sketch self-contains `N_STEP` / `N_LEN` / `N_F` note tables and an arpeggio player.

## Development

- `_verify.js` runs the inline studio script against a small Node DOM shim and asserts the resize / frequency / colour / passive-export behaviour: `node _verify.js`.
