#!/usr/bin/env python3
# Usage: a11hub.py PACKS_FOLDER
# Builds the "topics" pack in PACKS_FOLDER: one page, Quick topics, with a line per first aid, medical and survival topic and links
# straight to the best articles in the other packs. Only links that exist in the packs on hand are written, so run it again after adding packs.
# A link between packs looks like [[wikimed-full>snakebite|WikiMed: Snakebite]], the part before > is the pack folder.
import sys, os, struct, zlib, importlib.util, unicodedata, re

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('a11pack', os.path.join(here, 'a11pack.py'))
a11 = importlib.util.module_from_spec(spec)
sys.modules['libzim'] = type(sys)('libzim')           # a11pack imports libzim at the top, this tool does not need it
sys.modules['libzim.reader'] = type(sys)('libzim.reader')
sys.modules['libzim.reader'].Archive = None
spec.loader.exec_module(a11)

SHORT = {'nhs-medicines': 'NHS', 'wikimed-full': 'WikiMed', 'military-medicine': 'Army', 'medical-library': 'Library', 'quick-guides': 'Quick guide',
         'libretexts-med': 'Textbook', 'water': 'Water', 'food-prep': 'Food', 'wikivet': 'Vet', 'appropedia': 'Appropedia',
         'trueprepper': 'Prepper', 'urban-prepper': 'Prepper', 'wikibooks': 'Wikibooks', 'knots': 'Knots', 'wiki-simple': 'Simple', 'wiki-chemistry': 'Chem'}

class Pack:
    def __init__(self, base, name):
        self.name = name
        d = os.path.join(base, name)
        self.ok = os.path.exists(os.path.join(d, 'pack.txt'))
        if not self.ok: return
        rd = lambda f: open(os.path.join(d, f), 'rb').read() if os.path.exists(os.path.join(d, f)) else b''
        self.tidx, self.tstr, self.didx = rd('titles.idx'), rd('titles.str'), rd('docs.idx')
        self.widx, self.wstr, self.post = rd('words.idx'), rd('words.str'), rd('post.dat')
        self.n = len(self.tidx) // 20
        self.nw = len(self.widx) // 12
        info = dict(l.strip().split('=', 1) for l in open(os.path.join(d, 'pack.txt')) if '=' in l)
        self.weight = int(info.get('weight', 100))

    def strs(self, koff):
        e = self.tstr.index(b'\0', koff)
        e2 = self.tstr.index(b'\0', e + 1)
        return self.tstr[koff:e].decode(), self.tstr[e + 1:e2].decode()

    def exact(self, key):
        lo, hi = 0, self.n
        while lo < hi:
            m = (lo + hi) // 2
            k = self.strs(struct.unpack('<QIII', self.tidx[m * 20:m * 20 + 20])[1])[0]
            if k < key: lo = m + 1
            else: hi = m
        if lo < self.n:
            k, t = self.strs(struct.unpack('<QIII', self.tidx[lo * 20:lo * 20 + 20])[1])
            if k == key: return k, t
        return None

    def word(self, w):
        lo, hi = 0, self.nw
        while lo < hi:
            m = (lo + hi) // 2
            so, po, cnt = struct.unpack('<III', self.widx[m * 12:m * 12 + 12])
            ww = self.wstr[so:self.wstr.index(b'\0', so)].decode()
            if ww == w: return po, cnt
            if ww < w: lo = m + 1
            else: hi = m
        return None

    def search(self, query, top=1):
        scores = {}
        hits = {}
        for w in a11.tokens(query):
            r = self.word(w)
            if not r: continue
            po, cnt = r
            for i in range(cnt):
                v = struct.unpack('<I', self.post[po + 4 * i:po + 4 * i + 4])[0]
                scores[v >> 8] = scores.get(v >> 8, 0) + (v & 255)
                hits[v >> 8] = hits.get(v >> 8, 0) + 1
        ranked = sorted(scores, key=lambda d: -(scores[d] + (hits[d] - 1) * 60))[:top]
        out = []
        for d in ranked:
            k, t = self.strs(struct.unpack('<QIII', self.didx[d * 20:d * 20 + 20])[1])
            out.append((k, t))
        return out

