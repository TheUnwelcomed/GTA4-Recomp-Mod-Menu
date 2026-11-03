"""The names of the game's own map models, from its .ide files (the mapsets
export zip, or a directory holding them): a map mod placing one of them
needs no file of its own for it - the set lists the piece by that name and
the game's model and collision stand there (mapset.py: cfg['stock']).

usage: python3 -I stocknames.py <export.zip | directory> <out.txt> [vehicles.txt]
  (vehicles.txt: the game's vehicle names, from the .ide files' cars
  sections, for the parked vehicles a mod's .wpl files generate)
"""
import os
import sys
import zipfile

SECTIONS = ('objs', 'tobj', 'tree', 'anim', 'tanm')
ENDS = ('end', 'mloend', 'roomend')


def names_in(text, out, vehicles=None):
    section = None
    for line in text.split('\n'):
        s = line.split('#')[0].strip()
        low = s.lower()
        if not s:
            continue
        if ',' not in s and ' ' not in s:
            if low in ENDS:
                section = None
            elif low in SECTIONS or low == 'cars' or section is None:
                section = low if low in SECTIONS or low == 'cars' else 'other'
            continue
        if section in SECTIONS:
            out.add(s.split(',')[0].strip().lower())
        elif section == 'cars' and vehicles is not None:
            vehicles.add(s.split(',')[0].strip().lower())


def main():
    src, out_path = sys.argv[1:3]
    vehicles_path = sys.argv[3] if len(sys.argv) > 3 else None
    names, vehicles = set(), set()
    count = 0
    if src.lower().endswith('.zip'):
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                if n.lower().endswith('.ide') and '/._' not in n:
                    names_in(z.read(n).decode('latin-1'), names, vehicles)
                    count += 1
    else:
        for root, _, files in os.walk(src):
            for f in files:
                if f.lower().endswith('.ide'):
                    names_in(open(os.path.join(root, f), encoding='latin-1').read(), names, vehicles)
                    count += 1
    with open(out_path, 'w') as f:
        f.write('# the game\'s own map model names (stocknames.py over %d .ide files): a map set\'s piece by one of '
                'these stands as the game\'s model\n' % count)
        for n in sorted(names):
            f.write(n + '\n')
    print('%d names from %d .ide files -> %s' % (len(names), count, out_path))
    if vehicles_path:
        with open(vehicles_path, 'w') as f:
            f.write('# the game\'s own vehicle names (stocknames.py, the .ide files\' cars sections)\n')
            for n in sorted(vehicles):
                f.write(n + '\n')
        print('%d vehicle names -> %s' % (len(vehicles), vehicles_path))


if __name__ == '__main__':
    main()
