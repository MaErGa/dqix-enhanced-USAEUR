"""Recompile the DS ARM7 program (sound, saves, touch, RTC, power, card service).

There is no symbol map for the ARM7, so functions are discovered here:
  * seeds: the entry point, its call targets, and code pointers found in
    literal pools that start with a function prologue
  * a function runs from its entry to the next known entry (calls and tail
    calls create new entries)
  * repeated until nothing new turns up

Modules follow the ARM7's autoload layout (read from its module parameters):
  a7c  crt0 at 0x02380000 (runs in place)
  a7w  code autoloaded to ARM7 WRAM (0x037F8000)
  a7m  code autoloaded to main RAM (0x027E0000)

Generated code calls mem7_* / bios7_swi / dispatch7_call (renamed by macros in
the prelude), so the ARM7 gets its own memory map and BIOS in the runtime.

Usage: python recomp/arm7.py --extract <extract/usa> --out <dir> [--hle hle7.txt]
"""
import argparse
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from build import Func, Module, Recompiler, write_if_changed  # noqa: E402
from insn import BRANCH, CALL, Decoder, h  # noqa: E402

PRELUDE7 = """/* ARM7 code: memory, BIOS and dispatch go to the ARM7's side of the runtime.
 * The macros come first so cpu.h's inline helpers bind to them too. */
#define mem_read8 mem7_read8
#define mem_read16 mem7_read16
#define mem_read32 mem7_read32
#define mem_write8 mem7_write8
#define mem_write16 mem7_write16
#define mem_write32 mem7_write32
#define bios_swi bios7_swi
#define dispatch_call dispatch7_call
#define cpu_unhandled cpu7_unhandled
#define g_cyc g_cyc7
#define CYC_SYNC 2048u   /* the ARM7 runs at the bus clock: about one scanline of work between yields */
#define recomp_trace_enter recomp7_trace_enter
#include "cpu.h"
#include "funcs7.h"

"""


def load_arm7(extract):
    data = open(os.path.join(extract, "arm7", "arm7.bin"), "rb").read()
    base = 0x02380000
    w = lambda a: struct.unpack_from("<I", data, a - base)[0]
    params = 0x02380204
    lst, lst_end, src = w(params), w(params + 4), w(params + 8)
    mods = [Module("a7c", -1, base, data[: src - base])]
    names = ["a7w", "a7m", "a7x", "a7y"]
    a, s, k = lst, src, 0
    while a < lst_end:
        dst, size = w(a), w(a + 4)
        mods.append(Module(names[k], -1, dst, data[s - base: s - base + size]))
        s += size
        a += 12
        k += 1
    return mods


def is_prologue(dec, a, thumb):
    if thumb:
        hw = dec.half(a)
        return (hw & 0xFE00) == 0xB400 or (hw & 0xFF80) == 0xB080
    w = dec.word(a)
    return (w & 0xFFFF0000) == 0xE92D0000 or (w & 0xFFFFF000) == 0xE24DD000 or w == 0xE1A0C00D


