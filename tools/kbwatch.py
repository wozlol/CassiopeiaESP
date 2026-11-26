#!/usr/bin/env python3
# Usage: kbwatch.py [seconds]   Prints which pads connect whenever the key state changes. Release all keys at start.
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrf

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
fd = nrf.open_port()
time.sleep(0.3)

def snap():
    for line in nrf.run('snap', 5, fd):
        if line.startswith('SNAP '):
            w = line.split()
            n = int(w[2])
            v = [int(x, 16) for x in w[3:]]
            pads, rows = v[:n], v[32:32 + n]
            return {tuple(sorted((pads[i], pads[j]))) for i in range(n) for j in range(n) if rows[i] >> j & 1}
    return None

base = snap() or set()
print('Idle connections ignored: %d %s' % (len(base), sorted(base)[:3]), flush=True)
print('Watching for %d seconds, press keys now.' % secs, flush=True)
last = set()
t0 = time.time()
while time.time() - t0 < secs:
    cur = snap()
    if cur is None:
        continue
    cur -= base
    if cur != last:
        print('%5.1fs %s' % (time.time() - t0, ('pressed ' + str(sorted(cur))) if cur else 'released'), flush=True)
        last = cur
    time.sleep(0.01)
print('Done.')
