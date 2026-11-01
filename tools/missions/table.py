"""Build the trainer's mission-pass table from the decompiled IV scripts.

For each mission script: how main reaches the mission's own "passed" code,
picking a path the trainer can trigger safely:
  key   - Rockstar's debug pass left in the retail scripts: the S key
          (IS_KEYBOARD_KEY_[JUST_]PRESSED(31)) calls the pass routine; the
          trainer answers yes to that question for the mission's thread.
  stage - the mission's stage variables (statics) set to the values whose
          case calls the pass routine (main's switch, and a stage function's
          own switch inside it), or its "passed" flag set.
The pass routine is the mission's wrapper that calls the shared flow code
(the one holding REGISTER_MISSION_PASSED) with its "passed" argument 1, or,
for missions without that flow (the random characters), the function that
plays the mission-complete audio.
A stage path also needs a signature to find the statics in memory: statics
the script sets once, at start, to constants (a vector, or several values)."""
import re, glob, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import analyze

def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF; h = (h + (h << 10)) & 0xFFFFFFFF; h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF; h ^= h >> 11; h = (h + (h << 15)) & 0xFFFFFFFF
    return h

CALL = re.compile(r'\b(sub_\d+)\(')

def reach(funcs, target):
    """Functions that call `target`, directly or not."""
    out, todo = set(), [target]
    while todo:
        t = todo.pop()
        for c, _ in analyze.callers(funcs, t):
            if c not in out:
                out.add(c)
                todo.append(c)
    return out

def pass_wrappers(funcs):
    flows = [fn for fn, (s, b) in funcs.items() if any('REGISTER_MISSION_PASSED' in l for l in b)]
    if not flows:
        return []
    towards = set(flows)
    for f in flows:
        towards |= reach(funcs, f)
    out = []
    for fn, (s, body) in funcs.items():
        consts = {}
        for l in body[1:]:
            m = re.match(r'\s*(iVar\d+) = (-?\d+);\s*$', l)
            if m:
                consts[m.group(1)] = int(m.group(2))
            m = re.match(r'\s*(sub_\d+)\( ([^,)]+)(,|\s*\))', l)
            if m and m.group(1) in towards:
                a = m.group(2).strip()
                if (a == '1' or consts.get(a) == 1) and fn not in flows:
                    out.append(fn)
    return sorted(set(out))

BENIGN_IF = [re.compile(p) for p in (
    r'^IS_PLAYER_PLAYING\( sub_\d+\(\) \)$',
    r'^NOT \(IS_CHAR_INJURED\( sub_\d+\(\) \)\)$',
    r'^NOT \(IS_CHAR_DEAD\( sub_\d+\(\) \)\)$',
    r'^NOT \(IS_WANTED_LEVEL_GREATER\( sub_\d+\(\), 0 \)\)$',  # (the trainer clears it first)
    # true soon enough on their own once the stage is set
    r'^TIMER[AB]\(\) >=? \d{1,4}$', r'^\(TIMER[AB]\(\) >=? \d{1,4}\)$',
    r'^HAS_CUTSCENE_FINISHED\(\)$',
)]
KEY = r'IS_KEYBOARD_KEY_(JUST_)?PRESSED\( 31 \)'

def inside_loop(link):
    """main's guards inside the loop holding the call (those before it held
    when the script entered its loop)."""
    g = link['guards'] or []
    last = max((i for i, x in enumerate(g) if x['kind'] == 'while'), default=-1)
    return g[last:] if last >= 0 else g

def classify(chain):
    """-> ('key', flags) / ('stage', [(static, value)]) / None"""
    writes, key, flags = [], False, []
    for depth, link in enumerate(chain):
        guards = inside_loop(link) if link['fn'] == 'main' else (link['guards'] or [])
        for g in guards:
            k = g['kind']
            if k == 'while':
                c = g['cond'].strip()
                if not (c == 'true' or re.match(r'^l_U\d+$', c) or any(b.match(c) for b in BENIGN_IF)):
                    return None
            elif k == 'switch':
                continue
            elif k == 'case':
                m = re.match(r'^l_U(\d+)$', g['var'])
                if not m or g['value'] == 'default':
                    return None
                writes.append((int(m.group(1)), int(g['value'])))
            elif k == 'if':
                c = g['cond'].strip()
                if any(b.match(c) for b in BENIGN_IF):
                    continue
                if re.fullmatch(KEY, c) or re.fullmatch(r'\(NOT l_U\d+\) AND \(%s\)' % KEY, c):
                    key = True
                    continue
                m = re.match(r'^l_U(\d+)$', c) or re.match(r'^l_U(\d+) == (\d+)$', c)
                if m:
                    writes.append((int(m.group(1)), int(m.group(2)) if m.lastindex > 1 else 1))
                    continue
                return None
            else:
                return None
    if key:
        return ('key', writes)  # (the flags on the way: a key path checks them, the trainer doesn't set them)
    return ('stage', writes) if writes else None

