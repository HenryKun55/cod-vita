"""Control-flow recovery for the game's modules (the exe and its DLLs).

Finds function entry points (direct call targets, code pointers stored in data or
used as immediates), decodes each function by recursive descent, and resolves
MSVC switch jump tables. The result drives the C code generator.
"""
import struct

import capstone
from capstone import x86 as X

from pe import PE

JCC = {
    'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
    'jl', 'jge', 'jle', 'jg', 'jecxz', 'loop', 'loope', 'loopne',
}


class Insn:
    __slots__ = ('addr', 'size', 'mnem', 'ops', 'prefix', 'raw', 'text')

    def __init__(self, ci):
        self.addr = ci.address
        self.size = ci.size
        self.mnem = ci.mnemonic
        self.ops = list(ci.operands)
        self.prefix = list(ci.prefix)
        self.raw = ci
        self.text = '%s %s' % (ci.mnemonic, ci.op_str)


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}          # addr -> Insn
        self.block_starts = {entry}
        self.jump_tables = {}    # jmp insn addr -> [targets]
        self.tail_calls = set()  # addrs of jmp insns leaving the function
        self.bad = None          # reason if decoding failed

    def end(self):
        last = max(self.insns)
        return last + self.insns[last].size


class Analyzer:
    def __init__(self, pe):
        self.pe = PE(pe) if isinstance(pe, str) else pe
        # Where analysis starts: the entry point and the named exports (a DLL's
        # dllEntry/vmMain are reached only through GetProcAddress).
        self.seeds = {self.pe.entry} | {v for v in self.pe.exports.values() if self.in_text_va(v)}
        self.text = self.pe.section('.text')
        self.text_lo = self.text.va
        self.text_hi = self.text.va + self.text.vsize
        self.code = self.pe.read(self.text_lo, self.text.vsize)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self._cache = {}
        self.strong = set()
        self.noreturn_imports = {'ExitProcess', 'TerminateProcess', 'RaiseException', 'exit', '_exit',
                                 '_CxxThrowException', '?terminate@@YAXXZ'}
        self.noreturn = set()
        # Import stubs (`jmp [iat]`) of the no-return imports never return either.
        nr_slots = {s for s, (d, n) in self.pe.imports.items() if n in self.noreturn_imports}
        for i in range(len(self.code) - 6):
            if self.code[i] == 0xFF and self.code[i + 1] == 0x25 \
                    and struct.unpack_from('<I', self.code, i + 2)[0] in nr_slots:
                self.noreturn.add(self.text_lo + i)

    def in_text_va(self, a):
        t = self.pe.section('.text')
        return t.va <= a < t.va + t.vsize

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def decode(self, a):
        if a in self._cache:
            return self._cache[a]
        off = a - self.text_lo
        ci = next(self.md.disasm(self.code[off:off + 16], a), None)
        ins = Insn(ci) if ci else None
        self._cache[a] = ins
        return ins

    # ------------------------------------------------------------------ helpers
    def direct_target(self, ins):
        op = ins.ops[0] if ins.ops else None
        if op is not None and op.type == X.X86_OP_IMM:
            return op.imm & 0xFFFFFFFF
        return None

    def import_of(self, ins):
        """If ins is `call/jmp [iat_slot]`, return (dll, name)."""
        if not ins.ops:
            return None
        op = ins.ops[0]
        if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            return self.pe.imports.get(op.mem.disp & 0xFFFFFFFF)
        return None

    def find_jump_table(self, fn, ins):
        """`jmp [reg*4 + table]`: return list of targets or None."""
        op = ins.ops[0]
        if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            return None
        table = op.mem.disp & 0xFFFFFFFF
        prev = self._preceding(fn, ins.addr, 1)
        if prev and prev[0].mnem == 'neg' and prev[0].ops[0].type == X.X86_OP_REG \
                and prev[0].ops[0].reg == op.mem.index:
            # `neg idx; jmp [idx*4 + table]`: entries run backwards from the table base.
            count = None
            earlier = sorted((a for a in fn.insns if a < prev[0].addr), reverse=True)[:12]
            for p in (fn.insns[a] for a in earlier):
                if p.mnem == 'cmp' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_IMM \
                        and p.ops[0].reg == op.mem.index:
                    count = p.ops[1].imm & 0xFFFFFFFF
                    break
            if count is None or count > 256:
                return None
            back = [self.pe.u32(table - 4 * k) for k in range(count)]
            return sorted({v for v in back if v is not None and self.in_text(v)}) or None
        count = self._table_bound(fn, ins, op.mem.index)
        targets = []
        a = table
        limit = count if count is not None else 1024
        for _ in range(limit):
            v = self.pe.u32(a)
            a += 4
            if v is None or not self.in_text(v):
                if count is None:
                    break
                continue    # unreachable slot (e.g. memcpy's case 0)
            if count is None and targets and a - 4 in fn.insns:
                break
            targets.append(v)
        return sorted(set(targets)) or None

    def _table_bound(self, fn, jmp, reg):
        """Walk back a few instructions for `cmp reg, N` + `ja` to size the table.

        Also handles the two-level form `movzx reg, byte [reg2 + idx_table]`, where the
        table size is (max byte in the index table) + 1.
        """
        back = self._preceding(fn, jmp.addr, 6)
        for p in back:
            if p.mnem == 'movzx' and p.ops[0].type == X.X86_OP_REG and p.ops[0].reg == reg \
                    and p.ops[1].type == X.X86_OP_MEM and p.ops[1].size == 1:
                idx_table = p.ops[1].mem.disp & 0xFFFFFFFF
                n = self._table_bound(fn, p, p.ops[1].mem.index)
                if n is None:
                    return None
                return max(self.pe.read(idx_table, n)) + 1
            if p.mnem == 'cmp' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_IMM:
                if self._same_reg(p.ops[0].reg, reg):
                    return (p.ops[1].imm & 0xFFFFFFFF) + 1
            if p.mnem == 'and' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_IMM:
                if self._same_reg(p.ops[0].reg, reg):
                    mask = p.ops[1].imm & 0xFFFFFFFF
                    if mask < 0x1000:
                        return mask + 1
            if p.mnem == 'mov' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_REG \
                    and p.ops[0].reg == reg:
                reg = p.ops[1].reg
        return None

    def _same_reg(self, a, b):
        if not a or not b:
            return False
        return self.md.reg_name(a)[-2:] == self.md.reg_name(b)[-2:]

    def _preceding(self, fn, addr, n):
        """Instructions before addr in straight-line order (best effort)."""
        out = []
        cands = sorted((a for a in fn.insns if a < addr), reverse=True)
        cur = addr
        for a in cands:
            if a + fn.insns[a].size != cur:
                break
            out.append(fn.insns[a])
            cur = a
            if len(out) >= n:
                break
        return out

    # --------------------------------------------------------------- discovery
    def analyze_function(self, entry, known_entries):
        fn = Function(entry)
        work = [entry]
        seen = set()
        while work:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            while True:
                if not self.in_text(a):
                    fn.bad = 'left .text at %08x' % a
                    return fn
                ins = self.decode(a)
                if ins is None:
                    fn.bad = 'undecodable at %08x' % a
                    return fn
                if a in fn.insns:
                    # Straight-line code meeting code decoded before (overlapping
                    # decodings converge mid-block): it continues at a label there.
                    fn.block_starts.add(a)
                    break
                if a != entry and a in self.strong and a in known_entries and a not in fn.block_starts:
                    # Falling into another function: treat as tail call.
                    fn.tail_calls.add(a)
                    break
                fn.insns[a] = ins
                m = ins.mnem
                nxt = a + ins.size
                if m in ('ret', 'retf', 'iretd'):
                    break
                if m == 'int3' or m == 'hlt':
                    break
                if m in JCC:
                    t = self.direct_target(ins)
                    fn.block_starts.update((t, nxt))
                    work.append(t)
                    a = nxt
                    continue
                if m == 'jmp':
                    t = self.direct_target(ins)
                    if t is not None:
                        if t in known_entries and t in self.strong and t != entry:
                            fn.tail_calls.add(a)
                        else:
                            fn.block_starts.add(t)
                            work.append(t)
                        break
                    if self.import_of(ins):
                        fn.tail_calls.add(a)
                        break
                    tab = self.find_jump_table(fn, ins)
                    if tab:
                        fn.jump_tables[a] = tab
                        fn.block_starts.update(tab)
                        work.extend(tab)
                    else:
                        fn.tail_calls.add(a)   # computed jump: dispatched at runtime
                    break
                if m == 'call':
                    imp = self.import_of(ins)
                    if imp and imp[1] in self.noreturn_imports:
                        break
                    t = self.direct_target(ins)
                    if t is not None and t in self.noreturn:
                        break
                a = nxt
        return fn

    def code_pointers_in_data(self):
        """Aligned dwords in .rdata/.data that point into .text."""
        ptrs = set()
        for s in self.pe.sections:
            if s.name not in ('.rdata', '.data'):
                continue
            raw = self.pe.data[s.raw_off:s.raw_off + s.raw_size]
            for off in range(0, len(raw) - 3, 4):
                v = struct.unpack_from('<I', raw, off)[0]
                if self.in_text(v):
                    ptrs.add(v)
        return ptrs

    def run(self):
        banned = set()
        while True:
            funcs, entries = self._run_once(banned)
            bad = {e for e, f in funcs.items() if f.bad and e not in self.seeds}
            if not bad:
                break
            banned |= bad
        self.eh_scan(funcs)
        inner = {a for f in funcs.values() for a in f.insns if a != f.entry}
        self.alt_entries = self._alt_entries(funcs) | {a for a in self.eh_conts if a in inner and a not in funcs}
        for a in sorted(self.alt_entries):
            funcs[a] = self.analyze_function(a, entries)
        return funcs, entries

    # ------------------------------------------------------- C++ exceptions
    def eh_scan(self, funcs):
        """MSVC C++ exception handling data: the per-function handler stubs
        (`mov eax, FuncInfo; jmp __CxxFrameHandler`), the functions with try
        blocks (they push such a stub in their prologue) and the continuation
        addresses their catch funclets return (`mov eax, cont; ret`)."""
        self.eh_stubs = {}          # stub address -> FuncInfo address
        self.eh_try_funcs = set()   # entries of functions with try blocks
        self.eh_conts = set()
        cfh = {s for s, (d, n) in self.pe.imports.items() if n == '__CxxFrameHandler'}
        code = self.code
        lo = self.text_lo
        jmp_cfh = set()
        if cfh:
            for i in range(len(code) - 6):
                if code[i] == 0xFF and code[i + 1] == 0x25 and struct.unpack_from('<I', code, i + 2)[0] in cfh:
                    jmp_cfh.add(lo + i)
        else:
            # Statically linked C runtime: __CxxFrameHandler is the common target of
            # the handler stubs whose FuncInfo carries the magic.
            counts = {}
            for i in range(len(code) - 10):
                if code[i] == 0xB8 and code[i + 5] == 0xE9:
                    fi = struct.unpack_from('<I', code, i + 1)[0]
                    if self.pe.u32(fi) == 0x19930520:
                        tgt = lo + i + 10 + struct.unpack_from('<i', code, i + 6)[0]
                        counts[tgt] = counts.get(tgt, 0) + 1
            if not counts:
                return
            jmp_cfh.add(max(counts, key=counts.get))
        for i in range(len(code) - 10):
            if code[i] == 0xB8 and code[i + 5] == 0xE9:
                tgt = lo + i + 10 + struct.unpack_from('<i', code, i + 6)[0]
                fi = struct.unpack_from('<I', code, i + 1)[0]
                if tgt in jmp_cfh and self.pe.u32(fi) == 0x19930520:
                    self.eh_stubs[lo + i] = fi
        with_try = {st for st, fi in self.eh_stubs.items() if self.pe.u32(fi + 12)}
        for f in funcs.values():
            for ins in f.insns.values():
                if ins.mnem == 'push' and ins.ops and ins.ops[0].type == X.X86_OP_IMM \
                        and (ins.ops[0].imm & 0xFFFFFFFF) in with_try:
                    self.eh_try_funcs.add(f.entry)
        for st in with_try:
            fi = self.eh_stubs[st]
            ntb, tbm = self.pe.u32(fi + 12), self.pe.u32(fi + 16)
            for k in range(ntb):
                nc, ha = self.pe.u32(tbm + 20 * k + 12), self.pe.u32(tbm + 20 * k + 16)
                for j in range(nc):
                    h = self.pe.u32(ha + 16 * j + 12)
                    self.eh_conts |= self._funclet_conts(h)

    def _funclet_conts(self, a):
        """Continuation addresses a catch funclet returns in eax."""
        out = set()
        last_mov = None
        for _ in range(400):
            if not self.in_text(a):
                break
            ins = self.decode(a)
            if ins is None:
                break
            if ins.mnem == 'mov' and len(ins.ops) == 2 and ins.ops[0].type == X.X86_OP_REG \
                    and self.md.reg_name(ins.ops[0].reg) == 'eax' and ins.ops[1].type == X.X86_OP_IMM:
                last_mov = ins.ops[1].imm & 0xFFFFFFFF
            if ins.mnem == 'ret':
                if last_mov is not None and self.in_text(last_mov):
                    out.add(last_mov)
                break
            if ins.mnem in ('jmp', 'int3', 'nop'):
                break
            if ins.mnem == 'call':
                imp = self.import_of(ins)
                t = self.direct_target(ins)
                if (imp and imp[1] in self.noreturn_imports) or (t is not None and t in self.noreturn):
                    break
            a += ins.size
        return out

    def _alt_entries(self, funcs):
        """Data-referenced labels inside functions (SEH handlers, etc.): they get their
        own C entry so runtime dispatch can reach them."""
        inner = {}
        for fn in funcs.values():
            for a in fn.insns:
                if a != fn.entry:
                    inner[a] = fn
        table_targets = {t for f in funcs.values() for tab in f.jump_tables.values() for t in tab}
        # A table target is still an alt entry when its function also has computed
        # jumps we could not resolve: those reach it through runtime dispatch.
        unresolved = {id(f) for f in funcs.values() if self.has_unresolved_jump(f)}
        return {p for p in self.code_pointers_in_data() | self.code_pointers_in_text_data(funcs)
                if p in inner and p not in funcs and (p not in table_targets or id(inner[p]) in unresolved)}

    def has_unresolved_jump(self, fn):
        for a, ins in fn.insns.items():
            if ins.mnem == 'jmp' and self.direct_target(ins) is None and not self.import_of(ins) \
                    and a not in fn.jump_tables:
                return True
        return False

    def code_pointers_in_text_data(self, funcs):
        """Aligned dwords inside .text that are not instructions (jump tables placed
        after their function) and point into .text."""
        covered = bytearray(self.text.vsize)
        for fn in funcs.values():
            for a, ins in fn.insns.items():
                covered[a - self.text_lo:a - self.text_lo + ins.size] = b'\x01' * ins.size
        ptrs = set()
        for off in range(0, self.text.vsize - 3, 4):
            if covered[off] or covered[off + 3]:
                continue
            v = struct.unpack_from('<I', self.code, off)[0]
            if self.in_text(v):
                ptrs.add(v)
        return ptrs

    def _run_once(self, banned):
        entries = set(self.seeds)
        # Entries that end the function falling or jumping into them: real starts
        # (direct call targets, the entry point and exports, aligned starts after padding).
        # Pointers found in data or immediates may be mid-function labels; cutting
        # there would lose the flags and registers live across the cut.
        self.strong = set(self.seeds)
        data_ptrs = self.code_pointers_in_data()
        funcs = {}
        imm_ptrs = set()
        work = sorted(self.seeds)
        from_data = set()
        # Pass 1: direct call graph from the entry point.
        while work:
            e = work.pop()
            if e in funcs:
                continue
            fn = self.analyze_function(e, entries)
            if not fn.bad and e in from_data and self.looks_like_data(fn):
                fn.bad = 'looks like data'
            funcs[e] = fn
            for ins in fn.insns.values():
                if ins.mnem == 'call':
                    t = self.direct_target(ins)
                    if t is not None and self.in_text(t):
                        self.strong.add(t)
                    if t is not None and self.in_text(t) and t not in entries:
                        entries.add(t)
                        work.append(t)
                for op in ins.ops:
                    if op.type == X.X86_OP_IMM and ins.mnem not in JCC and ins.mnem not in ('call', 'jmp'):
                        v = op.imm & 0xFFFFFFFF
                        if self.in_text(v):
                            imm_ptrs.add(v)
            if not work:
                # Pass 2: pointers taken as immediates, then pointers stored in data.
                for p in sorted(imm_ptrs | data_ptrs):
                    if p in entries or p in funcs or p in banned:
                        continue
                    if not self._plausible_entry(p, funcs):
                        continue
                    entries.add(p)
                    work.append(p)
                    from_data.add(p)
                imm_ptrs = set()
                data_ptrs = set()
                if not work:
                    # Pass 3: function starts in the code no one reached (aligned,
                    # after padding or a return), tried one by one.
                    for p in self._gap_candidates(funcs, entries, banned):
                        fn = self.analyze_function(p, entries)
                        if fn.bad or self.looks_like_data(fn):
                            banned.add(p)
                            continue
                        entries.add(p)
                        self.strong.add(p)
                        work.append(p)
        return funcs, entries

    def _gap_candidates(self, funcs, entries, banned):
        covered = bytearray(self.text.vsize)
        for fn in funcs.values():
            if fn.bad:
                continue
            for a, ins in fn.insns.items():
                covered[a - self.text_lo:a - self.text_lo + ins.size] = b'\x01' * ins.size
        code = self.code
        out = []
        for off in range(16, self.text.vsize - 16, 16):
            if covered[off] or code[off] in (0x00, 0x90, 0xCC):
                continue
            a = self.text_lo + off
            if a in entries or a in funcs or a in banned:
                continue
            prev = code[off - 1]
            if prev in (0x90, 0xCC, 0xC3) or (code[off - 3] == 0xC2 and covered[off - 3]):
                out.append(a)
        return out

    SUSPICIOUS = {'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'les', 'lds', 'lcall', 'ljmp', 'arpl',
                  'hlt', 'cli', 'sti', 'into', 'iretd', 'retf', 'bound', 'daa', 'das', 'aaa', 'aas', 'aam',
                  'aad', 'salc', 'sldt', 'wbinvd', 'int1', 'enter'}

    def looks_like_data(self, fn):
        for ins in fn.insns.values():
            if ins.mnem in self.SUSPICIOUS:
                return True
            if ins.mnem == 'int' or ins.raw.bytes[:2] == b'\0\0':
                return True
            if ins.mnem in ('push', 'pop') and ins.ops and ins.ops[0].type == X.X86_OP_REG \
                    and self.md.reg_name(ins.ops[0].reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return True
            if ins.mnem == 'mov' and any(o.type == X.X86_OP_REG and self.md.reg_name(o.reg) in
                                         ('cs', 'ds', 'es', 'ss', 'fs', 'gs') for o in ins.ops):
                return True
            if ins.mnem.startswith('fcmov') or ins.mnem == 'fstpnce':
                return True
        return False

    def _plausible_entry(self, a, funcs):
        off = a - self.text_lo
        # Inside the immediate of a `ret N`: data that happens to point there.
        if off >= 2 and (self.code[off - 1] == 0xC2 or self.code[off - 2] == 0xC2):
            return False
        # A 16-byte aligned start right after padding is a function even when a
        # bogus entry decoded through the padding into it.
        after_pad = a % 16 == 0 and off >= 1 and self.code[off - 1] in (0x90, 0xCC, 0xC3)
        # Pointers that land inside an already decoded instruction stream are
        # jump-table targets or mid-function labels, not entries.
        if not after_pad:
            for fn in funcs.values():
                if a in fn.insns and a != fn.entry:
                    return False
        ins = self.decode(a)
        return ins is not None and ins.mnem not in ('int3', 'nop')


class MultiImage:
    """The modules' PE images seen as one address space (reads by guest address)."""

    def __init__(self, pes):
        self.pes = pes
        self.imports = {}
        for p in pes:
            self.imports.update(p.imports)
        self.sections = [s for p in pes for s in p.sections]
        self.entry = pes[0].entry

    def owner(self, va):
        for p in self.pes:
            if p.section_at(va):
                return p
        return None

    def section_at(self, va):
        p = self.owner(va)
        return p.section_at(va) if p else None

    def read(self, va, n):
        p = self.owner(va)
        return p.read(va, n) if p else None

    def u32(self, va):
        p = self.owner(va)
        return p.u32(va) if p else None

    def cstr(self, va):
        return self.owner(va).cstr(va)


class MultiAnalyzer:
    """Several modules (the exe and the DLLs it loads), each analysed on its own
    (their code only meets through function pointers, which run-time dispatch
    resolves), presented to the generator as one analyzer."""

    def __init__(self, pes):
        self.subs = [Analyzer(p) for p in pes]
        self.pe = MultiImage(pes)
        self.md = self.subs[0].md
        self.noreturn = set()
        for a in self.subs:
            self.noreturn |= a.noreturn
        self.noreturn_imports = self.subs[0].noreturn_imports

    def sub(self, a):
        for s in self.subs:
            if s.text_lo <= a < s.text_hi:
                return s
        return self.subs[0]

    def in_text(self, a):
        return any(s.in_text(a) for s in self.subs)

    def decode(self, a):
        return self.sub(a).decode(a)

    def direct_target(self, ins):
        return self.subs[0].direct_target(ins)

    def import_of(self, ins):
        return self.sub(ins.addr).import_of(ins)

    def run(self):
        funcs, entries = {}, set()
        self.eh_stubs, self.eh_try_funcs, self.eh_conts, self.alt_entries = {}, set(), set(), set()
        for a in self.subs:
            f, e = a.run()
            funcs.update(f)
            entries |= e
            self.eh_stubs.update(a.eh_stubs)
            self.eh_try_funcs |= a.eh_try_funcs
            self.eh_conts |= a.eh_conts
            self.alt_entries |= a.alt_entries
        return funcs, entries


def load_modules(specs):
    """PE images from 'path[@base]' strings (base in hex): the exe first."""
    out = []
    for spec in specs:
        path, _, base = spec.partition('@')
        out.append(PE(path, int(base, 16) if base else None))
    return out


def main():
    import sys
    an = MultiAnalyzer(load_modules(sys.argv[1:]))
    funcs, entries = an.run()
    bad = [f for f in funcs.values() if f.bad]
    covered = set()
    for f in funcs.values():
        if f.bad:
            continue
        for a, ins in f.insns.items():
            covered.update(range(a, a + ins.size))
    print('functions:', len(funcs), 'bad:', len(bad))
    tsize = sum(a.text.vsize for a in an.subs)
    print('text bytes covered: %d / %d (%.1f%%)' % (len(covered), tsize, 100.0 * len(covered) / tsize))
    print('jump tables:', sum(len(f.jump_tables) for f in funcs.values()))
    print('computed jumps:', sum(1 for f in funcs.values() for a in f.tail_calls
                                 if f.insns.get(a) and f.insns[a].mnem == 'jmp'
                                 and an.direct_target(f.insns[a]) is None and not an.import_of(f.insns[a])))
    for f in bad[:40]:
        print('  bad %08x: %s' % (f.entry, f.bad))


if __name__ == '__main__':
    main()
