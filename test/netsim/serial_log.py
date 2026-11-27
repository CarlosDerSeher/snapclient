#!/usr/bin/env python3
"""Append a board's serial output to a file, each line prefixed with the PC's time (HH:MM:SS).

usage: serial_log.py PORT LOG
Opens the port with DTR and RTS off, so it doesn't reset the board (on most ESP32 boards RTS is
wired to EN). Needs pyserial. Stop it with Ctrl-C or kill.
"""
import sys
import time

import serial

s = serial.Serial()
s.port = sys.argv[1]
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 1
s.open()
with open(sys.argv[2], 'a') as f:
    while True:
        line = s.readline()
        if line:
            f.write(time.strftime('%H:%M:%S ') + line.decode(errors='replace').rstrip() + '\n')
            f.flush()
