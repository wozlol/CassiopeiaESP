#!/usr/bin/env python3
# Usage: a11pack.py FILE.zim OUTFOLDER [--limit N] [--name TITLE] [--weight PERCENT] [--ocr] [--no-fulltext]
# --images keeps pictures (diagrams, rashes, wounds, insects, plants) as small JPEGs, never faces or people (macOS Vision), at most 4 an article, --max-images N and --image-mb N cap the amount.
# --ocr reads scanned PDFs of 3 or more pages with the text recognition built into macOS (needs pyobjc-framework-Vision in the venv, and pdftoppm). Slow, about 2 seconds a page, cached.
# --weight ranks this pack's search results higher or lower than the others, 100 is normal. Forum Q and A should be well below reference books.
# Turns a Kiwix ZIM file into a reference pack for the handheld's reader. Copy OUTFOLDER to /library on the SD card.
# The pack folder holds:
#   pack.txt     what it is, number of articles and files
#   titles.idx   one 20 byte record per title and redirect, sorted by key: article offset (8, the top 2 bytes are the file number), key offset (4), compressed length (4), text length (4)
#   titles.str   for each title the lower case ASCII key, a zero, the real title, a zero
#   docs.idx     the same kind of record, one per real article in the order they were written (full text results point here)
#   articles.000 articles.001 ...  each article as plain text with a few markers, compressed with deflate, each file under 1.8 GB
#   words.str words.idx post.dat   full text index: sorted stemmed words, a record per word (string offset, postings offset, count), postings are
#                                  4 bytes each, article number in the top 24 bits and a weight in the low 8, strongest first, at most 2000 per word
# PDFs inside a ZIM are read with pdftotext and cut into parts of about 50 KB with Previous part and Next part links.
# Article markers: lines starting with #, ##, ### are headings, "- " is a list item, four spaces start code, [[key|text]] is a link, key is the lower case key of the target title.
# Needs libzim: python3 -m venv v; v/bin/pip install libzim. Run it with the -I option when the file is not yours.
import sys, os, re, struct, zlib, unicodedata, urllib.parse, math
from array import array
from html.parser import HTMLParser
from libzim.reader import Archive

MAX_ARTICLE = 400 * 1024
FILE_MAX = 1_800_000_000
INDEX_CHARS = 20000
MAX_POSTINGS = 2000
STOP = set('the and for with that this from are was were been have has had not but you your can may will all any one two use used using such also into than then there their they them its his her she him who whom what when where which while about over under more most other some each both between per via etc would could should these those only very just like how why out off our own same too'.split())

def stem(w):
    if len(w) > 4 and w.endswith('ies'): return w[:-3] + 'y'
    if len(w) > 4 and (w.endswith('ches') or w.endswith('shes') or w.endswith('sses') or w.endswith('xes') or w.endswith('zes')): return w[:-2]
    if len(w) > 3 and w.endswith('s') and not w.endswith(('ss', 'us', 'is')): return w[:-1]
    return w

def tokens(text):
    t = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode().lower()
    return [stem(w) for w in re.findall(r'[a-z0-9]{2,24}', t) if w not in STOP]
BLOCK = {'p', 'div', 'br', 'tr', 'ul', 'ol', 'table', 'section', 'blockquote', 'dl', 'dt', 'dd', 'figure', 'figcaption', 'header', 'footer'}
SKIP = {'script', 'style', 'head', 'nav', 'noscript', 'svg', 'math', 'button', 'form'}

def key_of(title):
    k = unicodedata.normalize('NFKD', title).encode('ascii', 'ignore').decode().lower()
    k = re.sub(r'\s+', ' ', k).strip()
    return (k or title.casefold())[:100]

