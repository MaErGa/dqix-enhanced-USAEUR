"""Cross-check the recompiler's decoder against LLVM's ARM disassembler.

ARM code is checked with LLVM in ARMv6 mode: its ARMv5 mode wrongly rejects
some valid multiply encodings (register restrictions it enforces too strictly).

For every instruction the recompiler translates, compare its base mnemonic
(and branch target) with what llvm-objdump says.
"""
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recomp"))
from build import Recompiler, load_modules  # noqa: E402
from insn import BRANCH, CALL, Decoder  # noqa: E402

LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f ]+?)\s{2,}(\S+)\s*(.*)$")

# Map LLVM mnemonics (cond/flag/size suffixes stripped) to ours
ALIASES = {
    "push": {"push", "stmdb", "str"}, "pop": {"pop", "ldmia", "ldr"},
    "lsl": {"mov", "lsl", "movs"}, "lsr": {"mov", "lsr"}, "asr": {"mov", "asr"},
    "ror": {"mov", "ror"}, "rrx": {"mov"}, "nop": {"mov"},
    "stmdb": {"stmdb"}, "ldmia": {"ldmia"}, "stm": {"stmia", "stm"}, "ldm": {"ldmia", "ldm"},
    "stmib": {"stmib"}, "ldmib": {"ldmib"}, "stmda": {"stmda"}, "ldmda": {"ldmda"},
    "ldmdb": {"ldmdb"}, "stmia": {"stmia", "stm"},
    "adr": {"add", "sub", "adr"}, "neg": {"rsbs", "rsb"}, "rsbs": {"rsbs"},
    "smlabb": {"smla"}, "smlabt": {"smla"}, "smlatb": {"smla"}, "smlatt": {"smla"},
    "smulbb": {"smul"}, "smulbt": {"smul"}, "smultb": {"smul"}, "smultt": {"smul"},
    "smlawb": {"smlaw"}, "smlawt": {"smlaw"}, "smulwb": {"smulw"}, "smulwt": {"smulw"},
    "smlalbb": {"smlal"}, "smlalbt": {"smlal"}, "smlaltb": {"smlal"}, "smlaltt": {"smlal"},
    "swi": {"svc"}, "udf": {"udf"},
}
CONDS = ("eq", "ne", "hs", "cs", "lo", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al")


def norm(llvm_mn, ours):
    m = llvm_mn.split(".")[0]
    cands = {m}
    for cnd in CONDS:
        if m.endswith(cnd) and len(m) > len(cnd):
            cands.add(m[: -len(cnd)])
    for c in list(cands):
        if c.endswith("s") and c not in ("bics", "mvns"):
            cands.add(c[:-1])
    cands |= {c[:-1] for c in list(cands) if c.endswith("s")}
    o = {ours, ours.rstrip("s")}
    for c in list(cands):
        if c in ALIASES:
            cands |= ALIASES[c]
    return bool(cands & o) or any(c.startswith(ours) for c in cands)


def objdump(mod, thumb):
    with tempfile.TemporaryDirectory() as d:
        b = os.path.join(d, "m.bin")
        open(b, "wb").write(mod.data)
        s = os.path.join(d, "m.s")
        open(s, "w").write(f'.section .text,"ax"\n.incbin "{b}"\n')
        subprocess.run(["clang", "--target=armv5te-none-eabi", "-c", s, "-o", f"{d}/m.o"], check=True)
        subprocess.run(["ld.lld", f"-Ttext={mod.base:#x}", f"{d}/m.o", "-o", f"{d}/m.elf"],
                       check=True, capture_output=True)
        out = subprocess.run(["llvm-objdump", "-d", "--triple=" + ("thumbv5te" if thumb else os.environ.get("ARM_TRIPLE", "armv6")) + "-none-eabi",
                              f"{d}/m.elf"], capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        m = LINE.match(line)
        if m:
            res[int(m.group(1), 16)] = (m.group(3), m.group(4))
    return res


def main(extract, config):
    mods = load_modules(extract, config)
    rc = Recompiler(mods)
    total = bad = 0
    bad_kinds = Counter()
    examples = []
    for mod in mods:
        if not mod.funcs:
            continue
        llvm = {False: objdump(mod, False), True: objdump(mod, True)}
        dec = Decoder(mod.base, mod.data)
        for fa in sorted(mod.funcs):
            f = mod.funcs[fa]
            insns, _ = rc.discover(f, dec)
            for a, i in insns.items():
                ref = llvm[f.thumb].get(a)
                if ref is None:
                    continue
                total += 1
                ok = norm(ref[0], i.mn)
                if ok and i.flow in (BRANCH, CALL):
                    t = re.search(r"0x([0-9a-f]+)", ref[1])
                    ok = bool(t) and int(t.group(1), 16) == i.target
                if not ok:
                    bad += 1
                    bad_kinds[(ref[0], i.mn)] += 1
                    if len(examples) < 25:
                        examples.append(f"{mod.name} {a:08x} {i.word:08x}: llvm '{ref[0]} {ref[1]}' vs ours '{i.mn}'")
    print(f"checked {total} instructions, {bad} disagreements")
    for k, v in bad_kinds.most_common(30):
        print(f"  {v:6d}  llvm={k[0]:10s} ours={k[1]}")
    print("\n".join(examples))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
