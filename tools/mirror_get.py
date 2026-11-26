#!/usr/bin/env python3
"""Downloads a big Kiwix file from several mirrors at once, in pieces, and keeps going after a stop.
Usage: mirror_get.py URL_OF_THE_ZIM OUTPUT_FILE
It reads the mirror list from URL.meta4, times each mirror for a few seconds, and uses the fast ones, two connections each.
Progress is kept next to the output in OUTPUT_FILE.parts. An existing partial file from a plain download is reused."""
import sys, os, re, json, time, threading, hashlib, urllib.request

SEG = 32 * 1024 * 1024

def fetch(url, rng=None, timeout=30):
    req = urllib.request.Request(url, headers={'User-Agent': 'a11-mirror-get'})
    if rng: req.add_header('Range', 'bytes=%d-%d' % rng)
    return urllib.request.urlopen(req, timeout=timeout)

def mirrors_of(url):
    meta = fetch(url + '.meta4').read().decode('utf8', 'replace')
    size = int(re.search(r'<size>(\d+)</size>', meta).group(1))
    sha = re.search(r'<hash type="sha-256">([0-9a-f]+)</hash>', meta)
    urls = re.findall(r'<url[^>]*location="[^"]*"[^>]*>([^<]+)</url>', meta)
    return size, (sha.group(1) if sha else None), urls

def speed(u, seconds=6):
    t0 = time.time(); got = 0
    try:
        r = fetch(u, (60_000_000, 120_000_000), timeout=12)
        while time.time() - t0 < seconds:
            b = r.read(65536)
            if not b: break
            got += len(b)
    except Exception:
        return 0
    return got / max(0.5, time.time() - t0)

def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    url, out = sys.argv[1:]
    size, sha, urls = mirrors_of(url)
    print('size %.1f GB, %d mirrors' % (size / 1e9, len(urls)), flush=True)
    sp = [(speed(u), u) for u in urls]
    for s, u in sorted(sp, reverse=True): print('  %.2f MB/s  %s' % (s / 1e6, u.split('/')[2]), flush=True)
    good = [u for s, u in sorted(sp, reverse=True) if s > 1.2e6][:3] or [u for s, u in sorted(sp, reverse=True)[:2]]
    state_f = out + '.parts'
    nseg = (size + SEG - 1) // SEG
    done = set(json.load(open(state_f))) if os.path.exists(state_f) else set()
    if not os.path.exists(out): open(out, 'wb').close()
    have = os.path.getsize(out)
    if not done and have > 2 * SEG:                       # a plain partial download: its start is good
        done = set(range(0, max(0, have // SEG - 1)))
    fd = os.open(out, os.O_RDWR)
    if os.fstat(fd).st_size < size: os.ftruncate(fd, size)
    todo = [i for i in range(nseg) if i not in done]
    print('%d of %d pieces to go, using %s' % (len(todo), nseg, ', '.join(u.split('/')[2] for u in good)), flush=True)
    lock = threading.Lock(); t0 = time.time(); fetched = [0]
    def save():
        tmp = state_f + '.tmp'; json.dump(sorted(done), open(tmp, 'w')); os.replace(tmp, state_f)
    def worker(mirror, k):
        while True:
            with lock:
                if not todo: return
                i = todo.pop(0)
            a, b = i * SEG, min(size, (i + 1) * SEG) - 1
            ok = False
            for attempt in range(6):
                try:
                    r = fetch(mirror, (a, b), timeout=40); pos = a
                    while pos <= b:
                        buf = r.read(min(1 << 20, b - pos + 1))
                        if not buf: raise IOError('short')
                        os.pwrite(fd, buf, pos); pos += len(buf)
                        with lock: fetched[0] += len(buf)
                    ok = True; break
                except Exception:
                    time.sleep(5 + attempt * 5)
            with lock:
                if ok:
                    done.add(i)
                    if len(done) % 8 == 0: save()
                else: todo.append(i)
                if not ok: time.sleep(1)
    ths = []
    for m in good:
        for k in range(2):
            t = threading.Thread(target=worker, args=(m, k), daemon=True); t.start(); ths.append(t)
    while any(t.is_alive() for t in ths):
        time.sleep(30)
        with lock: n = len(done); f = fetched[0]
        print('%s  %d/%d pieces  %.1f MB/s' % (time.strftime('%H:%M:%S'), n, nseg, f / max(1, time.time() - t0) / 1e6), flush=True)
    save()
    if len(done) != nseg: sys.exit('not all pieces arrived, run it again')
    os.close(fd)
    if sha:
        h = hashlib.sha256()
        with open(out, 'rb') as f:
            for chunk in iter(lambda: f.read(8 << 20), b''): h.update(chunk)
        if h.hexdigest() != sha: sys.exit('checksum does not match, delete the file and run it again')
        print('checksum ok', flush=True)
    print('done', flush=True)

main()
