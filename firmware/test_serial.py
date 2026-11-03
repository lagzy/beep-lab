"""BEEP//1 device serial test.

Usage:
    python test_serial.py COM5

Needs pyserial (PlatformIO's Python already has it:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe test_serial.py COM5
)

It resets the ESP32, checks PING/PONG, loads a short C-major scale plus a
chord, plays it once, then starts/stops a loop.  Finally it beeps the same
tone at full and at 20 % volume so you can hear that VOL works, and at two
different per-note duties so you can hear the timbre override.
You should hear:
    boot chirp -> scale -> chord -> (loop starts) -> silence -> loud beep -> quiet beep

It also checks the unsolicited playhead reports:
    STATE 1 <loop>  when playback starts
    P <step>        once per step
    STATE 0         when playback stops
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
BAUD = 115200

s = serial.Serial()
s.port = PORT
s.baudrate = BAUD
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
time.sleep(0.4)

# clean reset into the application (EN low, IO0 high)
s.reset_input_buffer()
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False
time.sleep(1.3)


def drain():
    n = s.in_waiting
    return s.read(n).decode(errors='replace') if n else ''


def read_for(seconds):
    end = time.time() + seconds
    out = ''
    while time.time() < end:
        time.sleep(0.05)
        out += drain()
    return out.strip()


def lines_starting(text, prefix):
    return [ln for ln in text.splitlines() if ln.startswith(prefix)]


def steps_in(text):
    out = []
    for ln in text.splitlines():
        if ln.startswith('P '):
            try:
                out.append(int(ln[2:]))
            except ValueError:
                pass
    return out


print('BOOT  ->', repr(drain().strip()))


def cmd(line, wait=0.3):
    s.write((line + '\n').encode())
    s.flush()
    time.sleep(wait)
    return drain().strip()


print('PING  ->', repr(cmd('PING')))
print('RESET ->', repr(cmd('RESET')))
print('BPM   ->', repr(cmd('BPM 160')))
print('STEPS ->', repr(cmd('STEPS 12')))
print('GATE  ->', repr(cmd('GATE 90')))
print('DUTY  ->', repr(cmd('DUTY 50')))
print('VOL   ->', repr(cmd('VOL 75')))
print('LOOP  ->', repr(cmd('LOOP 1')))
print('BTN   ->', repr(cmd('BUTTON 0')), 'then', repr(cmd('BUTTON 1')))

# C major scale + C major chord on step 8 (chord is arpeggiated)
# The two E notes carry a lower per-note volume so it can be heard, and one
# note uses a 20 % per-note duty so you can hear the timbre override.
song = [(0, 523, 100, 0), (1, 587, 100, 0), (2, 659, 100, 20), (3, 698, 100, 0),
        (4, 784, 100, 0), (5, 880, 100, 0), (6, 988, 100, 0), (7, 1047, 100, 0),
        (8, 523, 100, 0), (8, 659, 45, 0), (8, 784, 100, 0)]
for step, freq, vol, duty in song:
    cmd('N %d 1 %d %d %d' % (step, freq, vol, duty), 0.03)
print('notes queued:', len(song))

play_reply = cmd('PLAY 0', 0.2)          # STATE 1 + OK PLAY + first P lines
during = read_for(2.1)                   # song ends at ~1.125 s -> STATE 0
allplay = play_reply + '\n' + during
print('PLAY  ->', ' | '.join(lines_starting(play_reply, 'OK PLAY')))
print('STATE ->', ' | '.join(lines_starting(allplay, 'STATE')))
print('STEPS ->', ' '.join(str(n) for n in steps_in(allplay)))

loop_reply = cmd('PLAY 1', 0.2)
loop_during = read_for(1.2)
stop_reply = cmd('STOP', 0.4)
print('LOOP  ->', ' | '.join(lines_starting(loop_reply, 'OK PLAY')))
print('LOOP STATE ->', ' | '.join(lines_starting(loop_reply, 'STATE')))
print('LOOP STEPS ->', ' '.join(str(n) for n in steps_in(loop_reply + loop_during)))
print('STOP  ->', ' | '.join(lines_starting(stop_reply, 'OK STOP') +
                               lines_starting(stop_reply, 'STATE')))

print('BEEP  ->', repr(cmd('BEEP 1000 120', 0.3)))

# Audible volume check: the same tone at full and low master volume.
print('VOL 100 BEEP ->', repr(cmd('VOL 100', 0.05)), repr(cmd('BEEP 880 260', 0.4)))
print('VOL 20  BEEP ->', repr(cmd('VOL 20', 0.05)), repr(cmd('BEEP 880 260', 0.4)))
print('VOL 75  ->', repr(cmd('VOL 75', 0.1)))

# Per-note timbre: same tone, thin vs fat duty.
print('DUTY 20 BEEP ->', repr(cmd('BEEP 1000 220 20', 0.35)))
print('DUTY 80 BEEP ->', repr(cmd('BEEP 1000 220 80', 0.35)))

# Start in the middle of the song and confirm SEEK + P reports.
seek_reply = cmd('PLAY 1 8', 0.2)
seek_during = read_for(0.6)
seek_all = seek_reply + '\n' + seek_during
print('SEEK  ->', ' | '.join(lines_starting(seek_all, 'OK SEEK')),
      '| STATE', ' | '.join(lines_starting(seek_all, 'STATE')),
      '| P', ' '.join(str(n) for n in steps_in(seek_all)))
print('STOP  ->', repr(cmd('STOP', 0.3)))

s.close()
print('TEST DONE')