class Html2Text(HTMLParser):
    def __init__(self, resolve):
        super().__init__(convert_charrefs=True)
        self.resolve = resolve
        self.out = []
        self.skip = 0
        self.skip_tag = None
        self.images = []
        self.pre = 0
        self.href = None
        self.link_text = []
        self.heading = 0

    def nl(self, n=1):
        s = ''.join(self.out)
        have = len(s) - len(s.rstrip('\n'))
        if s and have < n:
            self.out.append('\n' * (n - have))

    VOID = {'br', 'img', 'meta', 'link', 'hr', 'input', 'wbr', 'source', 'area', 'base', 'col', 'embed', 'param', 'track'}

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if self.skip:   # inside something we throw away, count nested tags of the same name so we know where it ends
            if tag == self.skip_tag and tag not in self.VOID: self.skip += 1
            return
        cls = a.get('class') or ''
        if tag == 'img':
            src = a.get('src') or ''
            if src and 'math' not in cls and 'static/images' not in src:
                self.images.append((src, a.get('alt') or ''))
                self.out.append('\uE000%d\uE001' % (len(self.images) - 1))
            return
        if tag in SKIP or 'reference' in cls or cls in ('mw-editsection', 'navbox') or 'navbox' in cls.split() or 'noprint' in cls.split() or 'mw-empty-elt' in cls:
            if tag not in self.VOID:
                self.skip, self.skip_tag = 1, tag
            return
        if re.fullmatch(r'h[1-6]', tag):
            self.nl(2); self.heading = int(tag[1]); self.out.append('#' * min(self.heading, 3) + ' ')
        elif tag == 'li':
            self.nl(); self.out.append('- ')
        elif tag == 'pre':
            self.nl(2); self.pre += 1
        elif tag == 'br':
            self.nl()
        elif tag in BLOCK:
            self.nl(2 if tag in ('p', 'blockquote') else 1)
        elif tag == 'a' and a.get('href'):
            self.href = a['href']; self.link_text = []

    def handle_endtag(self, tag):
        if self.skip:
            if tag == self.skip_tag:
                self.skip -= 1
            return
        if re.fullmatch(r'h[1-6]', tag):
            self.heading = 0; self.nl(2)
        elif tag == 'pre':
            self.pre = max(0, self.pre - 1); self.nl(2)
        elif tag in ('li',) or tag in BLOCK:
            self.nl(2 if tag in ('p', 'blockquote') else 1)
        elif tag == 'a' and self.href is not None:
            text = ''.join(self.link_text).strip()
            target = self.resolve(self.href)
            self.out.append(f'[[{key_of(target)}|{text or target}]]' if target else text)
            self.href = None

    def handle_data(self, data):
        if self.skip:
            return
        if self.pre:
            for line in data.split('\n'):
                self.out.append('    ' + line + '\n') if line.strip() else self.out.append('\n')
            return
        text = re.sub(r'\s+', ' ', data)
        if self.href is not None:
            self.link_text.append(text)
        else:
            if text.strip() or (self.out and not ''.join(self.out[-1:]).endswith((' ', '\n'))):
                self.out.append(text)

JUNK_PATH = re.compile(r'(tagged|_page=|(^|/)(users?|tags|unanswered|badges|questions\?|search)(/|$|\?))')
NAV_LINE = re.compile(r'(?m)^- (\[\[[^\]]*\|)?(Questions|Tags|Users|About|Unanswered|Ask Question|Help|Home|Companies)(\]\])?\n')

