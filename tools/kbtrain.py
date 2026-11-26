#!/usr/bin/env python3
# Guided keyboard training. Needs the ESP running nrf-tools/nrf_tool and the nRF running nrf/kbscan.
# Usage: kbtrain.py [keymap.json]    Keys are named with USB HID Keyboard/Keypad usage names (HID Usage Tables, page 0x07).
# At each prompt press and release the key. Type s then Enter to skip, x to add a key not in the list, q to save and quit.
import sys, os, json, time, select
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrf

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'keymap', 'a11_keymap.json')

def hid_table():
    t = {}
    for i, c in enumerate('abcdefghijklmnopqrstuvwxyz'): t[c] = 0x04 + i
    for i, c in enumerate('1234567890'): t[c] = 0x1E + i
    t.update({'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B, 'Space': 0x2C, '-': 0x2D, '=': 0x2E,
              '[': 0x2F, ']': 0x30, '\\': 0x31, ';': 0x33, "'": 0x34, '`': 0x35, ',': 0x36, '.': 0x37, '/': 0x38,
              'CapsLock': 0x39})
    for i in range(12): t['F%d' % (i + 1)] = 0x3A + i
    t.update({'Insert': 0x49, 'Home': 0x4A, 'PageUp': 0x4B, 'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E,
              'Right': 0x4F, 'Left': 0x50, 'Down': 0x51, 'Up': 0x52,
              'LeftCtrl': 0xE0, 'LeftShift': 0xE1, 'LeftAlt': 0xE2, 'LeftGUI': 0xE3,
              'RightCtrl': 0xE4, 'RightShift': 0xE5, 'RightAlt': 0xE6, 'RightGUI': 0xE7})
    return t

HID = hid_table()
ORDER = (['Escape'] + list('1234567890-=') + ['Backspace', 'Tab'] + list('qwertyuiop[]') + ['Enter', 'CapsLock'] +
         list("asdfghjkl;'") + ['LeftShift'] + list('zxcvbnm,./') + ['RightShift', 'LeftCtrl', 'LeftAlt', 'LeftGUI',
         'Space', 'RightAlt', 'Left', 'Right', 'Up', 'Down', 'Delete', 'Insert', 'Home', 'End', 'PageUp', 'PageDown'])

fd = nrf.open_port()
time.sleep(0.3)

def snap():
    for line in nrf.run('snap', 5, fd):
        if line.startswith('SNAP '):
            w = line.split()
            n = int(w[2])
            vals = [int(x, 16) for x in w[3:]]
            return vals[:n], vals[32:32 + n]
        if line.startswith('RESULT: FAIL'):
            sys.exit(line)
    return None

def connections(pads, rows):
    return {tuple(sorted((pads[i], pads[j]))) for i in range(len(pads)) for j in range(len(pads)) if rows[i] >> j & 1}

def current(base):
    s = snap()
    if s is None:
        return None
    return connections(*s) - base

def stdin_line():
    r, _, _ = select.select([sys.stdin], [], [], 0)
    return sys.stdin.readline().strip() if r else None

def save(keys, pads, quiet=False):
    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    if os.path.exists(OUT):
        os.replace(OUT, OUT + '.bak')
    json.dump({'pads': pads, 'keys': keys}, open(OUT, 'w'), indent=1)
    if not quiet:
        print('Saved %d keys to %s' % (len(keys), OUT))

def wait_press(base, keys):
    stable, last = 0, None
    while True:
        cmd = stdin_line()
        if cmd is not None:
            return cmd
        cur = current(base)
        if cur:
            stable = stable + 1 if cur == last else 1
            last = cur
            if stable >= 3:
                return cur
        else:
            stable, last = 0, None
        time.sleep(0.02)

def wait_release(base):
    clear = 0
    while clear < 5:
        cur = current(base)
        clear = clear + 1 if not cur else 0
        time.sleep(0.02)

def main():
    s = snap()
    if s is None:
        sys.exit('No snapshot.')
    pads = s[0]
    print('Release every key. Taking the idle baseline for 1.5 seconds.')
    time.sleep(0.5)
    base = set()
    t0 = time.time()
    while time.time() - t0 < 1.5:
        base |= connections(*snap())
    print('Idle connections ignored: %d' % len(base))
    keys = []
    seen = {}
    if os.path.exists(OUT):
        keys = json.load(open(OUT))['keys']
        for k in keys:
            seen[tuple(sorted(tuple(p) for p in k['pairs']))] = k['name']
        print('Loaded %d keys already in %s, skipping those.' % (len(keys), OUT))
    done = {k['name'] for k in keys}
    queue = [n for n in ORDER if n not in done]
    while queue:
        name = queue.pop(0)
        print('Press and release: %s' % name)
        got = wait_press(base, keys)
        if isinstance(got, str):
            if got == 'q':
                break
            if got == 's':
                continue
            if got == 'x':
                custom = input('Name of the next key, then press it: ').strip()
                queue.insert(0, name)
                queue.insert(0, custom)
            continue
        sig = tuple(sorted(got))
        if sig in seen:
            print('  That is already %s. Try again.' % seen[sig])
            wait_release(base)
            queue.insert(0, name)
            continue
        wait_release(base)
        seen[sig] = name
        keys.append({'name': name, 'hid': HID.get(name), 'pairs': [list(p) for p in sig]})
        print('  OK %s uses pads %s' % (name, sorted({p for pair in sig for p in pair})))
        save(keys, pads, quiet=True)
    save(keys, pads)

try:
    main()
except KeyboardInterrupt:
    print('Interrupted, nothing saved.')
