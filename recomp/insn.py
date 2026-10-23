"""ARMv5TE (ARM + Thumb) instruction decoder that emits C.

Each decoded instruction is an `Insn`:
  * `body`  - C statements implementing it (already includes condition-code
              guarding only when the flow is SEQ; branch-like flows are guarded
              by the function emitter, which knows labels and call targets).
  * `flow`  - how control continues (see FLOW_*).
  * `mn`    - base mnemonic, used to cross-check the decoder against LLVM.
"""
from dataclasses import dataclass, field

SEQ = "seq"            # falls through
BRANCH = "branch"      # direct jump to `target`
CALL = "call"          # direct call to `target` (target_thumb gives mode), then falls through
RET = "ret"            # return to caller (body runs first)
JUMP_REG = "jump_reg"  # indirect jump; body leaves target in local `tgt`
CALL_REG = "call_reg"  # indirect call; body leaves target in local `tgt`, then falls through
JUMPTABLE = "jumptable"  # `add pc, pc, rX, lsl #2`; index expression in `index`
EXC_RET = "exc_ret"    # exception return (movs pc, lr / subs pc, lr, #4 / ldm ^)
STOP = "stop"          # undefined instruction / never returns

CONDS = ["EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "", ""]


@dataclass
class Insn:
    addr: int
    size: int
    word: int
    cond: str = ""          # "" = always
    flow: str = SEQ
    body: str = ""
    target: int = 0
    target_thumb: bool = False
    index: str = ""         # JUMPTABLE index expression
    mn: str = ""
    pool_refs: list = field(default_factory=list)  # literal-pool addresses read
    sets_lr_pc: bool = False  # `mov lr, pc` (old-style indirect call idiom)
    table_words: int = 0      # ldr pc, [pc, rX, lsl #2]: table follows
    adr: int = -1             # address computed by `add rX, pc, #imm` (possible jump table)


def sx(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def h(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


def R(n, pcval):
    return h(pcval) if n == 15 else f"c->r[{n}]"


DP_NAMES = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
            "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"]


class Decoder:
    """Decodes instructions of one module image."""

    def __init__(self, base, data):
        self.base = base
        self.data = data

    def in_image(self, a, n=4):
        return self.base <= a and a + n <= self.base + len(self.data)

    def word(self, a):
        o = a - self.base
        return int.from_bytes(self.data[o:o + 4], "little")

    def half(self, a):
        o = a - self.base
        return int.from_bytes(self.data[o:o + 2], "little")

    def byte(self, a):
        return self.data[a - self.base]

    # ------------------------------------------------------------------ ARM
    def arm(self, a):
        w = self.word(a)
        i = Insn(a, 4, w)
        cond = w >> 28
        if cond == 0xF:
            return self._arm_uncond(i, w)
        i.cond = CONDS[cond]
        op = (w >> 25) & 7
        if op == 0:
            if (w & 0x90) == 0x90:
                if (w >> 5) & 3 == 0:
                    if (w >> 24) & 1 == 0:
                        return self._arm_mul(i, w)
                    if (w & 0x0FB00FF0) == 0x01000090:
                        return self._arm_swp(i, w)
                    return self._undef(i)
                return self._arm_extra_ls(i, w)
            if (w >> 23) & 3 == 2 and (w >> 20) & 1 == 0:
                return self._arm_misc(i, w)
            return self._arm_dp(i, w)
        if op == 1:
            if (w >> 23) & 3 == 2 and (w >> 20) & 3 == 2:
                return self._arm_msr(i, w, imm=True)
            if (w >> 23) & 3 == 2 and (w >> 20) & 1 == 0:
                return self._undef(i)
            return self._arm_dp(i, w)
        if op == 2:
            return self._arm_ls(i, w)
        if op == 3:
            if w & 0x10:
                return self._undef(i)
            return self._arm_ls(i, w)
        if op == 4:
            return self._arm_ldm(i, w)
        if op == 5:
            i.target = (a + 8 + (sx(w & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF
            if (w >> 24) & 1:
                i.flow, i.mn = CALL, "bl"
            else:
                i.flow, i.mn = BRANCH, "b"
            return i
        if op == 6:
            i.mn = "ldc" if (w >> 20) & 1 else "stc"
            return self._unhandled(i)
        # op == 7
        if (w >> 24) & 1:
            i.mn = "svc"
            i.body = f"bios_swi(c, {(w >> 16) & 0xFF});"
            return i
        if w & 0x10:
            return self._arm_cp(i, w)
        i.mn = "cdp"
        return self._unhandled(i)

    def _undef(self, i):
        i.mn = "udf"
        i.flow = STOP
        i.cond = ""
        i.body = f"cpu_unhandled(c, {h(i.addr)}, {h(i.word)}); return;"
        return i

    def _unhandled(self, i):
        i.body = f"cpu_unhandled(c, {h(i.addr)}, {h(i.word)});"
        return i

    def _arm_uncond(self, i, w):
        if (w >> 25) & 7 == 5:
            i.mn = "blx"
            i.flow = CALL
            i.target_thumb = True
            i.target = (i.addr + 8 + (sx(w & 0xFFFFFF, 24) << 2) + (((w >> 24) & 1) << 1)) & 0xFFFFFFFF
            return i
        if (w & 0x0D70F000) == 0x0550F000:
            i.mn = "pld"
            return i
        return self._undef(i)

    # operand 2 for data processing; returns (setup, value_expr, carry_expr)
    def _op2(self, w, a, need_carry):
        if (w >> 25) & 1:
            rot = ((w >> 8) & 15) * 2
            imm = w & 0xFF
            v = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
            return "", h(v), ("c->cf" if rot == 0 else str(v >> 31))
        rm = w & 15
        st = (w >> 5) & 3
        if (w >> 4) & 1 == 0:
            amt = (w >> 7) & 31
            m = R(rm, a + 8)
            if st == 0:
                if amt == 0:
                    return "", m, "c->cf"
                return "", f"({m} << {amt})", f"(({m} >> {32 - amt}) & 1)"
            if st == 1:
                if amt == 0:
                    return "", "0u", f"({m} >> 31)"
                return "", f"({m} >> {amt})", f"(({m} >> {amt - 1}) & 1)"
            if st == 2:
                if amt == 0:
                    return "", f"(uint32_t)((int32_t){m} >> 31)", f"({m} >> 31)"
                return "", f"(uint32_t)((int32_t){m} >> {amt})", f"(({m} >> {amt - 1}) & 1)"
            if amt == 0:
                return "", f"((c->cf << 31) | ({m} >> 1))", f"({m} & 1)"
            return "", f"ror32({m}, {amt})", f"(({m} >> {amt - 1}) & 1)"
        rs = (w >> 8) & 15
        m = R(rm, a + 12)
        s = R(rs, a + 12)
        fn = ["lsl", "lsr", "asr", "ror"][st]
        if need_carry:
            return (f"uint32_t sc; uint32_t sv = {fn}_rc({m}, {s}, c->cf, &sc);", "sv", "sc")
        return "", f"{fn}_r({m}, {s})", "c->cf"

    def _arm_dp(self, i, w):
        a = i.addr
        opc = (w >> 21) & 15
        S = (w >> 20) & 1
        rn = (w >> 16) & 15
        rd = (w >> 12) & 15
        i.mn = DP_NAMES[opc]
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        need_carry = bool(S) and logical
        setup, op2, carry = self._op2(w, a, need_carry)
        regshift = not ((w >> 25) & 1) and ((w >> 4) & 1)
        n = R(rn, a + (12 if regshift else 8))
        lines = [setup] if setup else []
        if not S or rd == 15:
            expr = {
                0: f"{n} & {op2}", 1: f"{n} ^ {op2}", 2: f"{n} - {op2}", 3: f"{op2} - {n}",
                4: f"{n} + {op2}", 5: f"{n} + {op2} + c->cf", 6: f"{n} - {op2} - !c->cf",
                7: f"{op2} - {n} - !c->cf", 12: f"{n} | {op2}", 13: f"{op2}",
                14: f"{n} & ~{op2}", 15: f"~{op2}"}.get(opc)
            if expr is None:  # tst/teq/cmp/cmn with S=0 can't happen here
                return self._undef(i)
            if rd == 15:
                return self._pc_write(i, lines, expr, exc=bool(S))
            if rn == 15 and (w >> 25) & 1 and opc in (2, 4):
                rot = ((w >> 8) & 15) * 2
                imm = w & 0xFF
                imm = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                i.adr = (a + 8 + (imm if opc == 4 else -imm)) & 0xFFFFFFFF
            # Plain moves of registers: readable output
            lines.append(f"c->r[{rd}] = {expr};")
            i.body = " ".join(lines)
            if opc == 13 and rd == 14 and not (w >> 25) & 1 and (w & 0xFFF) == 15:
                i.sets_lr_pc = True   # mov lr, pc
            if opc == 4 and rd == 14 and rn == 15 and (w >> 25) & 1 and (w & 0xFFF) == 0:
                i.sets_lr_pc = True   # add lr, pc, #0 (same value as mov lr, pc)
            return i
        # S set, rd != 15
        cin = "c->cf"
        if logical:
            expr = {0: f"{n} & {op2}", 1: f"{n} ^ {op2}", 8: f"{n} & {op2}", 9: f"{n} ^ {op2}",
                    12: f"{n} | {op2}", 13: f"{op2}", 14: f"{n} & ~{op2}", 15: f"~{op2}"}[opc]
            # carry must be computed from operands before any register write
            lines.append(f"uint32_t cc = {carry}; uint32_t res = nz(c, {expr}); c->cf = cc;")
        else:
            expr = {2: f"sub_s(c, {n}, {op2})", 3: f"sub_s(c, {op2}, {n})", 4: f"add_s(c, {n}, {op2})",
                    5: f"adc_s(c, {n}, {op2}, {cin})", 6: f"sbc_s(c, {n}, {op2}, {cin})",
                    7: f"sbc_s(c, {op2}, {n}, {cin})", 10: f"sub_s(c, {n}, {op2})",
                    11: f"add_s(c, {n}, {op2})"}[opc]
            lines.append(f"uint32_t res = {expr};")
        if opc not in (8, 9, 10, 11):
            lines.append(f"c->r[{rd}] = res;")
        else:
            lines.append("(void)res;")
        i.body = "{ " + " ".join(lines) + " }"
        return i

    def _pc_write(self, i, lines, expr, exc=False):
        """Data-processing write to PC."""
        w = i.word
        if exc:
            i.flow = EXC_RET
            i.body = "{ " + " ".join(lines) + f" uint32_t tgt = {expr}; cpu_exception_return(c, tgt); }}"
            return i
        opc = (w >> 21) & 15
        # mov pc, lr  -> return
        if opc == 13 and not (w >> 25) & 1 and (w & 0xFFF) == 14:
            i.flow = RET
            return i
        # add pc, pc, rX, lsl #2  -> jump table
        if opc == 4 and ((w >> 16) & 15) == 15 and not (w >> 25) & 1 and (w & 0xFF0) == 0x100:
            i.flow = JUMPTABLE
            i.index = f"c->r[{w & 15}]"
            return i
        i.flow = JUMP_REG
        i.body = " ".join(lines + [f"uint32_t tgt = {expr};"])
        return i

    def _arm_misc(self, i, w):
        op = (w >> 21) & 3
        lo = (w >> 4) & 15
        rd = (w >> 12) & 15
        rm = w & 15
        rn = (w >> 16) & 15
        rs = (w >> 8) & 15
        if lo == 0:
            if op & 1:
                return self._arm_msr(i, w, imm=False)
            i.mn = "mrs"
            if (w >> 22) & 1:
                i.body = f"c->r[{rd}] = cpu_read_spsr(c);"
            else:
                i.body = f"c->r[{rd}] = CPSR_GET(c);"
            return i
        if lo == 1 and op == 1:
            i.mn = "bx"
            if rm == 14:
                i.flow = RET
            else:
                i.flow = JUMP_REG
                i.body = f"uint32_t tgt = {R(rm, i.addr + 8)};"
            return i
        if lo == 1 and op == 3:
            i.mn = "clz"
            i.body = f"c->r[{rd}] = clz32({R(rm, i.addr + 8)});"
            return i
        if lo == 3 and op == 1:
            i.mn = "blx"
            i.flow = CALL_REG
            i.body = f"uint32_t tgt = {R(rm, i.addr + 8)};"
            return i
        if lo == 5:
            i.mn = ["qadd", "qsub", "qdadd", "qdsub"][op]
            m, n = f"c->r[{rm}]", f"c->r[{rn}]"
            if op == 0:
                e = f"sat_add(c, {m}, {n})"
            elif op == 1:
                e = f"sat_sub(c, {m}, {n})"
            elif op == 2:
                e = f"sat_add(c, {m}, sat_add(c, {n}, {n}))"
            else:
                e = f"sat_sub(c, {m}, sat_add(c, {n}, {n}))"
            i.body = f"c->r[{rd}] = {e};"
            return i
        if lo == 7 and op == 1:
            i.mn = "bkpt"
            i.cond = ""
            i.body = f"cpu_breakpoint(c, {h(i.addr)}, {((w >> 4) & 0xFFF0) | (w & 15)});"
            return i
        if (lo & 9) == 8:  # DSP multiplies: bit7=1, bit4=0
            x = (w >> 5) & 1
            y = (w >> 6) & 1
            m = f"half(c->r[{rm}], {x})"
            s = f"half(c->r[{rs}], {y})"
            if op == 0:
                i.mn = "smla"
                i.body = f"c->r[{rn}] = add_q(c, (uint32_t)({m} * {s}), c->r[{rd}]);"
            elif op == 1:
                prod = f"(uint32_t)(((int64_t)(int32_t)c->r[{rm}] * {s}) >> 16)"
                if x:
                    i.mn = "smulw"
                    i.body = f"c->r[{rn}] = {prod};"
                else:
                    i.mn = "smlaw"
                    i.body = f"c->r[{rn}] = add_q(c, {prod}, c->r[{rd}]);"
            elif op == 2:
                i.mn = "smlal"
                i.body = (f"{{ uint64_t acc = ((uint64_t)c->r[{rn}] << 32) | c->r[{rd}]; "
                          f"acc += (uint64_t)(int64_t)({m} * {s}); "
                          f"c->r[{rd}] = (uint32_t)acc; c->r[{rn}] = (uint32_t)(acc >> 32); }}")
            else:
                i.mn = "smul"
                i.body = f"c->r[{rn}] = (uint32_t)({m} * {s});"
            return i
        return self._undef(i)

    def _arm_msr(self, i, w, imm):
        i.mn = "msr"
        spsr = (w >> 22) & 1
        fields = (w >> 16) & 15
        mask = 0
        for b in range(4):
            if fields & (1 << b):
                mask |= 0xFF << (8 * b)
        if imm:
            rot = ((w >> 8) & 15) * 2
            v = w & 0xFF
            v = ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v
            val = h(v)
        else:
            val = f"c->r[{w & 15}]"
        if spsr:
            i.body = f"cpu_write_spsr(c, {val}, {h(mask)});"
            return i
        parts = [f"uint32_t mv = {val};"]
        if mask & 0xF8000000:
            parts.append("c->n = mv >> 31; c->z = (mv >> 30) & 1; c->cf = (mv >> 29) & 1; "
                         "c->v = (mv >> 28) & 1; c->q = (mv >> 27) & 1;")
        if mask & 0x07FFFFFF:
            parts.append(f"cpu_write_cpsr_ctl(c, mv, {h(mask & 0x07FFFFFF)});")
        i.body = "{ " + " ".join(parts) + " }"
        return i

    def _arm_mul(self, i, w):
        op = (w >> 21) & 7
        S = (w >> 20) & 1
        rd = (w >> 16) & 15
        rn = (w >> 12) & 15
        rs = (w >> 8) & 15
        rm = w & 15
        m, s = f"c->r[{rm}]", f"c->r[{rs}]"
        if op in (0, 1):
            i.mn = "mul" if op == 0 else "mla"
            e = f"{m} * {s}" + (f" + c->r[{rn}]" if op == 1 else "")
            i.body = f"c->r[{rd}] = {'nz(c, ' + e + ')' if S else e};"
            return i
        if op >= 4:
            i.mn = ["umull", "umlal", "smull", "smlal"][op - 4]
            hi, lo = rd, rn
            signed = op >= 6
            if signed:
                prod = f"(uint64_t)((int64_t)(int32_t){m} * (int32_t){s})"
            else:
                prod = f"((uint64_t){m} * {s})"
            acc = f" + (((uint64_t)c->r[{hi}] << 32) | c->r[{lo}])" if op in (5, 7) else ""
            body = f"uint64_t p = {prod}{acc}; c->r[{lo}] = (uint32_t)p; c->r[{hi}] = (uint32_t)(p >> 32);"
            if S:
                body += " c->n = (uint32_t)(p >> 63); c->z = p == 0;"
            i.body = "{ " + body + " }"
            return i
        return self._undef(i)

    def _arm_swp(self, i, w):
        B = (w >> 22) & 1
        rn, rd, rm = (w >> 16) & 15, (w >> 12) & 15, w & 15
        i.mn = "swpb" if B else "swp"
        if B:
            i.body = (f"{{ uint32_t a = c->r[{rn}]; uint32_t t = mem_read8(a); "
                      f"mem_write8(a, (uint8_t)c->r[{rm}]); c->r[{rd}] = t; }}")
        else:
            i.body = (f"{{ uint32_t a = c->r[{rn}]; uint32_t t = ldr_word(a); "
                      f"mem_write32(a & ~3u, c->r[{rm}]); c->r[{rd}] = t; }}")
        return i

    def _arm_cp(self, i, w):
        L = (w >> 20) & 1
        cp = (w >> 8) & 15
        op1 = (w >> 21) & 7
        crn = (w >> 16) & 15
        rd = (w >> 12) & 15
        op2 = (w >> 5) & 7
        crm = w & 15
        i.mn = "mrc" if L else "mcr"
        if cp != 15:
            return self._unhandled(i)
        if L:
            if rd == 15:
                i.body = (f"{{ uint32_t t = cp15_read(c, {op1}, {crn}, {crm}, {op2}); "
                          "c->n = t >> 31; c->z = (t >> 30) & 1; c->cf = (t >> 29) & 1; c->v = (t >> 28) & 1; }")
            else:
                i.body = f"c->r[{rd}] = cp15_read(c, {op1}, {crn}, {crm}, {op2});"
        else:
            i.body = f"cp15_write(c, {op1}, {crn}, {crm}, {op2}, {R(rd, i.addr + 12)});"
        return i

    def _load_expr(self, kind, addr):
        return {"w": f"ldr_word({addr})", "b": f"mem_read8({addr})", "h": f"mem_read16({addr})",
                "sb": f"(uint32_t)(int32_t)(int8_t)mem_read8({addr})",
                "sh": f"(uint32_t)(int32_t)(int16_t)mem_read16({addr})"}[kind]

    def _store_stmt(self, kind, addr, val):
        return {"w": f"mem_write32(({addr}) & ~3u, {val});", "b": f"mem_write8({addr}, (uint8_t){val});",
                "h": f"mem_write16({addr}, (uint16_t){val});"}[kind]

    def _ls_common(self, i, P, U, W, L, rn, rd, off, kind, off_const=None):
        a = i.addr
        base = R(rn, a + 8)
        sign = "+" if U else "-"
        wb = (not P or W) and rn != 15
        # literal-pool load: constant folded
        if rn == 15 and L and off_const is not None and P and not W and rd != 15:
            ea = (a + 8 + (off_const if U else -off_const)) & 0xFFFFFFFF
            size = {"w": 4, "b": 1, "h": 2, "sb": 1, "sh": 2}[kind]
            if self.in_image(ea, size) and (kind != "w" or ea % 4 == 0):
                if kind == "w":
                    v = self.word(ea)
                elif kind in ("h", "sh"):
                    v = self.half(ea)
                    if kind == "sh":
                        v = sx(v, 16)
                else:
                    v = self.byte(ea)
                    if kind == "sb":
                        v = sx(v, 8)
                i.pool_refs.append((ea, size))
                i.body = f"c->r[{rd}] = {h(v)};"
                return i
        ea = f"{base} {sign} {off}" if P else base
        lines = [f"uint32_t a = {ea};"]
        if L:
            lines.append(f"uint32_t val = {self._load_expr(kind, 'a')};")
            if wb:
                lines.append(f"c->r[{rn}] = {base} {sign} {off};" if not P else f"c->r[{rn}] = a;")
            if rd == 15:
                if rn == 13 and not P and U and off_const == 4 and kind == "w":
                    i.flow = RET
                    i.body = "{ " + " ".join(lines) + " (void)val; }"
                    return i
                i.flow = JUMP_REG
                i.body = " ".join(lines + ["uint32_t tgt = val;"])
                return i
            lines.append(f"c->r[{rd}] = val;")
        else:
            val = h(a + 12) if rd == 15 else f"c->r[{rd}]"
            lines.append(self._store_stmt(kind, "a", val))
            if wb:
                lines.append(f"c->r[{rn}] = {base} {sign} {off};" if not P else f"c->r[{rn}] = a;")
        i.body = "{ " + " ".join(lines) + " }"
        return i

    def _arm_ls(self, i, w):
        P, U, B, W, L = [(w >> b) & 1 for b in (24, 23, 22, 21, 20)]
        rn, rd = (w >> 16) & 15, (w >> 12) & 15
        i.mn = ("ldr" if L else "str") + ("b" if B else "")
        if not (w >> 25) & 1:
            off_const = w & 0xFFF
            off = h(off_const)
        else:
            off_const = None
            amt = (w >> 7) & 31
            st = (w >> 5) & 3
            m = R(w & 15, i.addr + 8)
            if st == 0:
                off = f"({m} << {amt})" if amt else m
            elif st == 1:
                off = f"({m} >> {amt})" if amt else "0u"
            elif st == 2:
                off = f"(uint32_t)((int32_t){m} >> {amt if amt else 31})"
            else:
                off = f"ror32({m}, {amt})" if amt else f"((c->cf << 31) | ({m} >> 1))"
            # ldr pc, [pc, rX, lsl #2] : address table follows
            if L and rd == 15 and rn == 15 and P and not W and not B and st == 0 and amt == 2:
                i.table_words = 1
        return self._ls_common(i, P, U, W, L, rn, rd, off, "b" if B else "w", off_const)

    def _arm_extra_ls(self, i, w):
        P, U, I, W, L = [(w >> b) & 1 for b in (24, 23, 22, 21, 20)]
        rn, rd = (w >> 16) & 15, (w >> 12) & 15
        sh = (w >> 5) & 3
        if I:
            off_const = ((w >> 4) & 0xF0) | (w & 15)
            off = h(off_const)
        else:
            off_const = None
            off = R(w & 15, i.addr + 8)
        if sh == 1:
            i.mn = "ldrh" if L else "strh"
            return self._ls_common(i, P, U, W, L, rn, rd, off, "h", off_const)
        if L:
            i.mn = "ldrsb" if sh == 2 else "ldrsh"
            return self._ls_common(i, P, U, W, 1, rn, rd, off, "sb" if sh == 2 else "sh", off_const)
        # ldrd / strd
        load = sh == 2
        i.mn = "ldrd" if load else "strd"
        if rd & 1:
            return self._undef(i)
        base = R(rn, i.addr + 8)
        sign = "+" if U else "-"
        wb = (not P or W) and rn != 15
        lines = [f"uint32_t a = {base} {sign} {off};" if P else f"uint32_t a = {base};"]
        if load:
            lines.append(f"uint32_t v0 = mem_read32(a); uint32_t v1 = mem_read32(a + 4);")
            if wb:
                lines.append(f"c->r[{rn}] = {base} {sign} {off};" if not P else f"c->r[{rn}] = a;")
            lines.append(f"c->r[{rd}] = v0; c->r[{rd + 1}] = v1;")
        else:
            lines.append(f"mem_write32(a, c->r[{rd}]); mem_write32(a + 4, c->r[{rd + 1}]);")
            if wb:
                lines.append(f"c->r[{rn}] = {base} {sign} {off};" if not P else f"c->r[{rn}] = a;")
        i.body = "{ " + " ".join(lines) + " }"
        return i

    def _arm_ldm(self, i, w):
        P, U, S, W, L = [(w >> b) & 1 for b in (24, 23, 22, 21, 20)]
        rn = (w >> 16) & 15
        rl = w & 0xFFFF
        regs = [r for r in range(16) if rl >> r & 1]
        n = len(regs)
        i.mn = ("ldm" if L else "stm") + ("i" if U else "d") + ("b" if P else "a")
        if n == 0:
            return self._undef(i)
        if U:
            start = "b + 4" if P else "b"
            newb = f"b + {4 * n}"
        else:
            start = f"b - {4 * n}" + ("" if P else " + 4")
            newb = f"b - {4 * n}"
        lines = [f"uint32_t b = c->r[{rn}]; uint32_t a = {start};"]
        if L:
            for k, r in enumerate(regs):
                dst = "tgt" if r == 15 else f"c->r[{r}]"
                pre = "uint32_t " if r == 15 else ""
                lines.append(f"{pre}{dst} = mem_read32(a + {4 * k});")
            if W and rn not in regs:
                lines.append(f"c->r[{rn}] = {newb};")
            if 15 in regs:
                if S:
                    i.flow = EXC_RET
                    i.body = "{ " + " ".join(lines) + " cpu_exception_return(c, tgt); }"
                    return i
                if rn == 13:
                    i.flow = RET
                    i.body = "{ " + " ".join(lines) + " (void)tgt; }"
                    return i
                i.flow = JUMP_REG
                i.body = " ".join(lines)
                return i
        else:
            for k, r in enumerate(regs):
                v = h(i.addr + 12) if r == 15 else f"c->r[{r}]"
                if r == rn and k != 0 and W:
                    v = newb
                lines.append(f"mem_write32(a + {4 * k}, {v});")
            if W:
                lines.append(f"c->r[{rn}] = {newb};")
        i.body = "{ " + " ".join(lines) + " }"
        return i

    # ---------------------------------------------------------------- Thumb
    def thumb(self, a):
        w = self.half(a)
        i = Insn(a, 2, w)
        top5 = w >> 11
        pc = a + 4
        if top5 < 3:  # shift by immediate
            op = top5
            amt = (w >> 6) & 31
            rs, rd = (w >> 3) & 7, w & 7
            m = f"c->r[{rs}]"
            i.mn = ["lsl", "lsr", "asr"][op]
            if op == 0:
                if amt == 0:
                    i.body = f"c->r[{rd}] = nz(c, {m});"
                    i.mn = "movs"
                else:
                    i.body = f"{{ uint32_t cc = ({m} >> {32 - amt}) & 1; c->r[{rd}] = nz(c, {m} << {amt}); c->cf = cc; }}"
            elif op == 1:
                if amt == 0:
                    i.body = f"{{ uint32_t cc = {m} >> 31; c->r[{rd}] = nz(c, 0); c->cf = cc; }}"
                else:
                    i.body = f"{{ uint32_t cc = ({m} >> {amt - 1}) & 1; c->r[{rd}] = nz(c, {m} >> {amt}); c->cf = cc; }}"
            else:
                a2 = amt or 32
                sh = min(a2, 31)
                i.body = (f"{{ uint32_t cc = ({m} >> {a2 - 1}) & 1; "
                          f"c->r[{rd}] = nz(c, (uint32_t)((int32_t){m} >> {sh})); c->cf = cc; }}")
            return i
        if top5 == 3:  # add/sub
            I = (w >> 10) & 1
            sub = (w >> 9) & 1
            rn = (w >> 6) & 7
            rs, rd = (w >> 3) & 7, w & 7
            o = h(rn) if I else f"c->r[{rn}]"
            i.mn = "subs" if sub else "adds"
            i.body = f"c->r[{rd}] = {'sub_s' if sub else 'add_s'}(c, c->r[{rs}], {o});"
            return i
        if top5 < 8:  # mov/cmp/add/sub imm8
            op = (w >> 11) & 3
            rd = (w >> 8) & 7
            imm = h(w & 0xFF)
            i.mn = ["movs", "cmp", "adds", "subs"][op]
            i.body = [f"c->r[{rd}] = nz(c, {imm});", f"(void)sub_s(c, c->r[{rd}], {imm});",
                      f"c->r[{rd}] = add_s(c, c->r[{rd}], {imm});",
                      f"c->r[{rd}] = sub_s(c, c->r[{rd}], {imm});"][op]
            return i
        if (w >> 10) == 0x10:  # ALU
            op = (w >> 6) & 15
            rs, rd = (w >> 3) & 7, w & 7
            d, s = f"c->r[{rd}]", f"c->r[{rs}]"
            names = ["ands", "eors", "lsls", "lsrs", "asrs", "adcs", "sbcs", "rors",
                     "tst", "rsbs", "cmp", "cmn", "orrs", "muls", "bics", "mvns"]
            i.mn = names[op]
            body = {
                0: f"{d} = nz(c, {d} & {s});", 1: f"{d} = nz(c, {d} ^ {s});",
                2: f"{{ uint32_t cc; {d} = nz(c, lsl_rc({d}, {s}, c->cf, &cc)); c->cf = cc; }}",
                3: f"{{ uint32_t cc; {d} = nz(c, lsr_rc({d}, {s}, c->cf, &cc)); c->cf = cc; }}",
                4: f"{{ uint32_t cc; {d} = nz(c, asr_rc({d}, {s}, c->cf, &cc)); c->cf = cc; }}",
                5: f"{d} = adc_s(c, {d}, {s}, c->cf);", 6: f"{d} = sbc_s(c, {d}, {s}, c->cf);",
                7: f"{{ uint32_t cc; {d} = nz(c, ror_rc({d}, {s}, c->cf, &cc)); c->cf = cc; }}",
                8: f"(void)nz(c, {d} & {s});", 9: f"{d} = sub_s(c, 0, {s});",
                10: f"(void)sub_s(c, {d}, {s});", 11: f"(void)add_s(c, {d}, {s});",
                12: f"{d} = nz(c, {d} | {s});", 13: f"{d} = nz(c, {d} * {s});",
                14: f"{d} = nz(c, {d} & ~{s});", 15: f"{d} = nz(c, ~{s});"}[op]
            i.body = body
            return i
        if (w >> 10) == 0x11:  # hi-register ops / bx
            op = (w >> 8) & 3
            rs = (w >> 3) & 15
            rd = (w & 7) | ((w >> 4) & 8)
            s = R(rs, pc)
            if op == 3:
                if (w >> 7) & 1:
                    i.mn = "blx"
                    i.flow = CALL_REG
                    i.body = f"uint32_t tgt = {s};"
                    return i
                i.mn = "bx"
                if rs == 14:
                    i.flow = RET
                else:
                    i.flow = JUMP_REG
                    i.body = f"uint32_t tgt = {s};"
                return i
            d = R(rd, pc)
            if op == 1:
                i.mn = "cmp"
                i.body = f"(void)sub_s(c, {d}, {s});"
                return i
            i.mn = "add" if op == 0 else "mov"
            expr = f"{d} + {s}" if op == 0 else s
            if rd == 15:
                if op == 2 and rs == 14:
                    i.flow = RET
                    return i
                i.flow = JUMP_REG
                i.body = f"uint32_t tgt = {expr};"
                return i
            i.body = f"c->r[{rd}] = {expr};"
            if op == 2 and rd == 14 and rs == 15:
                i.sets_lr_pc = True
            return i
        if (w >> 11) == 9:  # ldr rd, [pc, #imm]
            rd = (w >> 8) & 7
            ea = ((pc & ~3) + (w & 0xFF) * 4) & 0xFFFFFFFF
            i.mn = "ldr"
            if self.in_image(ea):
                i.pool_refs.append((ea, 4))
                i.body = f"c->r[{rd}] = {h(self.word(ea))};"
            else:
                i.body = f"c->r[{rd}] = mem_read32({h(ea)});"
            return i
        if (w >> 12) == 5:  # load/store register offset
            ro, rb, rd = (w >> 6) & 7, (w >> 3) & 7, w & 7
            a_ = f"c->r[{rb}] + c->r[{ro}]"
            op = (w >> 9) & 7
            i.mn = ["str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"][op]
            kinds = ["w", "h", "b", "sb", "w", "h", "b", "sh"]
            if op in (0, 1, 2):
                i.body = f"{{ uint32_t a = {a_}; {self._store_stmt(kinds[op], 'a', f'c->r[{rd}]')} }}"
            else:
                i.body = f"{{ uint32_t a = {a_}; c->r[{rd}] = {self._load_expr(kinds[op], 'a')}; }}"
            return i
        if (w >> 13) == 3:  # ldr/str imm (word/byte)
            B = (w >> 12) & 1
            L = (w >> 11) & 1
            off = (w >> 6) & 31
            if not B:
                off *= 4
            rb, rd = (w >> 3) & 7, w & 7
            k = "b" if B else "w"
            i.mn = ("ldr" if L else "str") + ("b" if B else "")
            a_ = f"c->r[{rb}] + {off}"
            if L:
                i.body = f"c->r[{rd}] = {self._load_expr(k, a_)};"
            else:
                i.body = self._store_stmt(k, a_, f"c->r[{rd}]")
            return i
        if (w >> 12) == 8:  # ldrh/strh imm
            L = (w >> 11) & 1
            off = ((w >> 6) & 31) * 2
            rb, rd = (w >> 3) & 7, w & 7
            i.mn = "ldrh" if L else "strh"
            a_ = f"c->r[{rb}] + {off}"
            i.body = f"c->r[{rd}] = mem_read16({a_});" if L else f"mem_write16({a_}, (uint16_t)c->r[{rd}]);"
            return i
        if (w >> 12) == 9:  # sp-relative
            L = (w >> 11) & 1
            rd = (w >> 8) & 7
            a_ = f"c->r[13] + {(w & 0xFF) * 4}"
            i.mn = "ldr" if L else "str"
            i.body = f"c->r[{rd}] = ldr_word({a_});" if L else f"mem_write32(({a_}) & ~3u, c->r[{rd}]);"
            return i
        if (w >> 12) == 10:  # add rd, pc/sp, #imm
            sp = (w >> 11) & 1
            rd = (w >> 8) & 7
            imm = (w & 0xFF) * 4
            i.mn = "add" if sp else "adr"
            i.body = f"c->r[{rd}] = c->r[13] + {imm};" if sp else f"c->r[{rd}] = {h((pc & ~3) + imm)};"
            if not sp:
                i.adr = (pc & ~3) + imm
            return i
        if (w >> 8) == 0xB0:  # add sp, #imm
            imm = (w & 0x7F) * 4
            neg = (w >> 7) & 1
            i.mn = "sub" if neg else "add"
            i.body = f"c->r[13] {'-' if neg else '+'}= {imm};"
            return i
        if (w >> 12) == 0xB and ((w >> 9) & 3) == 2:  # push/pop
            L = (w >> 11) & 1
            Rb = (w >> 8) & 1
            rl = w & 0xFF
            regs = [r for r in range(8) if rl >> r & 1]
            if L:
                i.mn = "pop"
                if Rb:
                    regs.append(15)
                n = len(regs)
                lines = ["uint32_t a = c->r[13];"]
                for k, r in enumerate(regs):
                    if r == 15:
                        lines.append(f"uint32_t tgt = mem_read32(a + {4 * k});")
                    else:
                        lines.append(f"c->r[{r}] = mem_read32(a + {4 * k});")
                lines.append(f"c->r[13] = a + {4 * n};")
                if Rb:
                    i.flow = RET
                    lines.append("(void)tgt;")
                i.body = "{ " + " ".join(lines) + " }"
            else:
                i.mn = "push"
                if Rb:
                    regs.append(14)
                n = len(regs)
                lines = [f"uint32_t a = c->r[13] - {4 * n};"]
                for k, r in enumerate(regs):
                    lines.append(f"mem_write32(a + {4 * k}, c->r[{r}]);")
                lines.append("c->r[13] = a;")
                i.body = "{ " + " ".join(lines) + " }"
            return i
        if (w >> 8) == 0xBE:
            i.mn = "bkpt"
            i.body = f"cpu_breakpoint(c, {h(a)}, {w & 0xFF});"
            return i
        if (w >> 12) == 0xC:  # ldmia/stmia
            L = (w >> 11) & 1
            rb = (w >> 8) & 7
            regs = [r for r in range(8) if w >> r & 1]
            n = len(regs)
            i.mn = "ldm" if L else "stm"
            if n == 0:
                return self._undef(i)
            lines = [f"uint32_t a = c->r[{rb}];"]
            if L:
                for k, r in enumerate(regs):
                    lines.append(f"c->r[{r}] = mem_read32(a + {4 * k});")
                if rb not in regs:
                    lines.append(f"c->r[{rb}] = a + {4 * n};")
            else:
                for k, r in enumerate(regs):
                    lines.append(f"mem_write32(a + {4 * k}, c->r[{r}]);")
                lines.append(f"c->r[{rb}] = a + {4 * n};")
            i.body = "{ " + " ".join(lines) + " }"
            return i
        if (w >> 12) == 0xD:
            cond = (w >> 8) & 15
            if cond == 15:
                i.mn = "svc"
                i.body = f"bios_swi(c, {w & 0xFF});"
                return i
            if cond == 14:
                return self._undef(i)
            i.mn = "b"
            i.cond = CONDS[cond]
            i.flow = BRANCH
            i.target = (pc + (sx(w & 0xFF, 8) << 1)) & 0xFFFFFFFF
            i.target_thumb = True
            return i
        if top5 == 0x1C:
            i.mn = "b"
            i.flow = BRANCH
            i.target = (pc + (sx(w & 0x7FF, 11) << 1)) & 0xFFFFFFFF
            i.target_thumb = True
            return i
        if top5 == 0x1E:  # bl/blx prefix: combine with suffix
            nxt = self.half(a + 2) if self.in_image(a + 2, 2) else 0
            if (nxt >> 11) in (0x1F, 0x1D):
                off = (sx(w & 0x7FF, 11) << 12) + ((nxt & 0x7FF) << 1)
                i.size = 4
                i.word = w | (nxt << 16)
                i.flow = CALL
                if (nxt >> 11) == 0x1F:
                    i.mn = "bl"
                    i.target = (pc + off) & 0xFFFFFFFF
                    i.target_thumb = True
                else:
                    i.mn = "blx"
                    i.target = (pc + off) & 0xFFFFFFFC
                    i.target_thumb = False
                return i
            return self._undef(i)
        return self._undef(i)
