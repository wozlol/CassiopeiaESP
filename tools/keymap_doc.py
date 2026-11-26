#!/usr/bin/env python3
# Usage: keymap_doc.py   Writes keymap/A11_KEYBOARD.md and keymap/a11_keymap_connector.json from keymap/a11_keymap.json.
import json, os, collections
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'keymap')
d = json.load(open(os.path.join(root, 'a11_keymap.json')))

# As built: keyboard connector pin -> nRF pad. Pin 1 is not connected, pin 2 is keyboard ground and goes to an nRF GND pad.
PIN_TO_PAD = {3: 29, 4: 28, 5: 24, 6: 23, 7: 22, 8: 20, 9: 19, 10: 18, 11: 17, 12: 16, 13: 2, 14: 3, 15: 4,
              16: 5, 17: 8, 18: 11, 19: 12, 20: 13, 21: 14, 22: 15, 23: 31, 24: 9, 25: 10, 26: 21}
PAD_TO_PIN = {v: k for k, v in PIN_TO_PAD.items()}
HEADER = {31: 'J3 5', 30: 'J3 6', 29: 'J3 7', 28: 'J3 8', 24: 'J3 10', 23: 'J3 11', 22: 'J3 12', 20: 'J3 13', 19: 'J3 14',
          18: 'J3 15', 17: 'J3 16', 16: 'J3 17', 21: 'J3 4', 2: 'J4 6', 3: 'J4 7', 4: 'J4 8', 5: 'J4 9', 8: 'J4 10',
          9: 'J4 11', 10: 'J4 12', 11: 'J4 13', 12: 'J4 14', 13: 'J4 15', 14: 'J4 16', 15: 'J4 17'}
SHOW = {'-': 'Minus', '=': 'Equals', '[': 'LeftBracket', ']': 'RightBracket', '\\': 'Backslash', ';': 'Semicolon',
        "'": 'Apostrophe', '`': 'Grave', ',': 'Comma', '.': 'Period', '/': 'Slash'}

keys = []
for k in d['keys']:
    a, b = k['pairs'][0]
    p = sorted([PAD_TO_PIN[a], PAD_TO_PIN[b]])
    keys.append({'name': SHOW.get(k['name'], k['name']), 'hid': k['hid'], 'pins': p, 'pads': sorted([a, b])})

# Two coloring of the pin graph: every key joins one row side pin to one column side pin.
adj = collections.defaultdict(set)
for k in keys:
    x, y = k['pins']
    adj[x].add(y); adj[y].add(x)
color = {}
for start in sorted(adj, key=lambda p: (p != PAD_TO_PIN[2], p)):
    if start in color: continue
    color[start] = 0
    stack = [start]
    while stack:
        n = stack.pop()
        for m in adj[n]:
            if m not in color:
                color[m] = 1 - color[n]; stack.append(m)
            elif color[m] == color[n]:
                raise SystemExit('Not a two sided matrix at pin %d' % n)
rows = sorted(p for p in adj if color[p] == 0)
cols = sorted(p for p in adj if color[p] == 1)
cell = {}
for k in keys:
    x, y = k['pins']
    r, c = (x, y) if color[x] == 0 else (y, x)
    cell[(r, c)] = k['name']

pinuse = collections.Counter(p for k in keys for p in k['pins'])
immune = sorted(k['name'] for k in keys if all(pinuse[p] == 1 for p in k['pins']) or
                all(len(adj[p]) == len(adj[k['pins'][0]]) and False for p in k['pins']))
# a key cannot ghost if it is not part of any four key rectangle
rect = set()
for x in adj:
    for y in adj[x]:
        for z in adj[y] - {x}:
            for w in adj[z] - {y}:
                if x in adj[w] and w != y:
                    rect.update({frozenset((x, y)), frozenset((y, z)), frozenset((z, w)), frozenset((w, x))})
clean = sorted(k['name'] for k in keys if frozenset(k['pins']) not in rect)

out = []
w = out.append
w('# Casio Cassiopeia A-11 keyboard map\n')
w('Electrical map of the A-11 keyboard (marking z305-KY C140360-1), measured with the scanner in nrf/kbscan and trained with tools/kbtrain.py. Plain passive matrix, no diodes. Every key joins exactly one row pin to one column pin.\n')
w('## Connector pins\n')
w('The keyboard connector has 26 pins. Turn the keyboard face down so the connector traces on the PCB are visible, then count from right to left. Pin 1 is not connected and was not wired. Pin 2 is the keyboard ground and is wired to an nRF GND pad.\n')
w('| Connector pin | nRF pad | nRF header | Side |')
w('|---|---|---|---|')
for pin in range(1, 27):
    if pin == 1:
        w('| 1 | none | none | not connected |'); continue
    if pin == 2:
        w('| 2 | GND | GND pad | ground |'); continue
    pad = PIN_TO_PAD[pin]
    side = 'row' if pin in rows else 'column' if pin in cols else 'no key'
    w('| %d | %d | %s | %s |' % (pin, pad, HEADER[pad], side))
w('')
w('Pad numbers are the SparkFun WRL-13990 silkscreen numbers, which are nRF52832 P0.xx numbers. Pads 9, 10 and 21 are normally NFC and reset pins and need `tools/nrf.py freepads CONFIRM` once.\n')
w('## Matrix\n')
w('Rows are connector pins down the side, columns are connector pins across the top.\n')
w('| row \\ col | ' + ' | '.join(str(c) for c in cols) + ' |')
w('|---|' + '---|' * len(cols))
for r in rows:
    w('| %d | ' % r + ' | '.join(cell.get((r, c), '') for c in cols) + ' |')
w('')
w('## Keys\n')
w('Names are USB HID Keyboard/Keypad page 0x07 names where one exists. Keys with no HID usage are device buttons.\n')
w('| Key | HID usage | Pins |')
w('|---|---|---|')
for k in sorted(keys, key=lambda k: (k['pins'][0], k['pins'][1])):
    hid = '0x%02X' % k['hid'] if k['hid'] is not None else 'none'
    w('| %s | %s | %d and %d |' % (k['name'], hid, k['pins'][0], k['pins'][1]))
w('')
w('## Ghosting\n')
w('Without diodes, holding three keys at the corners of a rectangle in the matrix reads as a fourth key. Keys that sit in no such rectangle never ghost: %s.\n' % ', '.join(clean))
open(os.path.join(root, 'A11_KEYBOARD.md'), 'w').write('\n'.join(out) + '\n')
json.dump({'connector': 'A-11 keyboard, 26 pins, pin 1 not connected, counted right to left with the keyboard face down',
           'ground_pins': [2], 'pin_to_pad': PIN_TO_PAD, 'rows': rows, 'columns': cols, 'keys': keys},
          open(os.path.join(root, 'a11_keymap_connector.json'), 'w'), indent=1)
print('wrote', len(keys), 'keys, rows', rows, 'columns', cols)
print('ghost free:', clean)
