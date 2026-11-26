#!/usr/bin/env python3
# Usage: nrf.py info | verify | run | snap | dump <out.hex> | flash CONFIRM | freepads CONFIRM
# Talks to nrf-tools/nrf_tool over the ESP USB serial port. Python standard library only.
import os, sys, glob, termios, fcntl, time, select, struct

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
    fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', 0x002))
    return fd

def run(cmd, timeout, fd=None):
    if fd is None:
        fd = open_port()
        time.sleep(0.2)
    os.write(fd, (cmd + '\n').encode())
    buf = b''
    t0 = time.time()
    lines = []
    while time.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            buf += os.read(fd, 65536)
        except BlockingIOError:
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.decode(errors='replace').rstrip('\r')
            lines.append(line)
            if line.startswith('RESULT:'):
                return lines
    lines.append('RESULT: FAIL timed out waiting for the ESP')
    return lines

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'dump':
        out = sys.argv[2]
        lines = run('dump', 180)
        hexlines = [l for l in lines if l.startswith(':')]
        with open(out, 'w') as f:
            f.write('\n'.join(hexlines) + '\n')
        for l in lines:
            if not l.startswith(':'):
                print(l)
        print('Saved %d hex records to %s' % (len(hexlines), out))
    elif cmd == 'flash':
        if len(sys.argv) < 3 or sys.argv[2] != 'CONFIRM':
            sys.exit('Add CONFIRM to erase and write the nRF.')
        print('\n'.join(run('flash CONFIRM', 60)))
    elif cmd == 'freepads':
        if len(sys.argv) < 3 or sys.argv[2] != 'CONFIRM':
            sys.exit('Add CONFIRM to free pads 9, 10 and 21.')
        print('\n'.join(run('freepads CONFIRM', 30)))
    elif cmd in ('info', 'verify', 'run', 'snap'):
        print('\n'.join(run(cmd, 30)))
    else:
        sys.exit(__doc__)

if __name__ == '__main__':
    main()
