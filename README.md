# beep-lab

`BEEP//1` — a single-file 1-bit **passive piezo** beeper studio for the ESP32 (open `index.html`).

## What it does

- Piano-roll step editor for a **passive piezo / passive buzzer** (a real speaker tone, not a fixed-pitch active beeper).
- Paint notes by clicking / dragging; move notes by dragging the body; resize length by dragging either end; **shift+drag** sets per-note volume; right-click erases.
- Stack several pitches on the same step to build chords. Because one pin can only sound one tone at a time, chords are played back as a fast **arpeggio** (both in the browser preview and in the exported firmware).
- **NOTE INSPECTOR** side panel: every note has its own colour and its own frequency (Hz). Click a row to select the note on the grid, type a frequency, or click the swatch to recolour. Colours are stored with the project and drawn on the grid.
- Project library (save / duplicate / rename / delete, export / import `.json`), plus WAV / MP3 / Arduino `.ino` / `.json` export.

## Buzzer type

This build is **passive-only**. The active-buzzer mode was removed: a passive piezo/buzzer is driven with `tone()` on the selected GPIO. Legacy `.ino` files that were generated in active mode still import (their beeps become note rows at C5), and old projects keep their pitches — they now simply play as passive notes.

## Hardware (exported `.ino`)

- `BUZZER_PIN` → passive buzzer / piezo; other leg → GND.
- Tempo, gate, pin, and per-note frequencies are baked into the generated sketch.
- The sketch self-contains `N_STEP` / `N_LEN` / `N_F` note tables and an arpeggio player.

## Development

- `_verify.js` runs the inline studio script against a small Node DOM shim and asserts the resize / frequency / colour / passive-export behaviour: `node _verify.js`.