# section -> list of (topic, exact WikiMed keys, query, packs to ask)
MED = ['nhs-medicines', 'military-medicine', 'medical-library', 'quick-guides', 'libretexts-med']
SECTIONS = [
    ('Emergencies first', [
        ('CPR, no breathing', ['cardiopulmonary resuscitation'], 'cardiopulmonary resuscitation chest compressions', ['military-medicine', 'medical-library']),
        ('Choking', ['choking'], 'choking airway obstruction abdominal thrusts', ['military-medicine', 'medical-library']),
        ('Severe bleeding, tourniquet', ['bleeding', 'tourniquet', 'hemorrhage'], 'bleeding control pressure tourniquet', ['military-medicine', 'medical-library']),
        ('Severe allergic reaction', ['anaphylaxis', 'epinephrine (medication)'], 'anaphylaxis adrenaline allergic reaction', ['nhs-medicines', 'military-medicine']),
        ('Heart attack', ['myocardial infarction'], 'heart attack chest pain', ['quick-guides', 'military-medicine']),
        ('Stroke', ['stroke'], 'stroke face arm speech', ['quick-guides', 'libretexts-med']),
        ('Seizure', ['epileptic seizure', 'seizure'], 'seizure convulsion first aid', ['nhs-medicines', 'military-medicine']),
        ('Shock', ['shock (circulatory)', 'hypovolemic shock'], 'shock treatment blood loss', ['military-medicine', 'medical-library']),
        ('Drowning', ['drowning'], 'drowning rescue breathing', ['military-medicine']),
        ('Poisoning, overdose', ['poisoning', 'drug overdose'], 'poisoning overdose antidote', ['nhs-medicines', 'military-medicine', 'medical-library']),
    ]),
    ('Injuries', [
        ('Wounds, cuts', ['wound', 'laceration'], 'wound care cleaning dressing stitches', ['military-medicine', 'medical-library']),
        ('Broken bones, splints', ['bone fracture', 'splint (medicine)'], 'fracture splint immobilize', ['military-medicine', 'medical-library']),
        ('Bandaging', ['bandage'], 'bandaging splinting', ['military-medicine']),
        ('Burns', ['burn', 'burns'], 'burns treatment cool water dressing', ['military-medicine', 'medical-library']),
        ('Sprains and strains', ['sprain'], 'sprain strain ankle', ['military-medicine']),
        ('Head injury, concussion', ['concussion', 'head injury'], 'head injury concussion', ['military-medicine', 'quick-guides']),
        ('Spine injury', ['spinal cord injury'], 'spine injury neck back immobilize', ['military-medicine']),
        ('Eye injury', ['eye injury'], 'eye injury chemical foreign body', ['military-medicine']),
        ('Dental pain, tooth abscess', ['toothache', 'dental abscess'], 'tooth abscess pain', ['military-medicine', 'medical-library']),
    ]),
    ('Heat, cold, water', [
        ('Hypothermia', ['hypothermia'], 'hypothermia rewarming', ['military-medicine', 'medical-library']),
        ('Frostbite', ['frostbite'], 'frostbite cold injury', ['military-medicine']),
        ('Heat stroke, heat illness', ['hyperthermia', 'heat stroke'], 'heat stroke heat exhaustion cooling', ['military-medicine', 'medical-library']),
        ('Dehydration, oral rehydration', ['dehydration', 'oral rehydration therapy'], 'dehydration oral rehydration solution', ['nhs-medicines', 'military-medicine']),
        ('Making water safe', ['water purification', 'boil-water advisory', 'portable water purification'], 'water purification boiling disinfection', ['water', 'military-medicine', 'appropedia']),
    ]),
    ('Infection and fever', [
        ('Fever', ['fever'], 'fever temperature children adults', ['nhs-medicines', 'quick-guides']),
        ('Sepsis, blood poisoning', ['sepsis'], 'sepsis infection signs', ['quick-guides', 'libretexts-med']),
        ('Infected wound, abscess', ['abscess', 'wound infection'], 'infected wound abscess drainage', ['military-medicine', 'medical-library']),
        ('Tetanus', ['tetanus'], 'tetanus wound vaccine', ['military-medicine']),
        ('Rabies, animal bites', ['rabies', 'animal bite'], 'rabies animal bite', ['military-medicine']),
        ('Antibiotics', ['antibiotic'], 'antibiotics infection', ['nhs-medicines', 'quick-guides']),
        ('Malaria', ['malaria'], 'malaria prevention treatment', ['military-medicine', 'medical-library']),
    ]),
    ('Stomach, breathing, skin', [
        ('Diarrhoea and vomiting', ['diarrhea', 'vomiting', 'gastroenteritis'], 'diarrhoea vomiting rehydration', ['nhs-medicines', 'military-medicine']),
        ('Food poisoning', ['foodborne illness'], 'food poisoning', ['nhs-medicines', 'food-prep']),
        ('Constipation', ['constipation'], 'constipation laxative', ['nhs-medicines']),
        ('Abdominal pain', ['abdominal pain', 'appendicitis'], 'abdominal pain appendicitis', ['quick-guides', 'military-medicine']),
        ('Asthma, wheeze', ['asthma'], 'asthma inhaler wheeze', ['nhs-medicines', 'quick-guides']),
        ('Pneumonia, chest infection', ['pneumonia'], 'pneumonia cough chest infection', ['quick-guides', 'military-medicine']),
        ('Rash, allergy', ['rash', 'allergy', 'urticaria'], 'rash itching allergy antihistamine', ['nhs-medicines']),
        ('Lice, scabies', ['scabies', 'head lice'], 'scabies lice treatment', ['nhs-medicines', 'military-medicine']),
    ]),
    ('Bites and stings', [
        ('Snake bite', ['snakebite', 'snake antivenom'], 'snake bite venom', ['military-medicine', 'medical-library']),
        ('Spider, scorpion, insect stings', ['spider bite', 'scorpion sting', 'insect bites and stings'], 'spider scorpion sting bite', ['military-medicine']),
        ('Ticks, Lyme disease', ['lyme disease', 'tick'], 'tick bite lyme', ['nhs-medicines', 'military-medicine']),
    ]),
    ('Women, babies, children', [
        ('Emergency childbirth', ['childbirth'], 'emergency childbirth delivery baby', ['military-medicine', 'medical-library', 'quick-guides']),
        ('Bleeding after birth', ['postpartum bleeding', 'postpartum hemorrhage'], 'postpartum hemorrhage', ['quick-guides', 'libretexts-med']),
        ('Fever and dosing in children', ['paracetamol'], 'children fever paracetamol ibuprofen dose', ['nhs-medicines']),
    ]),
    ('Common medicines', [
        ('Paracetamol (acetaminophen)', ['paracetamol'], 'paracetamol for adults', ['nhs-medicines']),
        ('Ibuprofen', ['ibuprofen'], 'ibuprofen for adults', ['nhs-medicines']),
        ('Aspirin', ['aspirin'], 'aspirin', ['nhs-medicines']),
        ('Amoxicillin', ['amoxicillin'], 'amoxicillin', ['nhs-medicines']),
        ('Doxycycline', ['doxycycline'], 'doxycycline', ['nhs-medicines']),
        ('Antihistamines', ['antihistamine'], 'antihistamines', ['nhs-medicines']),
        ('Loperamide for diarrhoea', ['loperamide'], 'loperamide', ['nhs-medicines']),
        ('Salbutamol inhaler', ['salbutamol'], 'salbutamol inhaler', ['nhs-medicines']),
        ('Hydrocortisone cream', ['hydrocortisone'], 'hydrocortisone skin', ['nhs-medicines']),
        ('Omeprazole, heartburn', ['omeprazole'], 'omeprazole', ['nhs-medicines']),
    ]),
    ('Food, shelter, tools', [
        ('Preserving food', ['food preservation'], 'preserving food drying canning smoking', ['food-prep', 'appropedia']),
        ('Knots', ['knot'], 'knots rope', ['knots']),
        ('Shelter and heating', [], 'shelter building heating insulation', ['appropedia', 'trueprepper']),
        ('Sanitation and toilets', ['sanitation'], 'sanitation latrine toilet waste', ['appropedia', 'military-medicine']),
        ('Animals: sick or injured', [], 'animal health disease treatment livestock', ['wikivet']),
    ]),
]

