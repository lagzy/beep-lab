"""BEEP//1 device serial test.

Usage:
    python test_serial.py COM5

Needs pyserial (PlatformIO's Python already has it:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe test_serial.py COM5
)

It resets the ESP32, checks PING/PONG, loads a short C-major scale plus a
chord, plays it once, then starts/stops a loop.  You should hear:
    boot chirp -> scale -> chord -> (loop starts) -> silence
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


print('BOOT:', repr(drain().strip()))


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

# C major scale + C major chord on step 8 (chord is arpeggiated)
song = [(0, 523), (1, 587), (2, 659), (3, 698), (4, 784), (5, 880),
        (6, 988), (7, 1047), (8, 523), (8, 659), (8, 784)]
for step, freq in song:
    cmd('N %d 1 %d' % (step, freq), 0.03)
print('notes queued:', len(song))

print('PLAY  ->', repr(cmd('PLAY 0', 0.4)))
time.sleep(2.2)
print('DURING ->', repr(drain().strip()))

print('LOOP  ->', repr(cmd('PLAY 1', 0.4)))
time.sleep(1.0)
print('STOP  ->', repr(cmd('STOP')))
print('BEEP  ->', repr(cmd('BEEP 1000 120', 0.3)))

s.close()
print('TEST DONE')
