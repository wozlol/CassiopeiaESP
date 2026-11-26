"""Builds search packs from the Project NOMAD health collections, which are public domain US government material.
Usage: a11json2pack.py home_remedies.json OUT "Pack title" [weight]    (also natural_remedies.json)
The reader finds each remedy by its name, its common names and the conditions it is used for."""
import sys, os, re, json, struct, zlib, math, importlib.util
from array import array
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('a11pack', os.path.join(here, 'a11pack.py'))
a11 = importlib.util.module_from_spec(spec); spec.loader.exec_module(a11)

def write_pack(out, arts, pack_title, pack_weight, pack_source):
    st = open(os.path.join(out, 'titles.str'), 'wb')
    dat = open(os.path.join(out, 'articles.000'), 'wb')
    imgdat = open(os.path.join(out, 'images.dat'), 'wb')
    img_recs = []
    recs, docs, doclen = [], [], array('I')
    postings = {}
    for entry in arts:
        title, text, pics = entry[0], entry[1], entry[2]
        group = entry[3] if len(entry) > 3 else ''
        lines = []
        if True:
            lines.append(f'# {title}')
            lines.append(group)
            lines.append('')
            for fname, jpg in pics:
                img_recs.append((imgdat.tell(), len(jpg)))
                imgdat.write(jpg)
                label = re.sub(r'\.(jpe?g|png|webp)$', '', fname.replace('_', ' '), flags=re.I)[:60]
                lines.append(f'[[img:{len(img_recs) - 1}|Picture: {label}]]')
            if pics: lines.append('')
            body = re.sub(r'(?m)^={2,}\s*(.+?)\s*={2,}\s*$', lambda m: '## ' + m.group(1), text)
            body = re.sub(r'\n{3,}', '\n\n', body)
            lines.append(body[:150000])
            full = '\n'.join(lines)
        b = full.encode('utf-8')
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        cb = c.compress(b) + c.flush()
        off = dat.tell()
        dat.write(cb)
        key = a11.key_of(title)
        koff = st.tell()
        st.write(key.encode('ascii', 'replace') + b'\0' + title.encode('utf-8')[:200] + b'\0')
        recs.append((key, koff, off, len(cb), len(b)))
        doc = len(docs)
        docs.append((off, koff, len(cb), len(b)))
        counts = {}
        for w in a11.tokens(re.sub(r'\[\[[^|\]]*\|([^\]]*)\]\]', r'\1', full)[:a11.INDEX_CHARS]): counts[w] = counts.get(w, 0) + 1
        for w in a11.tokens(title): counts[w] = counts.get(w, 0) + 6
        for w in a11.tokens(group): counts[w] = counts.get(w, 0) + 2
        doclen.append(sum(counts.values()))
        for w, cnt in counts.items(): postings.setdefault(w, array('I')).append((doc << 8) | min(cnt, 255))
    for f in (st, dat, imgdat): f.close()
    recs.sort(key=lambda r: r[0].encode())
    with open(os.path.join(out, 'titles.idx'), 'wb') as f:
        for key, koff, off, cl, rl in recs: f.write(struct.pack('<QIII', off, koff, cl, rl))
    with open(os.path.join(out, 'docs.idx'), 'wb') as f:
        for off, koff, cl, rl in docs: f.write(struct.pack('<QIII', off, koff, cl, rl))
    with open(os.path.join(out, 'images.idx'), 'wb') as f:
        for off, ln in img_recs: f.write(struct.pack('<QI', off, ln))
    N = len(docs); avg = sum(doclen) / N
    words = sorted(postings)
    with open(os.path.join(out, 'words.str'), 'wb') as ws, open(os.path.join(out, 'words.idx'), 'wb') as wi, open(os.path.join(out, 'post.dat'), 'wb') as pd:
        for w in words:
            arr = postings[w]; df = len(arr)
            idf = math.log(1 + (N - df + 0.5) / (df + 0.5))
            scored = []
            for v in arr:
                d, tf = v >> 8, v & 255
                tfw = tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * doclen[d] / avg))
                scored.append((min(255, max(1, int(tfw * idf * 8))), d))
            scored.sort(reverse=True)
            scored = scored[:a11.MAX_POSTINGS]
            wi.write(struct.pack('<III', ws.tell(), pd.tell(), len(scored)))
            ws.write(w.encode() + b'\0')
            pd.write(b''.join(struct.pack('<I', (d << 8) | q) for q, d in scored))
    with open(os.path.join(out, 'pack.txt'), 'w') as f:
        f.write('a11pack 2\ntitle=%s\ncount=%d\ndocs=%d\nwords=%d\nfiles=1\nweight=%d\nimages=%d\nsource=%s\n' % (pack_title, len(recs), N, len(words), pack_weight, len(img_recs), pack_source))
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print(f'{N} articles, {len(img_recs)} pictures, {size // 1024} KB in {out}')


def pretty(slugs): return ', '.join(s.replace('-', ' ') for s in slugs)

def main():
    if len(sys.argv) < 4: sys.exit(__doc__)
    src, out, title = sys.argv[1:4]
    weight = int(sys.argv[4]) if len(sys.argv) > 4 else 110
    d = json.load(open(src))
    os.makedirs(out, exist_ok=True)
    source = d.get('source', {}).get('name', 'Project NOMAD')
    arts = []
    for r in d['remedies']:
        names = ', '.join(r.get('commonNames') or [])
        lines = []
        if names: lines.append('Also called: ' + names)
        if r.get('conditions'): lines.append('Used for: ' + pretty(r['conditions']))
        lines.append('')
        if r.get('cautions'): lines += ['## Cautions', r['cautions'], '']
        if r.get('uses'): lines += ['## What it is used for', r['uses'], '']
        if r.get('how'): lines += ['## How', r['how'], '']
        if r.get('evidence'): lines += ['## What the evidence says', r['evidence'], '']
        lines += ['Source: ' + source + '. ' + (r.get('sourceUrl') or '')]
        group = 'Reference only. ' + (names and 'Also called ' + names + '.' or '')
        arts.append((r['name'], '\n'.join(lines), [], group))
    write_pack(out, arts, title, weight, source[:60])

if __name__ == '__main__':
    main()
