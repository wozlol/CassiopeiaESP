#!/usr/bin/env python3
# Usage: a11fieldguide.py OUTFOLDER [--limit N]
# Builds the "field-guide" pack: edible and poisonous plants, mushrooms, insects and other arthropods, and venomous animals, from Wikipedia
# (text under CC BY-SA, pictures from Wikimedia Commons under their own free licenses). A handful of pictures per species, small JPEGs,
# never faces or people. Needs network, Pillow, pyobjc-framework-Vision. Everything downloaded is cached in ~/a11-staging/fg_cache.
import sys, os, re, json, time, struct, zlib, math, hashlib, urllib.request, urllib.parse, importlib.util
from array import array

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('a11pack', os.path.join(here, 'a11pack.py'))
a11 = importlib.util.module_from_spec(spec)
sys.modules['libzim'] = type(sys)('libzim')
sys.modules['libzim.reader'] = type(sys)('libzim.reader')
sys.modules['libzim.reader'].Archive = None
spec.loader.exec_module(a11)

UA = 'CassiopeiaESP-offline-reader/0.1 (personal hobby project)'
CACHE = os.path.expanduser('~/a11-staging/fg_cache')
API = 'https://en.wikipedia.org/w/api.php'

GROUPS = [
 ('Edible plants', '''Taraxacum officinale|Plantago major|Plantago lanceolata|Stellaria media|Urtica dioica|Oxalis acetosella|Portulaca oleracea|Typha|Trifolium pratense|Trifolium repens|Chenopodium album|Allium tricoccum|Allium ursinum|Allium vineale|Arctium lappa|Cichorium intybus|Malva neglecta|Quercus|Pinus|Rubus fruticosus|Sambucus canadensis|Fragaria vesca|Claytonia perfoliata|Nasturtium officinale|Rumex acetosella|Rumex crispus|Amaranthus retroflexus|Achillea millefolium|Mentha|Rosa canina|Rhus typhina|Impatiens capensis|Asclepias syriaca|Helianthus tuberosus|Apios americana|Smilax|Salicornia|Crithmum maritimum|Kelp|Porphyra|Palmaria palmata|Vaccinium myrtillus|Vaccinium macrocarpon|Prunus virginiana|Juglans nigra|Carya|Corylus americana|Castanea dentata|Diospyros virginiana|Asimina triloba|Morus rubra|Crataegus|Viola odorata|Capsella bursa-pastoris|Alliaria petiolata|Cirsium vulgare|Sonchus oleraceus|Tragopogon pratensis|Daucus carota|Foeniculum vulgare|Opuntia|Yucca|Cocos nucifera|Musa (genus)|Manihot esculenta|Moringa oleifera|Carica papaya|Artocarpus altilis|Colocasia esculenta|Ipomoea batatas|Lactuca serriola|Lamium purpureum|Barbarea vulgaris|Chaenomeles|Prunus spinosa|Elaeagnus umbellata|Ribes|Aronia|Lonicera caerulea|Phragmites|Nelumbo nucifera|Sagittaria latifolia|Stachys palustris|Hemerocallis fulva|Polygonatum|Matteuccia struthiopteris|Pteridium aquilinum|Equisetum arvense|Galium aparine|Glechoma hederacea|Rumex acetosa|Atriplex hortensis|Persicaria|Veronica|Bellis perennis|Tilia|Betula|Acer saccharum|Populus tremuloides|Salix|Ulmus rubra|Fagus grandifolia'''),
 ('Poisonous plants', '''Conium maculatum|Cicuta maculata|Digitalis purpurea|Atropa belladonna|Solanum dulcamara|Solanum nigrum|Phytolacca americana|Convallaria majalis|Toxicoscordion venenosum|Pastinaca sativa|Heracleum mantegazzianum|Toxicodendron radicans|Toxicodendron diversilobum|Toxicodendron vernix|Ricinus communis|Nerium oleander|Aconitum|Datura stramonium|Veratrum|Taxus baccata|Laburnum anagyroides|Euphorbia|Abrus precatorius|Aesculus|Dieffenbachia|Arum maculatum|Ranunculus acris|Lantana camara|Cicuta virosa|Hyoscyamus niger|Colchicum autumnale|Narcissus (plant)|Hydrangea|Rhododendron|Kalmia latifolia|Nicotiana tabacum|Brugmansia|Oenanthe crocata|Heracleum sphondylium|Ruta graveolens|Dictamnus albus|Wisteria|Cestrum|Manchineel|Jatropha curcas|Strychnos nux-vomica|Lupinus|Senecio jacobaea|Euonymus europaeus|Prunus laurocerasus|Vinca|Ilex aquifolium|Lathyrus odoratus|Phoradendron'''),
 ('Mushrooms', '''Amanita phalloides|Amanita virosa|Amanita bisporigera|Galerina marginata|Amanita muscaria|Amanita pantherina|Cortinarius rubellus|Gyromitra esculenta|Cantharellus cibarius|Morchella|Laetiporus|Calvatia gigantea|Boletus edulis|Hericium erinaceus|Pleurotus ostreatus|Armillaria mellea|Agaricus campestris|Coprinus comatus|Omphalotus illudens|Lepiota brunneoincarnata|Clitocybe dealbata|Hypholoma fasciculare|Grifola frondosa|Cantharellus cinnabarinus|Craterellus cornucopioides|Lycoperdon perlatum|Macrolepiota procera|Lactarius deliciosus|Russula|Psilocybe|Inocybe|Entoloma sinuatum|Chlorophyllum molybdites|Tricholoma equestre|Paxillus involutus|Cordyceps|Fomes fomentarius|Chaga|Ganoderma lucidum|Trametes versicolor|Fistulina hepatica|Sparassis crispa|Auricularia auricula-judae|Flammulina velutipes|Lentinula edodes'''),
 ('Insects and arachnids', '''Latrodectus|Loxosceles reclusa|Ixodes scapularis|Dermacentor variabilis|Amblyomma americanum|Tick|Mosquito|Aedes aegypti|Anopheles|Bed bug|Solenopsis invicta|Western honey bee|Yellowjacket|Hornet|Polistes|Bumblebee|Scorpion|Arizona bark scorpion|Triatominae|Head louse|Body louse|Flea|Trombiculidae|Sarcoptes scabiei|Blister beetle|Tabanidae|Black fly|Sandfly|Tsetse fly|Tarantula|Hobo spider|Centipede|Megalopyge opercularis|Africanized bee|Chigoe flea|Botfly|Dermatobia hominis|Cimex lectularius|Stable fly|Horse-fly|Biting midge|Paederus|Fire ant|Harvester ant|Velvet ant|Wolf spider|Brown widow spider|Funnel-web spider|Phoneutria|Deer fly|Lyme disease|Rocky Mountain spotted fever|Dengue fever|Malaria|Zika virus|West Nile virus|Chagas disease|Giant hornet|Asian giant hornet|Bullet ant|Pediculosis|Myiasis|Tungiasis|Cutaneous larva migrans|Leech|Hookworm'''),
 ('Venomous animals and bites', '''Rattlesnake|Agkistrodon contortrix|Agkistrodon piscivorus|Micrurus fulvius|Naja|Bothrops|Vipera berus|Bungarus|Black mamba|Gila monster|Portuguese man o' war|Stingray|Lionfish|Cone snail|Sea urchin|Stonefish|Box jellyfish|Sea snake|Taipan|Boomslang|Puff adder|Russell's viper|King cobra|Copperhead|Water moccasin|Coral snake|Mojave rattlesnake|Eastern diamondback rattlesnake|Timber rattlesnake|Sistrurus|Snakebite|Antivenom|Platypus|Slow loris|Blue-ringed octopus|Sea wasp|Irukandji jellyfish|Catfish|Weever|Venomous snake|Rabies|Dog bite|Spider bite|Insect bites and stings'''),
]

