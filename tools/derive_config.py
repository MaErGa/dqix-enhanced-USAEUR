#!/usr/bin/env python3
"""Derive the code configuration of another version of the game (config/eur) from the USA one.

    python3 tools/derive_config.py <extract/usa> <extract/other> <name>     e.g. build/extract_usa build/extract_eur eur

The USA configuration (the dqix-decomp project's symbols and relocations, plus hle.txt, patches.txt,
tags.txt, interpret.txt) lists every function by address. Another version of the game is the same
program compiled again: the same functions, a few bytes apart. This finds each USA function in the
other version's code by its instructions (branch offsets and constant addresses set aside), moves
every address of the configuration to match, and checks the instructions the patches and tags touch.
The result goes to config/<name>/ and is kept in the repository (docs/TECHNICAL.md).
Needs PyYAML. Both extract folders come from tools/extract_rom.py.
"""
import bisect, os, re, struct, sys
import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFGU = os.path.join(ROOT, 'config', 'usa', 'arm9') + os.sep
SYM = re.compile(r"^(\S+) kind:function\((arm|thumb)(?:,size=0x([0-9a-f]+))?[^)]*\) addr:0x([0-9a-f]+)")


def load_syms(p):
    out = []
    for l in open(p):
        m = SYM.match(l)
        if m:
            out.append((m.group(1), m.group(2), int(m.group(3) or '0', 16), int(m.group(4), 16)))
    return sorted(out, key=lambda x: x[3])


