#!/usr/bin/env python3
"""Auto-raise ORB-TAMA from egg to adult. Logs everything."""
import sys
import time
import serial
import datetime

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
LOG = "raise_log.txt"

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S")

class PetSerial:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.dtr = False; self.s.rts = False
        time.sleep(0.15)
        self.s.dtr = True; self.s.rts = True
        time.sleep(2)
        self.s.reset_input_buffer()
        # drain boot messages
        time.sleep(0.5)
        while self.s.in_waiting:
            self.s.read(self.s.in_waiting)
            time.sleep(0.05)
    
    def send(self, c, wait=1.2):
        self.s.reset_input_buffer()
        self.s.write(c.encode()); self.s.flush()
        time.sleep(wait)
        out = b''
        while self.s.in_waiting:
            out += self.s.read(self.s.in_waiting)
            time.sleep(0.05)
        return out.decode('utf-8', 'replace')
    
    def drain(self, secs=1):
        out = b''
        t0 = time.time()
        while time.time() - t0 < secs:
            d = self.s.read(4096)
            if d: out += d
        return out.decode('utf-8', 'replace')
    
    def close(self):
        self.s.close()

def parse_status(text):
    for line in text.split('\n'):
        if '[CMD] age=' in line:
            info = {}
            for p in line.split():
                if p.startswith('age='): info['age'] = p[4:]
                elif p.startswith('h='): info['hunger'] = int(p[2:].rstrip('%'))
                elif p.startswith('f='): info['fun'] = int(p[2:].rstrip('%'))
                elif p.startswith('e='): info['energy'] = int(p[2:].rstrip('%'))
            for p in line.split():
                if '/' in p and p[0].isupper():
                    sp = p.split('/')
                    info['stage'] = sp[0]
            for p in line.split():
                if p.startswith('state='): info['state'] = p[6:]
            return info
    return None

# ---- main ----
ps = PetSerial(PORT)
logf = open(LOG, 'w')
def log(msg):
    line = f"[{ts()}] {msg}"
    print(line, flush=True)
    logf.write(line + '\n'); logf.flush()

log("=== AUTO-RAISE START ===")

# drain boot messages
r = ps.drain(3)
for l in r.split('\n'):
    if l.strip(): log(f"  boot: {l.strip()}")

# Step 1: Check current status
r = ps.send('s')
info = parse_status(r)
if info:
    log(f"  initial: stage={info.get('stage','?')} state={info.get('state','?')} h={info.get('hunger','?')} f={info.get('fun','?')} e={info.get('energy','?')}")

# Step 2: Kill
log("KILLING pet...")
r = ps.send('k')
for l in r.split('\n'):
    if l.strip(): log(f"  {l.strip()}")
time.sleep(1)

# Verify dead
r = ps.send('s')
info = parse_status(r)
if info:
    log(f"  after kill: stage={info.get('stage','?')} state={info.get('state','?')} dead={info.get('state','?')=='DEAD'}")

# Step 3: Re-hatch
log("RE-HATCHING...")
r = ps.send('t')
for l in r.split('\n'):
    if l.strip(): log(f"  {l.strip()}")
time.sleep(1)

# Step 4: Wait for egg phase
log("Waiting for egg to hatch (checking every 15s)...")
egg_start = time.time()
EGG_TIMEOUT = 420
hatched = False

while time.time() - egg_start < EGG_TIMEOUT:
    elapsed = int(time.time() - egg_start)
    r = ps.send('s', wait=1)
    info = parse_status(r)
    stage = info.get('stage', '?') if info else '?'
    state = info.get('state', '?') if info else '?'
    virgin = '?'
    for line in r.split('\n'):
        if 'virgin=' in line:
            for part in line.split():
                if part.startswith('virgin='): virgin = part[7:]
    
    log(f"  egg +{elapsed}s stage={stage} state={state} virgin={virgin}")
    
    if stage in ('BABY', 'TEEN', 'ADULT') and virgin == '0':
        log(f"  *** HATCHED after {elapsed}s! Stage={stage} ***")
        hatched = True
        break
    time.sleep(12)

if not hatched:
    log("EGG TIMEOUT - checking final state...")
    r = ps.send('d')
    for l in r.split('\n'):
        if l.strip(): log(f"  {l.strip()}")
    ps.close(); logf.close()
    sys.exit(1)

# Step 5: Raise to adult
log("=== RAISING PHASE ===")
tick = 0
max_ticks = 360  # 90 min absolute max

while tick < max_ticks:
    tick += 1
    r = ps.send('s', wait=1)
    info = parse_status(r)
    if not info:
        log(f"  tick {tick}: no status, draining...")
        ps.drain(2)
        time.sleep(15)
        continue
    
    stage = info.get('stage', '?')
    state = info.get('state', '?')
    h = info.get('hunger', -1)
    f = info.get('fun', -1)
    e = info.get('energy', -1)
    age = info.get('age', '?')
    
    log(f"  tick {tick}: {stage} st={state} age={age} h={h}% f={f}% e={e}%")
    
    if stage == 'ADULT':
        log(f"  *** ADULT REACHED! age={age} h={h}% f={f}% e={e}% ***")
        break
    
    if state == 'DEAD':
        log(f"  *** PET DIED at tick {tick}! Re-hatching... ***")
        r = ps.send('t')
        for l in r.split('\n'):
            if l.strip() and 'CMD' in l: log(f"    {l.strip()}")
        time.sleep(2)
        continue
    
    actions = []
    if h < 40 and state not in ('SLEEP',):
        r = ps.send('f')
        for l in r.split('\n'):
            if 'CMD' in l: log(f"    {l.strip()}")
        actions.append('feed')
    
    if f < 40 and state not in ('SLEEP',):
        r = ps.send('n')
        for l in r.split('\n'):
            if 'CMD' in l: log(f"    {l.strip()}")
        actions.append('fun')
    
    if e < 35 and state not in ('SLEEP',):
        r = ps.send('e')
        for l in r.split('\n'):
            if 'CMD' in l: log(f"    {l.strip()}")
        actions.append('energy')
    
    if actions:
        log(f"    => {', '.join(actions)}")
    
    time.sleep(12)

# Final status
log("=== FINAL STATUS ===")
r = ps.send('d', wait=2)
for l in r.split('\n'):
    if l.strip(): log(f"  {l.strip()}")

r = ps.send('s')
info = parse_status(r)
if info:
    log(f"  FINAL: {info.get('stage','?')} h={info.get('hunger','?')}% f={info.get('fun','?')}% e={info.get('energy','?')}% age={info.get('age','?')}")

log("=== AUTO-RAISE DONE ===")
ps.close()
logf.close()