INTRO = '''# Field guide, read this first

Pictures and text for plants, mushrooms, insects and venomous animals. Reference only. It cannot replace an expert and a plant book for your region.

## Eating wild plants and mushrooms

- Never eat something you cannot identify with complete certainty. Many edible plants have deadly lookalikes, for example wild carrot and poison hemlock, or wild garlic and death camas.
- Check several features, not one: leaves, stem, flower, fruit, smell, where and when it grows. Use the pictures, then compare with a second source.
- The old "universal edibility test" of nibbling and waiting is not reliable and can poison you. It is not recommended.
- Do not eat wild mushrooms unless an expert in your area has identified them. Several deadly mushrooms look like harmless ones, and cooking does not make them safe.
- Some foods are only safe when cooked or prepared, for example cassava, taro, acorns and pokeweed shoots.
- Try a very small amount of a new food first, and keep a sample in case someone needs to be treated.
- If someone may have eaten something poisonous, get medical help or a poison centre at once if you can. See the poisoning pages in the medical packs.

## Bites and stings

Look for the picture of the animal first, then read its entry. Ticks, spiders, scorpions, snakes and stinging insects each have their own page. Pictures are shown with the links that start with Picture, press Enter on one.

## Sources and credit

Text comes from Wikipedia (CC BY-SA). Pictures come from Wikimedia Commons under their own free licenses. The file name is shown with each picture.
'''

