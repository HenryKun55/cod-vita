"""Minimal PE32 loader for the recompiler: sections, imports, and address helpers."""
import struct


class Section:
    def __init__(self, name, va, vsize, raw_off, raw_size, chars):
        self.name = name
        self.va = va
        self.vsize = vsize
        self.raw_off = raw_off
        self.raw_size = raw_size
        self.chars = chars

    @property
    def end(self):
        return self.va + max(self.vsize, self.raw_size)


class PE:
    def __init__(self, path, base=None):
        """Load a PE32 image. With `base`, a module with relocations is rebased there:
        every address the class reports and every relocated dword it reads use the new
        base, so the analysis sees the image exactly as the loader will map it."""
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        pe_off = struct.unpack_from('<I', d, 0x3C)[0]
        assert d[pe_off:pe_off + 4] == b'PE\0\0'
        coff = pe_off + 4
        nsec, = struct.unpack_from('<H', d, coff + 2)
        opt_size, = struct.unpack_from('<H', d, coff + 16)
        opt = coff + 20
        self.entry_rva, = struct.unpack_from('<I', d, opt + 16)
        self.image_base, = struct.unpack_from('<I', d, opt + 28)
        self.size_of_image, = struct.unpack_from('<I', d, opt + 56)
        ndirs, = struct.unpack_from('<I', d, opt + 92)
        self.dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * i) for i in range(ndirs)]
        self.sections = []
        sh = opt + opt_size
        for i in range(nsec):
            name = d[sh:sh + 8].rstrip(b'\0').decode('ascii')
            vsize, va, raw_size, raw_off = struct.unpack_from('<IIII', d, sh + 8)
            chars, = struct.unpack_from('<I', d, sh + 36)
            self.sections.append(Section(name, self.image_base + va, vsize, raw_off, raw_size, chars))
            sh += 40
        self.preferred_base = self.image_base
        self.relocs = self._parse_relocs()
        if base is not None and base != self.image_base:
            self._rebase(base)
        self.entry = self.image_base + self.entry_rva
        self.imports = self._parse_imports()
        self.exports = self._parse_exports()

    def _rva_to_off(self, rva):
        for s in self.sections:
            va = s.va - self.image_base
            if va <= rva < va + s.raw_size:
                return s.raw_off + rva - va
        return None

    def _parse_relocs(self):
        """RVAs of the HIGHLOW (type 3) fixups; other types are rejected."""
        if len(self.dirs) <= 5 or not self.dirs[5][0]:
            return []
        rva, size = self.dirs[5]
        off = self._rva_to_off(rva)
        out = []
        end = off + size
        while off < end:
            page, block = struct.unpack_from('<II', self.data, off)
            if block < 8:
                break
            for i in range((block - 8) // 2):
                e, = struct.unpack_from('<H', self.data, off + 8 + 2 * i)
                kind, ofs = e >> 12, e & 0xFFF
                if kind == 3:
                    out.append(page + ofs)
                elif kind != 0:
                    raise ValueError('%s: unsupported relocation type %d' % (self.path, kind))
            off += block
        return out

    def _rebase(self, base):
        if not self.relocs:
            raise ValueError('%s has no relocations; it cannot move to %08X' % (self.path, base))
        delta = (base - self.image_base) & 0xFFFFFFFF
        d = bytearray(self.data)
        for rva in self.relocs:
            off = self._rva_to_off(rva)
            if off is None:
                continue        # in bss: nothing stored there yet
            v, = struct.unpack_from('<I', d, off)
            struct.pack_into('<I', d, off, (v + delta) & 0xFFFFFFFF)
        self.data = bytes(d)
        for s in self.sections:
            s.va = s.va - self.image_base + base
        self.image_base = base

    def _parse_exports(self):
        """{name: va} of the named exports."""
        rva, size = self.dirs[0] if self.dirs else (0, 0)
        if not rva:
            return {}
        b = self.image_base
        nfuncs, nnames, funcs, names, ords = struct.unpack('<IIIII', self.read(b + rva + 20, 20))
        out = {}
        for i in range(nnames):
            name = self.cstr(b + self.u32(b + names + 4 * i))
            ordinal, = struct.unpack('<H', self.read(b + ords + 2 * i, 2))
            out[name] = b + self.u32(b + funcs + 4 * ordinal)
        return out

    def section(self, name):
        return next(s for s in self.sections if s.name == name)

    def section_at(self, va):
        for s in self.sections:
            if s.va <= va < s.end:
                return s
        return None

    def read(self, va, n):
        s = self.section_at(va)
        if s is None:
            return None
        off = va - s.va
        if off + n > s.raw_size:
            # Bytes beyond raw data are zero-filled (bss).
            raw = self.data[s.raw_off + off:s.raw_off + min(off + n, s.raw_size)] if off < s.raw_size else b''
            return raw + b'\0' * (n - len(raw))
        return self.data[s.raw_off + off:s.raw_off + off + n]

    def u32(self, va):
        b = self.read(va, 4)
        return None if b is None else struct.unpack('<I', b)[0]

    def cstr(self, va):
        out = bytearray()
        while True:
            b = self.read(va, 1)
            if not b or b == b'\0':
                return out.decode('latin-1')
            out += b
            va += 1

    def _parse_imports(self):
        """Return {iat_slot_va: (dll, name_or_ordinal)}."""
        rva, size = self.dirs[1]
        imports = {}
        if not rva:
            return imports
        desc = self.image_base + rva
        while True:
            ilt, _, _, name_rva, iat = struct.unpack('<IIIII', self.read(desc, 20))
            if not name_rva:
                break
            dll = self.cstr(self.image_base + name_rva).lower()
            src = self.image_base + (ilt or iat)
            slot = self.image_base + iat
            while True:
                v = self.u32(src)
                if not v:
                    break
                if v & 0x80000000:
                    name = '#%d' % (v & 0xFFFF)
                else:
                    name = self.cstr(self.image_base + v + 2)
                imports[slot] = (dll, name)
                src += 4
                slot += 4
            desc += 20
        return imports
