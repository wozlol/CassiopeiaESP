#!/usr/bin/env python3
# Usage: kbtype.py [seconds]   Live typing test. Prints key names from keymap/a11_keymap.json as you press them, then lists keys never pressed.
# Release all keys at start. Pairs that match no trained key print as UNKNOWN.
import sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrf

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
here = os.path.dirname(os.path.abspath(__file__))
km = json.load(open(os.path.join(here, '..', 'keymap', 'a11_keymap.json')))
by_pair = {}
for k in km['keys']:
    for p in k['pairs']:
        by_pair[tuple(sorted(p))] = k['name']

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
print('Idle connections ignored: %d' % len(base), flush=True)
print('Type for %d seconds. Chords show every key held.' % secs, flush=True)
seen = set()
last = set()
prev = None
t0 = time.time()
while time.time() - t0 < secs:
    cur = snap()
    if cur is None:
        continue
    cur -= base
    if cur == prev and cur != last:
        names = sorted(by_pair.get(p, 'UNKNOWN%s' % (p,)) for p in cur)
        seen.update(n for n in names if not n.startswith('UNKNOWN'))
        print('%5.1fs %s' % (time.time() - t0, ' + '.join(names) if names else '(released)'), flush=True)
        last = cur
    prev = cur
    time.sleep(0.01)
missing = sorted(set(by_pair.values()) - seen)
print('Pressed %d of %d keys.' % (len(seen), len(set(by_pair.values()))))
print('Never pressed: %s' % (', '.join(missing) if missing else 'none'))
