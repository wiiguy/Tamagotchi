# ORB-TAMA — a pocket tamagotchi for the ESP32-C3 round display

A self-contained virtual pet for the **JCZN/Guition ESP32-2424S012** board
(ESP32-C3 + 1.28″ round 240×240 GC9A01 IPS + CST816D touchscreen).
No buttons, no wires, no cloud — everything happens on the round glass.

![hardware](https://homeding.github.io/boards/esp32c3/jczn-esp32-2424s012.htm)

## What it does

The Orb hatches from an egg, then lives on your desk:

| Interaction | What happens |
|---|---|
| **Drag** your finger over it | Petting — hearts float up, +fun |
| **Tap** it | It giggles, +a little fun |
| **Burger icon** (bottom-left) | Feed it — chomping animation, +fullness |
| **Ball icon** (bottom-middle) | Bubble-pop mini-game (5 rounds) — +fun, −energy |
| **Drop icon** (bottom-right) | Clean up all poops |
| **Tap a poop** directly | Clean just that one |
| **Hold your finger ~1s** | Stats & age overlay |

It gets hungry/bored/tired on its own, poops after meals (and gets sadder
while poops are around), **falls asleep and dims the screen** when exhausted
(tap to wake), **evolves** `BABY → TEEN → ADULT` with age, and can get
visibly miserable (droopy lids, frown, tears) if you neglect it.

The three colored arcs around the bezel are its vitals:
left = **fullness** (green→red), top-right = **fun** (pink), bottom = **energy** (blue).

## State & persistence

Everything is saved to the board's flash (NVS) every ~20s: stats, age,
hatched flag, poops. It survives power-off **and re-flashing** — your pet
is still yours after a firmware update.

## Time scale

`TIME_SCALE` in `orb.ino`:

- `1` — real time (a full day takes a full day; evolves over hours)
- `60` (default) — one pet-day per 24 real minutes. Hunger hits zero in
  ~3 min, it naps after ~8 min, baby→teen after 1 min, teen→adult after 12 min.

## Board wiring (already hard-coded, no jumpers needed)

| Signal | GPIO |
|---|---|
| SCLK / MOSI / DC / CS | 6 / 7 / 2 / 10 |
| Backlight | 3 |
| Touch SDA / SCL / RST | 4 / 5 / 1 (CST816D @ 0x15) |

If the picture is upside-down, flip `ROTATION` between `2` and `0`.
If touch axes are mirrored, flip `TOUCH_INVERT_X/Y` (current values are
verified correct for this board).

## Build & flash

Requires `arduino-cli` with the `esp32:esp32` core (3.x).

```bash
# one-shot: compile + flash + show boot log
./flash.sh

# autonomous dev loop: rebuilds+reflashes whenever orb/*.ino changes,
# or on demand with:  touch orb/.reflash
./devloop.sh

# manual
arduino-cli compile --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M' orb
arduino-cli upload   --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M' -p /dev/ttyACM0 --input-dir orb/build
```

`tools/serial_sniff.py PORT SECONDS` dumps serial output; the device logs
`[TAMA] ...` events (hatch, sleep, evolve, periodic stats).

## Notes

- The 240×240 RGB565 framebuffer is backed by a static buffer, not `malloc` —
  the C3's largest heap block (~114.7 KB) is just barely smaller than the
  115.2 KB a canvas needs, which would otherwise crash the board.
- Rendering is integer math only (the C3 has no FPU).
- FreeSansBold fonts are bundled in `orb/Fonts/`.
