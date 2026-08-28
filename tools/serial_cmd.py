#!/usr/bin/env python3
"""Send a command to the board and dump output for N seconds.

Usage:
  serial_cmd.py [PORT] [CMD] [SECS]
  serial_cmd.py k        # kill, 5s
  serial_cmd.py s        # status, 5s
  serial_cmd.py "?"      # help, 5s
  serial_cmd.py k 8      # kill, 8s
"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
cmd = sys.argv[2] if len(sys.argv) > 2 else "?"
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

s = serial.Serial(port, 115200, timeout=0.2)
# ESP32-C3 needs DTR managed carefully to avoid reset during command
s.dtr = False
s.rts = False
time.sleep(0.3)
s.dtr = True
s.rts = True
time.sleep(1.5)
s.reset_input_buffer()

s.write(cmd.encode())
s.flush()

t0 = time.time()
try:
    while time.time() - t0 < secs:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
finally:
    s.close()
