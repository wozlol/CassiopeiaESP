"""Builds a search pack from a PDF manual, one article per page: the page text, and a picture of the page that can be zoomed on the handheld.
Usage: a11manual.py MANUAL.pdf OUT "Pack title" "Short name of the manual" [weight]
Needs pdftotext and pdftoppm from poppler. Run it with the a11 venv python, it uses Pillow."""
import sys, os, re, subprocess, importlib.util
from concurrent.futures import ThreadPoolExecutor
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('a11json2pack', os.path.join(here, 'a11json2pack.py'))
j2p = importlib.util.module_from_spec(spec); spec.loader.exec_module(j2p)

JUNK = re.compile(r'\.book\b|ページ|^\s*\d{1,4}\s*$|^\s*[■▶•]*\s*$')

def page_text(pdf, n):
    t = subprocess.run(['pdftotext', '-f', str(n), '-l', str(n), '-enc', 'UTF-8', '-nopgbrk', pdf, '-'], capture_output=True, text=True).stdout
    lines = [l.rstrip() for l in t.split('\n')]
    lines = [l for l in lines if not re.search(r'\.book\b|[\u3000-\u9fff\uff00-\uffef]', l)]   # print marks with a Japanese date
    return [crumbs(l) for l in lines]

def page_jpeg(pdf, n):
    r = subprocess.run(['pdftoppm', '-jpeg', '-jpegopt', 'quality=55', '-scale-to-x', '960', '-scale-to-y', '-1', '-f', str(n), '-l', str(n), '-singlefile', pdf, '/tmp/a11manual_%d_%d' % (os.getpid(), n)], capture_output=True)
    p = '/tmp/a11manual_%d_%d.jpg' % (os.getpid(), n)
    try:
        b = open(p, 'rb').read(); os.remove(p); return b
    except Exception:
        return b''

def crumbs(l):
    # the arrow between parts of a breadcrumb line comes out of some PDFs as the letter u, as in uuSeat BeltsuFastening
    if re.match(r'^\s*u{1,2}[A-Z]', l) and re.search(r'[a-z]u[A-Z]', l):
        parts = [p.strip() for p in re.split(r'u{1,2}(?=[A-Z])', l) if p.strip()]
        return ' > '.join(parts)
    return l

def heading(lines):
    for l in lines:
        t = l.strip()
        if '▶' in t: t = t.split('▶')[-1].strip()   # a breadcrumb line, keep the last part
        if ' > ' in t: t = t.split(' > ')[-1].strip()
        t = re.sub(r'^u{1,2}(?=[A-Z0-9"])', '', t)            # leftover arrow letters
        t = re.split(r'(?<=[a-z"])u(?=[A-Z])', t)[-1]
        t = re.sub(r'(?<=[a-z])u$', '', t).strip()
        if len(t) >= 4 and not re.match(r'^[\d\W]+$', t): return t[:70]
    return 'Diagram'

def main():
    if len(sys.argv) < 5: sys.exit(__doc__)
    pdf, out, title, short = sys.argv[1:5]
    weight = int(sys.argv[5]) if len(sys.argv) > 5 else 100
    info = subprocess.run(['pdfinfo', pdf], capture_output=True, text=True).stdout
    total = int(re.search(r'Pages:\s+(\d+)', info).group(1))
    os.makedirs(out, exist_ok=True)
    def one(n):
        return n, page_text(pdf, n), page_jpeg(pdf, n)
    arts = []
    done = 0
    with ThreadPoolExecutor(max_workers=8) as ex:
        for n, lines, jpg in ex.map(one, range(1, total + 1)):
            h = heading(lines)
            body = '\n'.join(l for l in lines if l.strip())
            pics = [('page %d' % n, jpg)] if len(jpg) > 2000 else []
            arts.append(('%s page %d: %s' % (short, n, h), body, pics, '%s, page %d of %d.' % (title, n, total)))
            done += 1
            if done % 50 == 0: print('%d/%d pages' % (done, total), flush=True)
    j2p.write_pack(out, arts, title, weight, 'Honda owner\'s manual')

main()