def words(data, off, size, mode):
    """The instructions of a function with the parts that move between versions (branch offsets) set aside."""
    if mode == 'arm':
        return [w & 0xFF000000 if (w >> 25) & 7 == 5 else w for w in struct.unpack_from('<%dI' % (size // 4), data, off)]
    return [h & 0xF800 if (h & 0xF800) in (0xF000, 0xF800) else h for h in struct.unpack_from('<%dH' % (size // 2), data, off)]


def addr_like(w):
    return 0x01000000 <= w < 0x10000000


def modules(xu, xe):
    mods = {'main': (xu + '/arm9/arm9.bin', xe + '/arm9/arm9.bin', 0x02000000, 0x02000000, CFGU + 'symbols.txt')}
    for m in ('itcm', 'dtcm'):
        y = yaml.safe_load(open(xu + '/arm9/%s.yaml' % m))
        mods[m] = (xu + '/arm9/%s.bin' % m, xe + '/arm9/%s.bin' % m, y['base_address'], y['base_address'], CFGU + '%s/symbols.txt' % m)
    ovu = yaml.safe_load(open(xu + '/arm9_overlays/overlays.yaml'))['overlays']
    ove = {o['id']: o for o in yaml.safe_load(open(xe + '/arm9_overlays/overlays.yaml'))['overlays']}
    for o in ovu:
        n = 'ov%03d' % o['id']      # the overlays of another version may be loaded at another address
        mods[n] = (xu + '/arm9_overlays/%s.bin' % n, xe + '/arm9_overlays/%s.bin' % n, o['base_address'],
                   ove[o['id']]['base_address'], CFGU + 'overlays/%s/symbols.txt' % n)
    return mods


def pass1(du, de, base, base_e, syms, win=0x3000, thr=0.85):
    """Functions found by their instructions, searched near where the previous one was."""
    res, delta = {}, 0
    for name, mode, size, addr in syms:
        o0 = addr - base
        if size < 4 or o0 < 0 or o0 + size > len(du):
            continue
        step = 4 if mode == 'arm' else 2
        s = words(du, o0, size, mode); n = len(s)
        lo = max(0, o0 + delta - win); hi = min(len(de) - size, o0 + delta + win); lo -= lo % step
        best, bs = None, 0
        for o in range(lo, hi, step):
            if n > 3 and words(de, o, min(size, 8 if mode == 'arm' else 4), mode)[0] != s[0]:
                continue
            t = words(de, o, size, mode)
            sc = sum(1 for a, b in zip(s, t) if a == b) / n
            exp = o0 + delta
            if sc > bs or (sc == bs and best is not None and abs(o - exp) < abs(best - exp)):
                bs, best = sc, o
        if best is not None and bs >= thr:
            res[addr] = best + base_e; delta = best - o0
    return res


def pass2(du, de, base, base_e, syms, res):
    """The tiny functions that are mostly constants: between their matched neighbours, ignoring the constants."""
    mapped = sorted(res)
    for name, mode, size, addr in syms:
        if addr in res or size < 4 or addr - base + size > len(du):
            continue
        i = bisect.bisect(mapped, addr)
        exp = (res[mapped[i - 1]] - base_e + (addr - mapped[i - 1])) if i else addr - base
        fmt, step = ('I', 4) if mode == 'arm' else ('H', 2)
        s = list(struct.unpack_from('<%d%s' % (size // step, fmt), du, addr - base))
        best, bs = None, 0
        for o in range(max(0, exp - 0x200) & ~(step - 1), min(len(de) - size, exp + 0x200), step):
            t = list(struct.unpack_from('<%d%s' % (len(s), fmt), de, o))
            cmp = [(a, b) for a, b in zip(s, t) if not (mode == 'arm' and addr_like(a))]
            if not cmp or (mode == 'arm' and not all(addr_like(b) for a, b in zip(s, t) if addr_like(a))):
                continue
            sc = sum(1 for a, b in cmp if a == b) / len(cmp)
            if sc > bs or (sc == bs and best is not None and abs(o - exp) < abs(best - exp)):
                bs, best = sc, o
        if best is not None and bs >= 0.99:
            res[addr] = best + base_e; mapped = sorted(res)
    return res


class Map:
    def __init__(s, r):
        s.r, s.k = r, sorted(r)
        s.d = [r[a] - a for a in s.k]

    def addr(s, a):
        if a in s.r:
            return s.r[a]
        i = bisect.bisect_right(s.k, a) - 1
        return a + (s.d[i] if i >= 0 else 0)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    xu, xe, name = sys.argv[1:4]
    out = os.path.join(ROOT, 'config', name)
    mods = modules(xu, xe)
    M, total, missing = {}, 0, 0
    for n, (u, e, b, be, sp) in mods.items():
        if not os.path.exists(sp) or not os.path.exists(u):
            continue
        du, de, syms = open(u, 'rb').read(), open(e, 'rb').read(), load_syms(sp)
        r = pass2(du, de, b, be, syms, pass1(du, de, b, be, syms))
        M[n] = Map(r)
        total += sum(1 for s in syms if s[2] >= 4); missing += sum(1 for s in syms if s[2] >= 4 and s[3] not in r)
        print('%-6s %5d functions, %d without a match' % (n, len(syms), sum(1 for s in syms if s[2] >= 4 and s[3] not in r)), flush=True)
    print('matched %d of %d functions' % (total - missing, total))

    def paths(n, fn):
        if n == 'main': return CFGU + fn, os.path.join(out, 'arm9', fn)
        if n in ('itcm', 'dtcm'): return CFGU + n + '/' + fn, os.path.join(out, 'arm9', n, fn)
        return CFGU + 'overlays/%s/%s' % (n, fn), os.path.join(out, 'arm9', 'overlays', n, fn)

    for n, (u, e, b, be, sp) in mods.items():
        if n not in M:
            continue
        m = M[n]; syms = load_syms(sp)
        funcs = [s for s in syms if s[3] in m.r]
        size_new = {}
        for i, (nm, mode, size, addr) in enumerate(funcs[:-1]):       # a function that grew or shrank
            ug = funcs[i + 1][3] - addr; eg = m.r[funcs[i + 1][3]] - m.r[addr]
            if ug != eg:
                ns = eg if ug == size else size + (eg - ug)
                if ns >= 4 and ns % (4 if mode == 'arm' else 2) == 0:
                    size_new[addr] = ns
        su, dst = paths(n, 'symbols.txt')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        lines = []
        for line in open(su):
            mm = re.search(r' addr:0x([0-9a-f]+)', line)
            if mm:
                a = int(mm.group(1), 16)
                line = line.replace(' addr:0x%s' % mm.group(1), ' addr:0x%08x' % m.addr(a))
                if a in size_new:
                    line = re.sub(r'size=0x[0-9a-f]+', 'size=0x%x' % size_new[a], line, count=1)
            lines.append(line)
        open(dst, 'w').write(''.join(lines))
        ru, rd = paths(n, 'relocs.txt')
        if os.path.exists(ru):
            o = []
            for line in open(ru):
                mm = re.match(r'^from:0x([0-9a-f]+) (kind:\S+) to:0x([0-9a-f]+) module:(\S+)', line)
                if not mm:
                    o.append(line); continue
                tm = mm.group(4)
                o.append('from:0x%08x %s to:0x%08x module:%s\n' % (m.addr(int(mm.group(1), 16)), mm.group(2),
                         M[tm].addr(int(mm.group(3), 16)) if tm in M else int(mm.group(3), 16), tm))
            open(rd, 'w').write(''.join(o))

    bad = 0
    for f in ('tags.txt', 'patches.txt', 'hle.txt', 'interpret.txt'):
        src = os.path.join(ROOT, f)
        if not os.path.exists(src):
            continue
        o = []
        for line in open(src):
            mm = re.match(r'^(\w+) ([0-9a-f]{8})(.*)$', line)
            if mm and mm.group(1) in M:
                mod, a = mm.group(1), int(mm.group(2), 16); na = M[mod].addr(a)
                u, e, b, be, sp = mods[mod]
                wu = struct.unpack_from('<I', open(u, 'rb').read(), a - b)[0]
                we = struct.unpack_from('<I', open(e, 'rb').read(), na - be)[0]
                same = (wu & 0xFF000000) == (we & 0xFF000000) if (wu >> 25) & 7 == 5 else wu == we
                if not same:
                    bad += 1; print('CHECK %s %s %08x -> %08x: %08x vs %08x' % (f, mod, a, na, wu, we))
                line = '%s %08x%s\n' % (mod, na, mm.group(3))
            o.append(line)
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, f), 'w').write(''.join(o))
    print('written config/%s%s' % (name, '' if not bad else ' (%d instructions to check by hand)' % bad))


if __name__ == '__main__':
    main()
