#!/usr/bin/env python3
# Usage: loader.py list | sd | install <file> <ota_0|ota_1> | boot <ota_0|ota_1> | rm <file on card> | rmpack <pack folder under library> | put <local file> [name on card] | putdir <local folder> <card folder under library or media> | format CONFIRM | nrf wire | nrf flash <bin file>
# Talks to modes/loader over the ESP USB serial port. Python standard library only.
import os, sys, glob, termios, time, select

def open_port():
    ports = glob.glob('/dev/cu.usbmodem*')
    if not ports:
        sys.exit('No ESP on USB.')
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd

def read_until(fd, marks, timeout):
    buf = b''
    t0 = time.time()
    while time.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                buf += os.read(fd, 4096)
            except BlockingIOError:
                continue
            text = buf.decode(errors='replace')
            for m in marks:
                if m in text:
                    return text
    return buf.decode(errors='replace') + '\nTIMED OUT'

def write_all(fd, data):
    # Non blocking port, a write can take only part of the data, so loop until all of it is sent.
    while data:
        try:
            n = os.write(fd, data)
            data = data[n:]
        except BlockingIOError:
            select.select([], [fd], [], 1.0)

def send(fd, line, marks=('READY',), timeout=30):
    write_all(fd, (line + '\n').encode())
    return read_until(fd, marks, timeout)

def put_file(fd, path, name):
    data = open(path, 'rb').read()
    out = send(fd, 'put %s %d' % (name, len(data)), ('SEND', 'FAIL'), 10)
    if 'SEND' not in out:
        sys.exit(out)
    for i in range(0, len(data), 4096):
        write_all(fd, data[i:i + 4096])
        out = read_until(fd, ('A\r\n', 'FAIL'), 10)
        if 'FAIL' in out or 'TIMED OUT' in out:
            sys.exit(out)
    print(read_until(fd, ('READY',), 30).strip().splitlines()[0])

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fd = open_port()
    time.sleep(0.3)
    cmd = sys.argv[1]
    if cmd == 'put':
        name = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(sys.argv[2])
        put_file(fd, sys.argv[2], name)
    elif cmd == 'putdir':
        # putdir LOCAL_FOLDER CARD_FOLDER, for example: putdir /tmp/a11pack_out library/alpine
        local, card = sys.argv[2], sys.argv[3].strip('/')
        for root, _, files in os.walk(local):
            for fn in sorted(files):
                if fn.startswith('.'):
                    continue
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, local)
                put_file(fd, full, card + '/' + rel)
    elif cmd == 'nrf' and len(sys.argv) > 2 and sys.argv[2] == 'flash':
        data = open(sys.argv[3], 'rb').read()
        out = send(fd, 'nrf flash %d CONFIRM' % len(data), ('SEND', 'FAIL'), 10)
        if 'SEND' not in out:
            sys.exit(out)
        for i in range(0, len(data), 4096):
            write_all(fd, data[i:i + 4096])
            out = read_until(fd, ('A\r\n', 'FAIL'), 10)
            if 'FAIL' in out or 'TIMED OUT' in out:
                sys.exit(out)
        print(read_until(fd, ('READY',), 60))
    elif cmd == 'nrf':
        print(send(fd, ' '.join(sys.argv[1:]), ('READY',), 30))
    elif cmd == 'rmpack':
        print(send(fd, ' '.join(sys.argv[1:]), ('OK', 'FAIL'), 240))
    elif cmd == 'format':
        print(send(fd, ' '.join(sys.argv[1:]), ('READY',), 240))
    elif cmd in ('list', 'sd', 'install', 'boot', 'rm'):
        print(send(fd, ' '.join(sys.argv[1:])))
    else:
        sys.exit(__doc__)

if __name__ == '__main__':
    main()