ASSIGN = re.compile(r'^\s*l_U(\d+) = (\{[^}]*\}|-?[\d.]+);\s*$')

def signature(path):
    """Statics set once, in main's prologue, to constants: [(index, value)],
    value an int or a float (a vector gives three). None when nothing
    distinctive enough."""
    text = open(path, errors='replace').read()
    funcs = analyze.functions(text)
    start, body = funcs['main']
    cands = []
    for l in body:
        s = l.strip()
        if s.startswith('while') or 'WAIT(' in s:
            break
        m = ASSIGN.match(l)
        if not m:
            continue
        idx, v = int(m.group(1)), m.group(2)
        if v.startswith('{'):
            parts = [p.strip() for p in v[1:-1].split(',')]
            if len(parts) != 3:
                continue
            try:
                vals = [float(p) for p in parts]
            except ValueError:
                continue  # (an expression, not a constant)
            cands.append((idx, [(idx + i, vals[i]) for i in range(3)]))
        else:
            cands.append((idx, [(idx, float(v) if '.' in v else int(v))]))
    scored = []
    for idx, slots in cands:
        uses = re.findall(r'(ref )?\bl_U%d\b(\[|\._fU\d+)?(\s*(?:[-+*/]?=(?!=)|\+\+|--))?' % idx, text)
        writes = [u for u in uses if u[0] or u[2]]
        if len(writes) != 1 or any(u[1] == '[' for u in uses):
            continue
        for i, v in slots:
            if isinstance(v, float) and v not in (0.0, 1.0, -1.0):
                sc = 2.0 if abs(v) >= 2 or (v * 1000) % 1 else 1.0
            elif v not in (0, 1, -1, 0.0, 1.0, -1.0):
                sc = 0.5
            else:
                sc = 0.1
            scored.append((sc, i, v))
    scored.sort(key=lambda t: (-t[0], t[1]))
    chosen = scored[:8]
    if sum(t[0] for t in chosen) >= 4.0 and len(chosen) >= 4:
        return sorted((i, v) for sc, i, v in chosen)
    return None

def best_path(src, n):
    path = src + '/' + n + '.txt'
    funcs = analyze.functions(open(path, errors='replace').read())
    # the mission's own pass routine: the one playing the mission-complete
    # audio; without one, the wrapper handing the shared flow code "passed"
    targets = [fn for fn, (st, b) in funcs.items() if any('TRIGGER_MISSION_COMPLETE_AUDIO' in l for l in b)]
    if not targets:
        targets = [w for w in pass_wrappers(funcs) if w != 'main']
    results = []
    for t in targets:
        results += chains_to(funcs, t)
    best = None
    for chain in results:
        c = classify(chain)
        if c is None:
            continue
        rank = (0 if c[0] == 'key' else 1, len(c[1]))
        if best is None or rank < best[0]:
            best = (rank, c)
    return best[1] if best else None

def chains_to(funcs, target):
    out, frontier, seen = [], [(target, [])], set()
    while frontier:
        fn, chain = frontier.pop(0)
        if fn in seen:
            continue
        seen.add(fn)
        for caller, line in analyze.callers(funcs, fn):
            link = dict(fn=caller, call=fn, guards=analyze.guards_for_line(funcs[caller][1], line))
            if caller == 'main':
                out.append([link] + chain)
            else:
                frontier.append((caller, [link] + chain))
    return out

def build(src):
    mission = re.compile(r'.*\d[a-z]?$')
    names = sorted(os.path.basename(f)[:-4] for f in glob.glob(src + '/*.txt') if mission.match(os.path.basename(f)[:-4]))
    rows, missing = [], []
    for n in names:
        best = best_path(src, n)
        sig = signature(src + '/' + n + '.txt')
        if best is None or (best[0] == 'stage' and sig is None):
            missing.append((n, 'no path' if best is None else 'no signature'))
            continue
        rows.append((joaat(n), n, best, sig))
    return rows, missing

if __name__ == '__main__':
    rows, missing = build(sys.argv[1])
    print('// %d missions: %d by the debug pass key, %d by their stage; none for %d: %s' % (
        len(rows), sum(r[2][0] == 'key' for r in rows), sum(r[2][0] == 'stage' for r in rows), len(missing),
        ' '.join('%s(%s)' % m for m in missing)))
    for key, n, (kind, writes), sig in sorted(rows, key=lambda r: r[1]):
        print('%-16s %-5s %-34s %s' % (n, kind, writes, sig))