def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    base = sys.argv[1]
    packs = {n: Pack(base, n) for n in SHORT}
    packs = {n: p for n, p in packs.items() if p.ok}
    lines = ['Reference only, not medical advice. Each line below leads to articles in the packs on this card, strongest sources first.', 'Books marked Army or Library are partly scanned pages read by a computer, numbers and doses can be wrong, check them against a label.', '']
    found_topics = 0
    for section, topics in SECTIONS:
        lines += ['## ' + section, '']
        for topic, keys, query, qpacks in topics:
            links, seen = [], set()
            def add(pname, key, title):
                if (pname, key) in seen or len(links) >= 5: return
                seen.add((pname, key))
                t = re.sub(r'\s*-\s*NHS$', '', title).replace('\n', ' ')[:48]
                links.append('[[%s>%s|%s: %s]]' % (pname, key, SHORT[pname], t))
            for pname in ('nhs-medicines', 'wikimed-full', 'wiki-simple'):
                p = packs.get(pname)
                if p and pname == 'wikimed-full':
                    for k in keys:
                        r = p.exact(a11.key_of(k))
                        if r: add(pname, r[0], r[1]); break
            want = set(a11.tokens(query))
            for pname in qpacks:
                p = packs.get(pname)
                if not p or not p.nw: continue
                for k, t in p.search(query, 3):
                    # packs of single topic pages must share a word with the topic, book parts have titles that say little
                    if pname in ('nhs-medicines', 'quick-guides', 'libretexts-med') and not (want & set(a11.tokens(t))): continue
                    add(pname, k, t)
                    break
            if links:
                lines.append('- %s: %s' % (topic, ' '.join(links)))
                found_topics += 1
        lines.append('')
    text = '\n'.join(lines)
    out = os.path.join(base, 'topics')
    os.makedirs(out, exist_ok=True)
    title = 'Quick topics, first aid and survival'
    body = '# ' + title + '\n\n' + text
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    cb = comp.compress(body.encode()) + comp.flush()
    open(os.path.join(out, 'articles.000'), 'wb').write(cb)
    key = a11.key_of(title)
    st = key.encode() + b'\0' + title.encode() + b'\0'
    open(os.path.join(out, 'titles.str'), 'wb').write(st)
    rec = struct.pack('<QIII', 0, 0, len(cb), len(body.encode()))
    open(os.path.join(out, 'titles.idx'), 'wb').write(rec)
    open(os.path.join(out, 'docs.idx'), 'wb').write(rec)
    open(os.path.join(out, 'pack.txt'), 'w').write('a11pack 2\ntitle=Quick topics\ncount=1\ndocs=1\nwords=0\nfiles=1\nweight=100\nsource=a11hub\n')
    print('%d topics with links, %d packs used: %s' % (found_topics, len(packs), ', '.join(sorted(packs))))

if __name__ == '__main__':
    main()