def clean(text):
    text = NAV_LINE.sub('', text)
    text = re.sub(r'[ \t]+\n', '\n', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    text = re.sub(r'(?m)^[ ]+(?=\S)(?!-)', '', text) if False else text
    return text.strip() + '\n'

def pdf_text(data):
    import subprocess, tempfile
    with tempfile.NamedTemporaryFile(suffix='.pdf') as t:
        t.write(data); t.flush()
        r = subprocess.run(['pdftotext', '-enc', 'UTF-8', '-nopgbrk', t.name, '-'], capture_output=True, timeout=300)
    return r.stdout.decode('utf-8', 'replace')

OCR_CACHE = os.path.expanduser('~/a11-staging/ocr_cache')

def ocr_pdf(data):
    """Reads a scanned PDF with the text recognition built into macOS (Vision). Results are cached by file hash so a rerun is fast."""
    import hashlib, subprocess, tempfile, glob
    import Vision
    from Foundation import NSURL
    os.makedirs(OCR_CACHE, exist_ok=True)
    cache = os.path.join(OCR_CACHE, hashlib.sha1(data).hexdigest() + '.txt')
    if os.path.exists(cache):
        return open(cache, encoding='utf-8').read()
    out = []
    with tempfile.TemporaryDirectory() as td:
        pdf = os.path.join(td, 'in.pdf')
        open(pdf, 'wb').write(data)
        subprocess.run(['pdftoppm', '-r', '200', '-gray', '-png', pdf, os.path.join(td, 'p')], check=True, timeout=1800)
        pages = sorted(glob.glob(os.path.join(td, 'p-*.png')))
        for n, img in enumerate(pages):
            handler = Vision.VNImageRequestHandler.alloc().initWithURL_options_(NSURL.fileURLWithPath_(img), None)
            req = Vision.VNRecognizeTextRequest.alloc().init()
            req.setRecognitionLevel_(Vision.VNRequestTextRecognitionLevelAccurate)
            req.setUsesLanguageCorrection_(True)
            ok, err = handler.performRequests_error_([req], None)
            lines = []
            for obs in (req.results() or []):
                cands = obs.topCandidates_(1)
                if cands: lines.append(str(cands[0].string()))
            out.append('\n'.join(lines))
            if n % 10 == 0: print(f'   ocr page {n + 1}/{len(pages)}', flush=True)
    text = '\n\n'.join(out)
    open(cache, 'w', encoding='utf-8').write(text)
    return text

def pdf_title(data):
    import subprocess, tempfile
    try:
        with tempfile.NamedTemporaryFile(suffix='.pdf') as t:
            t.write(data); t.flush()
            r = subprocess.run(['pdfinfo', t.name], capture_output=True, timeout=60)
        for line in r.stdout.decode('utf-8', 'replace').split('\n'):
            if line.startswith('Title:'):
                v = line[6:].strip()
                if len(v) > 4 and not v.lower().endswith(('.doc', '.docx', '.pdf', '.indd')) and 'untitled' not in v.lower(): return v
    except Exception:
        pass
    return None

def reflow(raw):
    # join the lines of each paragraph, undo line end hyphens, make SHORT CAPITAL lines headings
    out = []
    for para in re.split(r'\n\s*\n', raw):
        lines = [l.strip() for l in para.split('\n') if l.strip()]
        if not lines: continue
        if len(lines) == 1 and len(lines[0]) < 80 and lines[0].isupper() and len(lines[0]) > 3:
            out.append('## ' + lines[0].title()); continue
        text = ''
        for l in lines:
            if text.endswith('-') and l[:1].islower(): text = text[:-1] + l
            else: text = (text + ' ' + l).strip()
        out.append(re.sub(r'\s+', ' ', text))
    return '\n\n'.join(out)

def split_parts(text, size=50000):
    paras = text.split('\n\n')
    parts, cur, n = [], [], 0
    for p in paras:
        if n + len(p) > size and cur:
            parts.append('\n\n'.join(cur)); cur, n = [], 0
        cur.append(p); n += len(p) + 2
    if cur: parts.append('\n\n'.join(cur))
    return parts

# ---------- pictures ----------
IMG_CACHE = os.path.expanduser('~/a11-staging/img_cache')
IMG_SKIP = re.compile(r'(range[-_ ]?map|distribution|status_|iucn|cites_|cladogram|phylogen|flag_of|logo|icon|symbol|ambox|question_book|edit-|padlock|nuvola|portal|commons-|wiki(pedia|media|data|source|books)|button|bullet|arrow|star_|stub|ooui|folder|speaker|sound|play_|crystal|gnome-|oxygen|blank|spacer|pixel|map_of|locator|location_map|signature|coat_of_arms|seal_of)', re.I)

def has_people(jpeg):
    """True if macOS Vision finds a face or a person in the picture. Close ups of skin, wounds, plants and insects pass."""
    import Vision
    from Foundation import NSData
    data = NSData.dataWithBytes_length_(jpeg, len(jpeg))
    handler = Vision.VNImageRequestHandler.alloc().initWithData_options_(data, None)
    faces = Vision.VNDetectFaceRectanglesRequest.alloc().init()
    humans = Vision.VNDetectHumanRectanglesRequest.alloc().init()
    try: humans.setUpperBodyOnly_(False)
    except Exception: pass
    ok, err = handler.performRequests_error_([faces, humans], None)
    if not ok: return True      # when in doubt leave it out
    if faces.results(): return True
    for o in (humans.results() or []):
        b = o.boundingBox()
        if o.confidence() > 0.3 and b.size.width * b.size.height > 0.12: return True
    return False

DROP_LINEART = False
def is_lineart(jpg):
    """Sparse black or grey line drawings on a white page, like chemical structures and plain charts. Photos, scans, colourful
    diagrams and dense engravings, like old anatomy plates, are not. Tuned on a sheet of 48 real pictures."""
    import io
    from PIL import Image
    im = Image.open(io.BytesIO(jpg)).convert('RGB')
    im.thumbnail((120, 80))
    px = list(im.getdata())
    n = len(px)
    white = sum(1 for r, g, b in px if min(r, g, b) > 225) / n
    nonwhite = [p for p in px if min(p) <= 225]
    sat = sum(max(p) - min(p) for p in nonwhite) / max(1, len(nonwhite)) / 255
    return white > 0.85 and sat < 0.12

def prep_image(raw, name=''):
    """Raw picture bytes to a small baseline JPEG that fits the 480x320 screen, or None if it should not be used."""
    import hashlib, io
    from PIL import Image
    h = hashlib.sha1(raw).hexdigest()
    os.makedirs(IMG_CACHE, exist_ok=True)
    cj, cn = os.path.join(IMG_CACHE, h + '.jpg'), os.path.join(IMG_CACHE, h + '.no')
    if os.path.exists(cj):
        jpg = open(cj, 'rb').read()
        return None if DROP_LINEART and is_lineart(jpg) else jpg
    if os.path.exists(cn): return None
    def no():
        open(cn, 'w').close(); return None
    if len(raw) < 3000 or IMG_SKIP.search(name): return no()
    try:
        if raw[:200].lstrip().startswith((b'<?xml', b'<svg')) or b'<svg' in raw[:400]:
            import cairosvg
            raw = cairosvg.svg2png(bytestring=raw, output_width=960, background_color='white')
        im = Image.open(io.BytesIO(raw))
        im.seek(0)
        if im.width < 140 or im.height < 90 or im.width * im.height < 20000: return no()
        if im.mode in ('RGBA', 'LA', 'P'):
            im = im.convert('RGBA')
            bg = Image.new('RGB', im.size, (255, 255, 255)); bg.paste(im, mask=im.split()[3]); im = bg
        else:
            im = im.convert('RGB')
        im.thumbnail((480, 320), Image.LANCZOS)
        flat = len(im.getcolors(maxcolors=300) or []) > 0 and len(im.getcolors(maxcolors=300)) < 256   # drawings and diagrams
        buf = io.BytesIO()
        im.save(buf, 'JPEG', quality=72 if flat else 52, optimize=True, progressive=False, subsampling=0 if flat else 2)
        jpg = buf.getvalue()
        if len(jpg) > 110000 or len(jpg) < 1500: return no()
        if has_people(jpg): return no()
        if DROP_LINEART and is_lineart(jpg): return no()
    except Exception:
        return no()
    open(cj, 'wb').write(jpg)
    return jpg

# Drawings of molecules, chemical reactions and spectra, which are no use on a small screen. Judged by the picture's file name and its caption.
IMG_MOLECULE = re.compile(r'(skeletal|structural[-_ ]?formula|3d[-_ ]?(?:balls|vdw|sticks|spacefill|ball)|ball[-_ ]and[-_ ]stick|space[-_ ]?filling|spacefill|lewis|stereo(?:chem|isomer)|chembox|molecul|chiral|enantiomer|isomer|resonance|mechanism|reaction[-_ ]|orbital|nmr|spectrum|spectra|crystal[-_ ]structure|unit[-_ ]cell|wedge|haworth|fischer[-_ ]proj|newman|chair[-_ ]conformation|conformer|pubchem|chemspider|kekule|[-_ ]formula\b)', re.I)
IMG_TOKEN = re.compile('(\\d+)')

# Titles that are not English. Wikis like Appropedia keep translations as subpages ending in a language code, like Page/es.
LANG_CODES = set("aa ab af ak am an ar as av ay az ba be bg bh bi bm bn bo br bs ca ce ch co cr cs cu cv cy da de dv dz ee el eo es et eu fa ff fi fj fo fr fy ga gd gl gn gu gv ha he hi ho hr ht hu hy hz ia id ie ig ii ik io is it iu ja jv ka kg ki kj kk kl km kn ko kr ks ku kv kw ky la lb lg li ln lo lt lu lv mg mh mi mk ml mn mr ms mt my na nb nd ne ng nl nn no nr nv ny oc oj om or os pa pi pl ps pt qu rm rn ro ru rw sa sc sd se sg si sk sl sm sn so sq sr ss st su sv sw ta te tg th ti tk tl tn to tr ts tt tw ty ug uk ur uz ve vi vo wa wo xh yi yo za zh zu".split())
SPANISH = set("el los las del que una para por con como mas pero muy este esta sobre entre tambien agua suelo cultivo cultivos de".split())
def not_english(title):
    t = title.strip()
    m = re.search(r'/([a-z]{2,3})(?:-[a-z]+)?$', t, re.I)
    if m and m.group(1).lower() in LANG_CODES: return True
    if t.lower().startswith('asapedia/') and not t.lower().endswith('/en'): return True   # a Spanish farming wiki, English copies end in /en
    words = re.findall(r"[a-záéíóúñü]+", t.lower())
    plain = [w.replace('á','a').replace('é','e').replace('í','i').replace('ó','o').replace('ú','u') for w in words]
    if len(plain) >= 2 and sum(1 for w in plain if w in SPANISH) >= 2: return True
    if any(ord(c) > 0x24F and not (0x2000 <= ord(c) <= 0x2BFF) for c in t): return True   # Chinese, Arabic, Cyrillic and other scripts in the title
    return False

# Pages the pack can leave out, found from the info box on the page and from how the first paragraph starts.
# people: person markers in the info box, or a birth date in brackets. places: towns, countries and regions. entertainment: films,
# music, television and games. sports: clubs, seasons and competitions.
PERSON_CLASS = re.compile(r'class="[^"]*\b(?:birthplace|deathplace)\b')
BORN_OPEN = re.compile(r'\((?:born|b\.|died|d\.)\s|\(\s*(?:c\.\s*)?\d{3,4}\s*[–—-]\s*(?:c\.\s*)?\d{3,4}\s*\)|\((?:[A-Z][a-z]+ \d{1,2},? \d{3,4}|\d{1,2} [A-Z][a-z]+ \d{3,4}|c\. \d{3,4}|\d{3,4} BC)[^)]{0,60}?[–—-]')
PLACE_CLASS = re.compile(r'\bib-(?:settlement|uk-place|country|former-subdiv|station|district|us-county|us-state|ukcounty|french-commune|german-state|australian-place|nz-place|canada-place|india-place|irish-place|indian-jurisdiction|china-place|city|town|village|municipality|region|subdivision|province|capital|suburb|neighbourhood)\w*|\bgeography\b')
PLACE_OPEN = re.compile(r'\bis (?:a|an|the) (?:[A-Za-z-]+ ){0,6}(?:city|town|village|commune|municipality|hamlet|suburb|district|county|province|settlement|census-designated place|unincorporated community|neighbourhood|borough|parish|country|state|prefecture|region|island nation|territory)\b (?:in|of|on|near|within|located|that|which)|\bis the name of (?:two|three|four|several|many) (?:counties|towns|cities|places)')
ENT_CLASS = re.compile(r'\bib-tv\w*|\bhaudio\b|\bib-film|\bib-album|\bib-single|\bib-video-game|\bib-book|\bib-comic|\bib-musical|\bib-software|\bib-radio')
ENT_LABELS = re.compile(r'Directed by|Starring|Screenplay by|Produced by|Cinematography|Songwriter|Producer\(s\)|Label\(s\)|Original network|No\. of seasons|Narrated by|Developer\(s\)|Publisher\(s\)|Platform\(s\)|Composer\(s\)|Recorded|Original release|Years active|Genres?\b|Labels?\b')
SPORT_CLASS = re.compile(r'\bfootball\b|\bvcalendar\b|\bib-(?:sport|nfl|nba|mlb|nhl|cricket|rugby|tennis|golf|olympic|boxing|wrestling|baseball|basketball|ice-hockey|football|f1|motorsport|athlet|cycling|racing|season|league|club|team)\w*')
SPORT_LABELS = re.compile(r'\bGround\b|\bHead coach\b|\bLeague\b|\bManager\b|\bCoach\b|\bCapacity\b')
INFOBOX = re.compile(r'<table[^>]*class="(infobox[^"]*)"[^>]*>', re.I)
def classify(raw):
    m = INFOBOX.search(raw)
    cls = m.group(1) if m else ''
    box = raw[m.start():m.start() + 15000] if m else ''
    labels = re.sub(r'<[^>]+>', ' ', box[:9000]) if box else ''
    if cls and ('biography' in cls or PERSON_CLASS.search(box)): return 'people'
    if cls and PLACE_CLASS.search(cls): return 'places'
    if cls and SPORT_CLASS.search(cls): return 'sports'
    if cls and ENT_CLASS.search(cls): return 'entertainment'
    if box and ('vevent' in cls or 'hproduct' in cls.lower() or 'vcard' in cls) and ENT_LABELS.search(labels): return 'entertainment'
    if box and SPORT_LABELS.search(labels) and re.search(r'\bFounded\b|\bSeason\b|\bFull name\b', labels): return 'sports'
    for p in re.finditer(r'<p[ >].*?</p>', raw[:200000], re.S):
        t = re.sub(r'<[^>]+>', '', p.group(0))
        t = re.sub(r'\s+', ' ', t).strip()
        if len(t) >= 60:
            if BORN_OPEN.search(t[:260]): return 'people'
            if PLACE_OPEN.search(t[:260]): return 'places'
            break
    return None

# Wikipedia keeps its filing pages in the same file as the articles: Category:, Portal:, Template:, Help: and so on.
NAMESPACE_PAGE = re.compile(r'^(?:Category|Template|Wikipedia|Help|Portal|File|Draft|MediaWiki|Module|Talk|User|Book|TimedText|Special|Media|Project|Image)(?: talk)?:', re.I)

# A piece of a very large wiki: only titles whose first letter is in LETTERS, or whose first character is not a letter when LETTERS is OTHER.
def in_letters(title, letters):
    t = title.strip()
    c = t[:1].upper() if t else ''
    if letters == 'OTHER': return not (c.isascii() and c.isalpha())
    return c in letters

def main():
    args = sys.argv[1:]
    limit = None; name = None; fulltext = True; weight = 100; ocr = False; images = False; max_img = 40000; img_mb = 900
    if '--images' in args: args.remove('--images'); images = True
    images_for = None
    if '--images-for' in args: i = args.index('--images-for'); images_for = args[i + 1]; del args[i:i + 2]
    drop_molecules = '--no-molecules' in args
    if drop_molecules:
        args.remove('--no-molecules')
        global DROP_LINEART
        DROP_LINEART = True
    per_article = 4
    auto_pics = False
    if '--per-article' in args:
        i = args.index('--per-article')
        if args[i + 1] == 'auto': auto_pics = True   # 1 picture for a short page up to 4 for a long one
        else: per_article = int(args[i + 1])
        del args[i:i + 2]
    pic_limit = [per_article]
    if '--max-images' in args: i = args.index('--max-images'); max_img = int(args[i + 1]); del args[i:i + 2]
    if '--image-mb' in args: i = args.index('--image-mb'); img_mb = int(args[i + 1]); del args[i:i + 2]
    letters = None
    if '--letters' in args: i = args.index('--letters'); letters = args[i + 1].upper(); del args[i:i + 2]
    leave_out = set()
    for flag, cat in (('--no-people', 'people'), ('--no-places', 'places'), ('--no-entertainment', 'entertainment'), ('--no-sports', 'sports')):
        if flag in args: args.remove(flag); leave_out.add(cat)
    skipped = {}
    articles_only = '--articles-only' in args
    if articles_only: args.remove('--articles-only')
    english_only = '--english-only' in args
    if english_only: args.remove('--english-only')
    if '--ocr' in args: args.remove('--ocr'); ocr = True
    if '--weight' in args: i = args.index('--weight'); weight = int(args[i + 1]); del args[i:i + 2]
    if '--limit' in args: i = args.index('--limit'); limit = int(args[i + 1]); del args[i:i + 2]
    if '--name' in args: i = args.index('--name'); name = args[i + 1]; del args[i:i + 2]
    if '--no-fulltext' in args: args.remove('--no-fulltext'); fulltext = False
    if len(args) != 2: sys.exit(__doc__)
    zim, out = args
    z = Archive(zim)
    os.makedirs(out, exist_ok=True)
    n_total = z.entry_count
    print('entries in the file:', n_total, flush=True)
    path2title = {}
    arts = []
    pdfs = []
    redirects = []
    for i in range(n_total):
        e = z.get_entry_by_id(i)
        if e.is_redirect:
            if letters is not None and not in_letters(e.title or e.path, letters): continue
            if articles_only and NAMESPACE_PAGE.match(e.title or e.path): continue
            redirects.append((e.title or e.path, e.get_redirect_entry().path))
            continue
        item = e.get_item()
        if item.mimetype.startswith('text/html'):
            if JUNK_PATH.search(e.path): continue
            if english_only and not_english(e.title or e.path): continue
            if articles_only and NAMESPACE_PAGE.match(e.title or e.path): continue
            if letters is not None and not in_letters(e.title or e.path, letters): continue
            arts.append(e.path)
            path2title[e.path] = e.title or e.path
            if limit and len(arts) >= limit: break
        elif item.mimetype == 'application/pdf':
            pdfs.append(e.path)
    def resolve(href):
        h = urllib.parse.unquote(href.split('#')[0])
        if not h or '://' in h or h.startswith(('mailto:', 'javascript:')): return None
        h = re.sub(r'^(\.\./|\./)+', '', h)
        for cand in (h, h.lstrip('/'), 'A/' + h):
            if cand in path2title: return path2title[cand]
        return None
    recs = []        # sorted title records: key, key offset, off, clen, rlen
    docs = []        # article records in write order: off, key offset, clen, rlen
    pos = {}
    postings = {}    # stemmed word -> array of (doc << 8 | tf)
    doclen = array('I')
    state = {'fileno': 0}
    dat = open(os.path.join(out, 'articles.000'), 'wb')
    st = open(os.path.join(out, 'titles.str'), 'wb')
    holder = {'dat': dat}

    def emit(title, text):
        text = '# ' + title + '\n\n' + (text if not text.lstrip().startswith('# ') else text.split('\n', 1)[1])
        b = text.encode('utf-8')
        comp = zlib.compressobj(9, zlib.DEFLATED, -15)
        cb = comp.compress(b) + comp.flush()
        if holder['dat'].tell() + len(cb) > FILE_MAX:
            holder['dat'].close(); state['fileno'] += 1
            holder['dat'] = open(os.path.join(out, 'articles.%03d' % state['fileno']), 'wb')
        off = holder['dat'].tell() | (state['fileno'] << 48)
        holder['dat'].write(cb)
        key = key_of(title)
        koff = st.tell()
        st.write(key.encode('ascii', 'replace') + b'\0' + title.encode('utf-8')[:200] + b'\0')
        recs.append((key, koff, off, len(cb), len(b)))
        doc = len(docs)
        docs.append((off, koff, len(cb), len(b)))
        if fulltext:
            body = re.sub(r'\[\[[^|\]]*\|([^\]]*)\]\]', r'\1', text)[:INDEX_CHARS]
            counts = {}
            for w in tokens(body): counts[w] = counts.get(w, 0) + 1
            for w in tokens(title): counts[w] = counts.get(w, 0) + 6   # words in the title count more
            doclen.append(sum(counts.values()))
            for w, c in counts.items():
                postings.setdefault(w, array('I')).append((doc << 8) | min(c, 255))
        return off, koff, len(cb), len(b)

    img_dat = open(os.path.join(out, 'images.dat'), 'wb') if images else None
    img_recs = []        # offset, length of each stored picture
    img_ids = {}         # hash of the final picture -> id, so a picture used in many articles is stored once
    def article_images(p, text, apath):
        # swaps the picture markers for links to the pictures that pass the filters, at most 4 per article
        used = [0]
        def sub(m):
            if used[0] >= pic_limit[0] or len(img_recs) >= max_img or img_dat.tell() > img_mb * 1024 * 1024: return ''
            src, alt = p.images[int(m.group(1))]
            if drop_molecules and (IMG_MOLECULE.search(os.path.basename(src.split('?')[0])) or IMG_MOLECULE.search(alt or '')): return ''
            ipath = re.sub(r'^/', '', urllib.parse.unquote(urllib.parse.urljoin('/' + apath, src.split('?')[0])))
            try:
                raw = bytes(z.get_entry_by_path(ipath).get_item().content)
            except Exception:
                return ''
            jpg = prep_image(raw, os.path.basename(ipath))
            if not jpg: return ''
            import hashlib
            hh = hashlib.sha1(jpg).hexdigest()
            if hh not in img_ids:
                img_ids[hh] = len(img_recs)
                img_recs.append((img_dat.tell(), len(jpg)))
                img_dat.write(jpg)
            used[0] += 1
            label = re.sub(r'\s+', ' ', re.sub(r'\.(jpe?g|png|gif|webp|svg)$', '', alt.replace('_', ' '), flags=re.I)).strip()[:60] or 'Picture'
            return '\n[[img:%d|Picture: %s]]\n' % (img_ids[hh], label)
        return IMG_TOKEN.sub(sub, text)

    for k, path in enumerate(arts):
        e = z.get_entry_by_path(path)
        raw = bytes(e.get_item().content).decode('utf-8', 'replace')
        if leave_out:
            cat = classify(raw)
            if cat in leave_out: skipped[cat] = skipped.get(cat, 0) + 1; continue
        p = Html2Text(resolve)
        p.feed(raw)
        title = e.title or path
        if re.search(r'\.(htm|html)$', title, re.I) or re.match(r'^(www\.)?[\w-]+\.[a-z]{2,}/', title):   # a page whose title is its address
            import html as _html
            mt = re.search(r'<title[^>]*>(.*?)</title>', raw, re.I | re.S)
            t2 = _html.unescape(re.sub(r'\s+', ' ', mt.group(1))).strip() if mt else ''
            NAV = re.compile(r'^(back to|home|contents|table of contents|next|previous|top of|go to|\[|return to|\|)', re.I)
            if len(t2) < 4 or NAV.match(t2):
                t2 = next((ln.strip('# ').strip() for ln in clean(''.join(p.out)).split('\n') if len(ln.strip('# ').strip()) > 7 and not NAV.match(ln.strip('# ').strip())), '')
            pm = re.search(r'/([A-Z0-9_]{3,})/([A-Z0-9_]{3,})/[^/]+$', path)   # CD3WD style folders: section and book code
            if t2 and pm: t2 = '%s (%s %s)' % (t2[:80], pm.group(1).title(), pm.group(2))
            if t2: title = t2[:100]
        body_text = ''.join(p.out)
        want_pics = images and (images_for is None or (images_for == 'biota' and re.search(r'<table[^>]*class="infobox[^"]*\bbiota\b', raw[:300000])))
        if auto_pics: n_ = len(body_text); pic_limit[0] = 1 if n_ < 3000 else 2 if n_ < 12000 else 3 if n_ < 40000 else 4
        body_text = article_images(p, body_text, path) if want_pics else IMG_TOKEN.sub('', body_text)
        text = clean(body_text)[:MAX_ARTICLE]
        off, koff, cl, rl = emit(title, text)
        pos[path] = (off, koff, cl, rl)
        if k % 500 == 0: print(f'\r{k}/{len(arts)} articles', end='', flush=True)
    npdf = 0
    meta = {}   # zimgit collections list their documents in database.js: title, description, author, file
    try:
        import ast
        js = bytes(z.get_entry_by_path('database.js').get_item().content).decode('utf-8', 'replace')
        for d in ast.literal_eval(js[js.index('['):js.rindex(']') + 1]):
            for fp in d.get('fp', []): meta[fp] = d
    except Exception:
        pass
    for path in pdfs:
        e = z.get_entry_by_path(path)
        md = meta.get(os.path.basename(path), {})
        title = md.get('ti')
        data = bytes(e.get_item().content)
        if not title:   # no list of titles in this file, ask the PDF itself, then fall back to its file name
            title = pdf_title(data)
        if not title:
            title = os.path.basename(path).rsplit('.pdf', 1)[0].replace('_', ' ').replace('-', ' ').strip()
        title = title.strip()
        intro = ''
        if md.get('aut') or md.get('dsc'): intro = ('From ' + md['aut'] + '. ' if md.get('aut') else '') + (md.get('dsc') or '') + '\n\n'
        try:
            text = reflow(pdf_text(data))
            if len(text) < 1500 and ocr:
                import subprocess as _sp, tempfile as _tf
                with _tf.NamedTemporaryFile(suffix='.pdf') as _t:
                    _t.write(data); _t.flush()
                    _pi = _sp.run(['pdfinfo', _t.name], capture_output=True).stdout.decode()
                _m = re.search(r'Pages:\s+(\d+)', _pi)
                if _m and int(_m.group(1)) >= 3:   # one and two page flyers are not worth reading
                    print(f'\nscan, reading {title} with text recognition', flush=True)
                    text = reflow(ocr_pdf(data))
        except Exception as ex:
            print('\npdf failed', path, ex); continue
        if len(text) < 1500:
            print('\nno text in', path, '(a scan?)'); continue
        parts = split_parts(intro + text)
        for i, part in enumerate(parts):
            t = title if len(parts) == 1 else f'{title} (part {i + 1} of {len(parts)})'
            nav = []
            if i > 0: nav.append('[[%s|Previous part]]' % key_of(f'{title} (part {i} of {len(parts)})'))
            if i + 1 < len(parts): nav.append('[[%s|Next part]]' % key_of(f'{title} (part {i + 2} of {len(parts)})'))
            body = ('\n'.join(nav) + '\n\n' if nav else '') + part[:MAX_ARTICLE] + ('\n\n' + '\n'.join(nav) if nav else '')
            emit(t, body)
        npdf += 1
        print(f'\npdf {title}: {len(parts)} parts', flush=True)
    holder['dat'].close()
    if images:
        img_dat.close()
        with open(os.path.join(out, 'images.idx'), 'wb') as ii:
            for off, ln in img_recs: ii.write(struct.pack('<QI', off, ln))
        print(f'\n{len(img_recs)} pictures kept, {os.path.getsize(os.path.join(out, "images.dat")) // 1024 // 1024} MB')
    for title, target in redirects:
        if target in pos:
            off, _, cl, rl = pos[target]
            key = key_of(title)
            koff = st.tell()
            st.write(key.encode('ascii', 'replace') + b'\0' + title.encode('utf-8')[:200] + b'\0')
            recs.append((key, koff, off, cl, rl))
    st.close()
    recs.sort(key=lambda r: r[0].encode())
    with open(os.path.join(out, 'titles.idx'), 'wb') as idx:
        for key, koff, off, cl, rl in recs: idx.write(struct.pack('<QIII', off, koff, cl, rl))
    with open(os.path.join(out, 'docs.idx'), 'wb') as idx:
        for off, koff, cl, rl in docs: idx.write(struct.pack('<QIII', off, koff, cl, rl))
    nwords = 0
    if fulltext and docs:
        N = len(docs)
        avg = sum(doclen) / N
        words = sorted(postings)
        with open(os.path.join(out, 'words.str'), 'wb') as ws, open(os.path.join(out, 'words.idx'), 'wb') as wi, open(os.path.join(out, 'post.dat'), 'wb') as pd:
            for w in words:
                arr = postings[w]
                df = len(arr)
                idf = math.log(1 + (N - df + 0.5) / (df + 0.5))
                scored = []
                for v in arr:
                    d, tf = v >> 8, v & 255
                    tfw = tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * doclen[d] / avg))
                    scored.append((min(255, max(1, int(tfw * idf * 8))), d))
                scored.sort(reverse=True)
                scored = scored[:MAX_POSTINGS]
                wi.write(struct.pack('<III', ws.tell(), pd.tell(), len(scored)))
                ws.write(w.encode() + b'\0')
                pd.write(b''.join(struct.pack('<I', (d << 8) | q) for q, d in scored))
        nwords = len(words)
    with open(os.path.join(out, 'pack.txt'), 'w') as f:
        f.write('a11pack 2\ntitle=%s\ncount=%d\ndocs=%d\nwords=%d\nfiles=%d\nweight=%d\nimages=%d\nsource=%s\n' % (name or os.path.basename(zim), len(recs), len(docs), nwords, state['fileno'] + 1, weight, len(img_recs), os.path.basename(zim)))
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    if leave_out: print('\nleft out', skipped)
    print(f'\n{len(docs)} articles ({npdf} PDFs), {len(recs)} titles, {nwords} words, {size // 1024 // 1024} MB in {out}')

if __name__ == '__main__':
    main()