class Discoverer:
    def __init__(self, mods):
        self.mods = mods
        self.entries = {}   # (module name, addr) -> thumb
        self.decs = {m.name: Decoder(m.base, m.data) for m in mods}

    def mod_of(self, a):
        for m in self.mods:
            if m.contains(a):
                return m
        return None

    def add(self, a, thumb):
        m = self.mod_of(a)
        if not m or (thumb and a & 1) or (not thumb and a & 3):
            return False
        key = (m.name, a)
        if key in self.entries:
            return False
        self.entries[key] = thumb
        return True

    def layout(self):
        """(Re)build Func objects: each runs to the next entry in its module."""
        for m in self.mods:
            m.funcs = {}
            addrs = sorted(a for (n, a) in self.entries if n == m.name)
            for i, a in enumerate(addrs):
                end = addrs[i + 1] if i + 1 < len(addrs) else m.end
                m.funcs[a] = Func(m, f"arm7_{a:08x}", a, end - a, self.entries[(m.name, a)])
            m.sym_addrs = addrs

    def run(self, seeds):
        for a, t in seeds:
            self.add(a, t)
        rc = Recompiler(self.mods)
        for rnd in range(50):
            self.layout()
            new = 0
            pointer_cands = []
            pool_ptrs = []
            for m in self.mods:
                dec = self.decs[m.name]
                for fa, f in list(m.funcs.items()):
                    insns, _ = rc.discover(f, dec)
                    end = f.addr + f.size
                    for a, i in insns.items():
                        if i.flow == CALL:
                            new += self.add(i.target, i.target_thumb)
                        elif i.flow == BRANCH and not (f.addr <= i.target < end):
                            new += self.add(i.target, f.thumb)
                        for ea, sz in i.pool_refs:
                            if sz == 4:
                                v = dec.word(ea)
                                # an address loaded into lr or r12 is a jump target (return
                                # continuation / long-call stub) even without a prologue
                                via_r12 = i.body.startswith("c->r[12] =") and any(
                                    (n := insns.get(a + 4 * k)) is not None and n.flow in ("jump_reg", "call_reg")
                                    and "c->r[12]" in n.body for k in (1, 2, 3))
                                if i.body.startswith("c->r[14] =") or via_r12:
                                    new += self.add(v & ~1, bool(v & 1))
                                else:
                                    pointer_cands.append(v)
                                    pool_ptrs.append(v)
            if new == 0:
                # pointers stored anywhere in the program's data, not only literal pools
                for m in self.mods:
                    words = [struct.unpack_from("<I", m.data, o)[0] for o in range(0, len(m.data) - 3, 4)]
                    for k, v in enumerate(words):
                        pointer_cands.append(v)
                        # tables of handler pointers: neighbours point into the same module
                        tm = self.mod_of(v)
                        if tm and not v & 3 and any(0 <= k + d < len(words) and tm.contains(words[k + d]) and words[k + d] != v
                                                    for d in (-2, -1, 1, 2)):
                            pool_ptrs.append(v)
                # literal-pool pointers to code without a prologue (leaf callbacks):
                # accept when the first three words decode as ordinary ARM code
                for v in pool_ptrs:
                    m = self.mod_of(v)
                    if not m or v & 3 or v + 16 > m.end:
                        continue
                    dec = self.decs[m.name]
                    if all((dec.word(v + 4 * k) >> 28) == 0xE and dec.arm(v + 4 * k).mn != "udf" for k in range(3)):
                        new += self.add(v, False)
                for v in pointer_cands:
                    thumb = bool(v & 1)
                    t = v & ~1
                    m = self.mod_of(t)
                    if m and not (t & (1 if thumb else 3)) and is_prologue(self.decs[m.name], t, thumb):
                        new += self.add(t, thumb)
            if new == 0:
                # prologues in code no known function covers
                covered = set()
                for m in self.mods:
                    dec = self.decs[m.name]
                    for f in m.funcs.values():
                        insns, _ = rc.discover(f, dec)
                        for a, i in insns.items():
                            covered.update(range(a, a + i.size))
                for m in self.mods:
                    dec = self.decs[m.name]
                    for a in range(m.base, m.end - 8, 4):
                        if a not in covered and is_prologue(dec, a, False) and (dec.word(a + 4) >> 28) == 0xE:
                            new += self.add(a, False)
            print(f"round {rnd}: {len(self.entries)} functions (+{new})")
            if new == 0:
                break
        self.layout()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hle")
    ap.add_argument("--seeds", help="extra '<addr> arm|thumb' entries, one per line")
    args = ap.parse_args()

    mods = load_arm7(args.extract)
    for m in mods:
        print(f"{m.name}: {m.base:08x}-{m.end:08x} ({len(m.data)} bytes)")
    seeds = [(0x02380000, False)]
    if args.seeds:
        for line in open(args.seeds):
            p = line.split("#")[0].split()
            if len(p) == 2:
                seeds.append((int(p[0], 16), p[1] == "thumb"))
    disc = Discoverer(mods)
    disc.run(seeds)

    hle = set()
    if args.hle:
        for line in open(args.hle):
            p = line.split("#")[0].split()
            if len(p) == 2:
                hle.add((p[0], int(p[1], 16)))

    rc = Recompiler(mods)
    code = {}
    queue = list(rc.all_funcs())
    while queue:
        for fn in queue:
            key = (fn.module.name, fn.addr)
            if key in hle:
                code[key] = f"/* {fn.cname}: provided by the runtime (HLE) */"
                rc.stats["hle_functions"] += 1
                continue
            code[key] = rc.emit_func(fn)
        queue, rc.pending = rc.pending, []

    os.makedirs(args.out, exist_ok=True)
    funcs = list(rc.all_funcs())
    with write_if_changed(os.path.join(args.out, "funcs7.h")) as f:
        f.write('#pragma once\n#include "cpu.h"\n' + "".join(f"void {x.cname}(Cpu* c);\n" for x in funcs))
    per = 300
    nfiles = 0
    for m in mods:
        mf = [m.funcs[a] for a in sorted(m.funcs)]
        for k in range(0, len(mf), per):
            with write_if_changed(os.path.join(args.out, f"{m.name}_{k // per:02d}.c")) as f:
                f.write(PRELUDE7)
                for fn in mf[k:k + per]:
                    f.write(code[(m.name, fn.addr)] + "\n\n")
            nfiles += 1
    with write_if_changed(os.path.join(args.out, "dispatch7_table.c")) as f:
        f.write('#include "funcs7.h"\n#include "dispatch.h"\n\nconst RecompFunc g_recomp7_funcs[] = {\n')
        for x in funcs:
            f.write(f'    {{ -1, {h(x.addr | x.thumb)}, {x.size}, {x.cname}, "{x.name}" }},\n')
        f.write("};\nconst unsigned g_recomp7_func_count = sizeof(g_recomp7_funcs) / sizeof(g_recomp7_funcs[0]);\n")
    with write_if_changed(os.path.join(args.out, "report7.txt")) as f:
        for k in sorted(rc.stats):
            f.write(f"{k}: {rc.stats[k]}\n")
        f.write("\n".join(rc.problems))
    for k in sorted(rc.stats):
        print(f"{k:26s} {rc.stats[k]}")
    print(f"{nfiles} C files written to {args.out}")


if __name__ == "__main__":
    main()