def get(url, binary=False):
    os.makedirs(CACHE, exist_ok=True)
    h = os.path.join(CACHE, hashlib.sha1(url.encode()).hexdigest())
    if os.path.exists(h): return open(h, 'rb').read() if binary else open(h, encoding='utf-8').read()
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': UA})
            data = urllib.request.urlopen(req, timeout=60).read()
            open(h, 'wb').write(data)
            time.sleep(0.25)
            return data if binary else data.decode('utf-8')
        except Exception as e:
            time.sleep(2 + attempt * 3)
    return None

def api(**p):
    p['format'] = 'json'
    t = get(API + '?' + urllib.parse.urlencode(p))
    return json.loads(t) if t else None

def article(title):
    d = api(action='query', prop='extracts', explaintext=1, exsectionformat='wiki', titles=title, redirects=1)
    if not d: return None
    for pg in d['query']['pages'].values():
        if 'extract' in pg and len(pg['extract']) > 400: return pg['title'], pg['extract']
    return None

def pictures(title, want=4):
    out = []
    d = api(action='query', prop='pageimages', piprop='thumbnail', pithumbsize=640, titles=title, redirects=1)
    names = []
    if d:
        for pg in d['query']['pages'].values():
            if 'thumbnail' in pg: out.append((pg.get('pageimage', 'main'), pg['thumbnail']['source']))
    d = api(action='query', prop='images', imlimit=40, titles=title, redirects=1)
    if d:
        for pg in d['query']['pages'].values():
            for im in pg.get('images', []):
                n = im['title']
                if re.search(r'\.(jpe?g|png|webp)$', n, re.I) and not a11.IMG_SKIP.search(n) and not re.search(r'(map|range|distribution|locator|diagram of|chart)', n, re.I): names.append(n)
    for n in names[:14]:
        if len(out) >= want + 4: break
        d = api(action='query', prop='imageinfo', iiprop='url|size|mime', iiurlwidth=640, titles=n)
        if not d: continue
        for pg in d['query']['pages'].values():
            for ii in pg.get('imageinfo', []):
                if ii.get('mime', '').startswith('image/') and ii.get('thumburl') and ii.get('width', 0) >= 300:
                    out.append((n.replace('File:', ''), ii['thumburl']))
    return out

def main():
    args = sys.argv[1:]
    limit = None
    if '--limit' in args: i = args.index('--limit'); limit = int(args[i + 1]); del args[i:i + 2]
    if len(args) != 1: sys.exit(__doc__)
    out = args[0]
    os.makedirs(out, exist_ok=True)
    arts = [('Field guide, read this first', INTRO, [])]
    n = 0
    for group, names in GROUPS:
        for title in names.split('|'):
            if limit and n >= limit: break
            n += 1
            a = article(title)
            if not a: print('no article:', title, flush=True); continue
            real, text = a
            pics = []
            seen = set()
            for fname, url in pictures(title):
                raw = get(url, binary=True)
                if not raw: continue
                jpg = a11.prep_image(raw, fname)
                if not jpg: continue
                hh = hashlib.sha1(jpg).hexdigest()
                if hh in seen: continue
                seen.add(hh)
                pics.append((fname, jpg))
                if len(pics) >= 4: break
            arts.append((real, text, pics, group))
            print(f'{n} {group}: {real} ({len(text)} chars, {len(pics)} pictures)', flush=True)
        if limit and n >= limit: break
    write_pack(out, arts)

def write_pack(out, arts):
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
        if title != 'Field guide, read this first':
            lines.append(f'# {title}')
            lines.append(f'{group}. Text from Wikipedia, pictures from Wikimedia Commons.')
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
        else:
            full = text
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
        f.write('a11pack 2\ntitle=Field guide, plants and animals\ncount=%d\ndocs=%d\nwords=%d\nfiles=1\nweight=105\nimages=%d\nsource=wikipedia\n' % (len(recs), N, len(words), len(img_recs)))
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print(f'{N} articles, {len(img_recs)} pictures, {size // 1024} KB in {out}')

if __name__ == '__main__':
    main()
