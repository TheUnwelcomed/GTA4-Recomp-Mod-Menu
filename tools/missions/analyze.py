"""For each IV mission script (decompiled), find how its main loop reaches
the mission-passed routine: the chain of guards (switch/case on a static,
if on a static) from main down to the call."""
import re, glob, sys, os, json
SRC = sys.argv[1]
FUNC = re.compile(r'^(void|int|float|unknown|bool|boolean|var) (main|sub_\d+)\((.*)\)\s*$')

def functions(text):
    lines = text.split('\n')
    out, cur, start = {}, None, 0
    for i, l in enumerate(lines):
        m = FUNC.match(l)
        if m:
            if cur:
                out[cur] = (start, lines[start:i])
            cur, start = m.group(2), i
    if cur:
        out[cur] = (start, lines[start:])
    return out

def guards_for_line(body, target):
    """Walk the function body up to line `target`, keeping a stack of open
    constructs; return the guards enclosing that line (outermost first)."""
    stack = []   # each: dict(kind, var, value, cond)
    pending = None
    switch_var = []
    for i, raw in enumerate(body):
        l = raw.strip()
        if i == target:
            out = []
            for st in stack:
                if st['kind'] in ('case', 'if', 'else', 'while', 'for', 'switch'):
                    out.append(st)
            # the current case label applies too (cases aren't braced)
            return out
        m = re.match(r'switch \((.*)\)$', l)
        if m:
            pending = dict(kind='switch', var=m.group(1), cases=[])
            continue
        m = re.match(r'case (-?\d+):$', l)
        if m and stack and stack[-1]['kind'] in ('switch', 'case'):
            # a new case inside the current switch: replace the case marker
            if stack[-1]['kind'] == 'case':
                stack.pop()
            sw = stack[-1]
            stack.append(dict(kind='case', var=sw['var'], value=int(m.group(1)), braces=0))
            continue
        m = re.match(r'(else if|if) \((.*)\)$', l)
        if m:
            pending = dict(kind='if', cond=m.group(2), elseif=m.group(1) == 'else if')
            continue
        if l == 'else':
            pending = dict(kind='else')
            continue
        m = re.match(r'(while|for) \((.*)\)$', l) or re.match(r'(for) ?\((.*)\)$', l)
        if m:
            pending = dict(kind='while', cond=m.group(2))
            continue
        if l == '{':
            stack.append(pending or dict(kind='block'))
            pending = None
            continue
        if l == '}' or l.startswith('}'):
            # close: pop a case marker first (cases end with the switch)
            while stack and stack[-1]['kind'] == 'case':
                stack.pop()
            if stack:
                stack.pop()
            continue
        if l.startswith('default:') and stack and stack[-1]['kind'] in ('switch', 'case'):
            if stack[-1]['kind'] == 'case':
                stack.pop()
            stack.append(dict(kind='case', var=stack[-1]['var'], value='default', braces=0))
            continue
        if not l.startswith('if') and not l.startswith('else'):
            pending = None if l not in ('',) else pending
    return None

def callers(funcs, name):
    out = []
    pat = re.compile(r'\b%s\(' % re.escape(name))
    for fn, (start, body) in funcs.items():
        for i, l in enumerate(body):
            if i == 0 and fn == name:
                continue
            if pat.search(l) and not FUNC.match(l):
                out.append((fn, i))
    return out

def analyze(path, marker='TRIGGER_MISSION_COMPLETE_AUDIO'):
    text = open(path, errors='replace').read()
    funcs = functions(text)
    passes = [fn for fn, (s, b) in funcs.items() if any(marker in l for l in b)]
    results = []
    for p in passes:
        # walk up to main, collecting the guard chain (BFS, first path wins)
        frontier = [(p, [])]
        seen = set()
        while frontier:
            fn, chain = frontier.pop(0)
            if fn in seen:
                continue
            seen.add(fn)
            for caller, line in callers(funcs, fn):
                g = guards_for_line(funcs[caller][1], line)
                link = dict(fn=caller, call=fn, guards=g)
                if caller == 'main':
                    results.append([link] + chain)
                else:
                    frontier.append((caller, [link] + chain))
    return passes, results

def describe(g):
    if g['kind'] == 'case':
        return 'case %s=%s' % (g['var'], g['value'])
    if g['kind'] in ('if', 'while'):
        return '%s(%s)' % (g['kind'], g['cond'][:60])
    return g['kind']

if __name__ == '__main__':
    mission = re.compile(r'.*\d[a-z]?$')
    names = sorted(os.path.basename(f)[:-4] for f in glob.glob(SRC + '/*.txt') if mission.match(os.path.basename(f)[:-4]))
    for n in names:
        passes, results = analyze(SRC + '/' + n + '.txt')
        if not passes:
            print('%-16s no pass marker' % n)
            continue
        for r in results[:3]:
            print('%-16s %s' % (n, ' -> '.join('%s[%s]' % (link['fn'], ', '.join(describe(g) for g in (link['guards'] or []))) + ' calls ' + link['call'] for link in r)))
        if not results:
            print('%-16s pass %s: no path from main' % (n, passes))
