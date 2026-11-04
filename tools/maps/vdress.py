"""The game's own props placed where a GTA V scene puts GTA V's (v1.7.42).

A V map pack (the Scarface mansion) places hundreds of the V game's own
props - ferns, bushes, wall lights, patio tables, a pool table - that the
pack does not carry and vimport.py leaves out. Given the V props' names
(ObjectList.ini from DurtyFree/gta-v-data-dumps: the entity's archetype hash
is joaat of its name) and a table of IV stand-ins, this writes each such
placement as a placement line of the IV model (mapset.py's `extra` lines:
model x y z qx qy qz qw, the set's coordinates, the true rotation), moved
and turned as vimport.py moved the pack (`--anchor`, `--turn`).

usage: python3 -I vdress.py <V stream dir> <ObjectList.ini> <subs.json> <out.txt>
         --anchor=vx,vy,vz:tx,ty,tz --turn=deg [--ymaps=a.ymap,b.ymap] [--thin=2]
  subs.json: {"prop_plant_fern_01b": "c_fern_md_ingame",
              "prop_bush_*": {"model": "ag_bushy1", "dz": 0.0, "every": 2}, ...}
    a V name or fnmatch pattern to an IV model (the game's own, or the set's),
    with an optional height nudge and "every": only every n-th such placement
    (V plants in drifts; the first pattern that matches wins)
  --ymaps: only these .ymap files (default: every one in the folder)
  --thin: the default "every" (1: all)
Prints what it placed and the V props with no stand-in, by count.
"""
import collections
import fnmatch
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import v_reader  # noqa: E402


def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=', 1)[0]: a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--') and '=' in a}
    vdir, names_path, subs_path, out = args[:4]
    names = {}
    for line in open(names_path):
        n = line.strip()
        if n and not n.startswith('#'):
            names[joaat(n)] = n.lower()
    subs = json.load(open(subs_path))
    rules = []
    for pat, sub in subs.items():
        if pat.startswith('_'):
            continue  # (notes)
        if isinstance(sub, str):
            sub = {'model': sub}
        rules.append((pat.lower(), sub))
    thin = int(opts.get('--thin', '1'))
    turn = math.radians(float(opts.get('--turn', '0')))
    anchor = target = None
    if '--anchor' in opts:
        a, t = opts['--anchor'].split(':')
        anchor = np.array([float(v) for v in a.split(',')])
        target = np.array([float(v) for v in t.split(',')])
    cs, sn = math.cos(turn), math.sin(turn)
    R = np.array([[cs, -sn, 0.0], [sn, cs, 0.0], [0.0, 0.0, 1.0]])
    q_turn = np.array([0.0, 0.0, math.sin(turn / 2), math.cos(turn / 2)])

    def place(pos, rot):
        """(vimport.py's) A V placement as the set's position and true rotation."""
        p = np.array(pos, dtype=float)
        q = np.array([-rot[0], -rot[1], -rot[2], rot[3]])  # (stored inverted, as GTA IV's .wpl rotations are)
        if anchor is not None:
            p = R @ (p - anchor) + target
        if turn:
            x1, y1, z1, w1 = q_turn
            x2, y2, z2, w2 = q
            q = np.array([w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
                          w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2])
        return p, q

    own = set()
    for f in os.listdir(vdir):
        if f.lower().endswith('.ytyp'):
            for a in v_reader.read_ytyp(os.path.join(vdir, f)):
                own.add(a['name'])
    ymaps = [f for f in sorted(os.listdir(vdir)) if f.lower().endswith('.ymap')]
    if '--ymaps' in opts:
        wanted = {w.lower() for w in opts['--ymaps'].split(',')}
        ymaps = [f for f in ymaps if f.lower() in wanted]
    placed, left, seen = collections.Counter(), collections.Counter(), collections.Counter()
    lines = ['# %s: the game\'s own props standing in for the V scene\'s (vdress.py) - model x y z qx qy qz qw '
             '(the set\'s coordinates, the true rotation)' % os.path.basename(out)]
    for f in ymaps:
        for e in v_reader.read_ymap(os.path.join(vdir, f)):
            if e['archetype'] in own:
                continue
            vname = names.get(e['archetype'])
            if vname is None:
                left['%08X' % e['archetype']] += 1
                continue
            rule = next((sub for pat, sub in rules if fnmatch.fnmatch(vname, pat)), None)
            if rule is None or not rule.get('model'):
                left[vname] += 1
                continue
            seen[vname] += 1
            every = int(rule.get('every', thin))
            if every > 1 and (seen[vname] - 1) % every:
                continue
            p, q = place(e['pos'], e['rot'])
            p[2] += float(rule.get('dz', 0.0))
            lines.append('%s %.3f %.3f %.3f %.6f %.6f %.6f %.6f   # %s' % (rule['model'], p[0], p[1], p[2], q[0], q[1],
                                                                           q[2], q[3], vname))
            placed[rule['model']] += 1
    with open(out, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    print('%d placements of %d of the game\'s models written to %s: %s' % (
        sum(placed.values()), len(placed), out, ', '.join('%s x%d' % kv for kv in placed.most_common())))
    print('no stand-in (%d placements): %s' % (sum(left.values()), ', '.join('%s x%d' % kv for kv in left.most_common(60))))


if __name__ == '__main__':
    main()
