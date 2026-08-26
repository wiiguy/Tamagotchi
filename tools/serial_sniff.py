#!/usr/bin/env python3
"""Dump N seconds of serial output from the board to stdout."""
import sys
import time

import serial


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:  # noqa: BLE001
        print(f"[serial_sniff] could not open {port}: {e}")
        return
    # opening may toggle DTR and reset the board - that's fine, we want boot logs
    t0 = time.time()
    try:
        while time.time() - t0 < secs:
            data = s.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except Exception as e:  # noqa: BLE001
        print(f"[serial_sniff] error: {e}")
    finally:
        s.close()


if __name__ == "__main__":
    main()
