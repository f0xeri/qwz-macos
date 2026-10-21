"""Read-only access to this Mac's kernel collections (shared by scripts/kc-*.py).

A kernel collection is one Mach-O of type MH_FILESET: the kernel and every kext of that
collection, each a fileset entry with its own load commands and symbol table. Pointers inside
it are stored as kernel-cache chained fixups (dyld_chained_ptr_64_kernel_cache_rebase), not as
plain addresses; Space.ptr() decodes them. Nothing here needs root or Xcode."""
import mmap, struct, subprocess, sys

KCS = ['/System/Library/KernelCollections/BootKernelExtensions.kc',
       '/System/Library/KernelCollections/SystemKernelExtensions.kc']

JMP_IND = b'\xff\x25'


def rel32(m, at):
    return struct.unpack_from('<i', m, at)[0]


class KC:
    def __init__(self, path):
        self.path = path
        f = open(path, 'rb')
        self.m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        self.segs, self.entries = [], []      # entries: (bundle id, mach header file offset)
        for cmd, p in self.cmds(0):
            if cmd == 0x19:                                 # LC_SEGMENT_64
                name = self.m[p + 8:p + 24].rstrip(b'\0').decode()
                va, vs, fo, fs, _maxp, initp = struct.unpack_from('<QQQQii', self.m, p + 24)
                self.segs.append((name, va, fo, fs, initp))
            elif cmd == 0x80000035:                         # LC_FILESET_ENTRY
                _va, fo, stro = struct.unpack_from('<QQI', self.m, p + 8)
                end = self.m.find(b'\0', p + stro)
                self.entries.append((self.m[p + stro:end].decode(), fo))
        self.base = min(s[1] for s in self.segs)

    def off(self, va):
        for _n, sva, fo, fs, _x in self.segs:
            if sva <= va < sva + fs:
                return fo + va - sva
        return None

    def cmds(self, off):
        magic, _c, _s, _t, ncmds = struct.unpack_from('<IiiII', self.m, off)
        if magic != 0xfeedfacf:
            sys.exit('%s: not a 64-bit Mach-O' % self.path)
        p = off + 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', self.m, p)
            yield cmd, p
            p += size

    def symbols(self):
        """(address, name, bundle id, external) of every defined symbol of every entry."""
        for bundle, fileoff in self.entries:
            for cmd, p in self.cmds(fileoff):
                if cmd != 0x2:                              # LC_SYMTAB
                    continue
                symoff, nsyms, stroff, _ss = struct.unpack_from('<IIII', self.m, p + 8)
                for i in range(nsyms):
                    strx, typ, _sect, _desc, value = struct.unpack_from('<IBBHQ', self.m, symoff + 16 * i)
                    if typ & 0xe0 or (typ & 0x0e) != 0x0e:  # debug entries, undefined
                        continue
                    end = self.m.find(b'\0', stroff + strx)
                    yield value, self.m[stroff + strx:end].decode(errors='replace'), bundle, bool(typ & 1)


class Space:
    """All collections together: symbols by address, and pointers between collections."""
    def __init__(self, kcs):
        self.kcs, self.syms, self.addr, self.owner = kcs, {}, {}, {}
        for kc in kcs:
            for value, name, bundle, _ext in kc.symbols():
                self.syms.setdefault(value, name)
                self.addr.setdefault(name, value)
                self.owner.setdefault(value, bundle)
        self.sorted = sorted(self.syms)

    def at(self, va):
        for kc in self.kcs:
            o = kc.off(va)
            if o is not None:
                return kc, o
        return None, None

    def raw(self, va):
        kc, o = self.at(va)
        return None if kc is None else struct.unpack_from('<Q', kc.m, o)[0]

    def ptr(self, va):
        """The pointer stored at va, decoding a kernel-cache chained pointer."""
        raw = self.raw(va)
        if raw is None or raw == 0:
            return raw
        if raw in self.syms:
            return raw
        level = (raw >> 30) & 3                 # dyld_chained_ptr_64_kernel_cache_rebase
        if level < len(self.kcs):
            return self.kcs[level].base + (raw & 0x3fffffff)
        return None

    def next_symbol(self, va):
        """Address of the first symbol after va."""
        import bisect
        i = bisect.bisect_right(self.sorted, va)
        return self.sorted[i] if i < len(self.sorted) else None

    def name(self, va, follow=True):
        """Symbol at va; through one `jmp [rip+GOT]` stub if va is a stub."""
        if va in self.syms:
            return self.syms[va]
        kc, o = self.at(va)
        if follow and kc is not None and kc.m[o:o + 2] == JMP_IND:
            t = self.ptr(va + 6 + rel32(kc.m, o + 2))
            return self.syms.get(t) if t is not None else None
        return None

    def bytes(self, va, n):
        kc, o = self.at(va)
        return None if kc is None else kc.m[o:o + n]


def demangle(names):
    # Mach-O symbols carry an extra leading underscore that only Apple's c++filt strips
    plain = [n[1:] if n.startswith('__Z') else n for n in names]
    try:
        out = subprocess.run(['c++filt'], input='\n'.join(plain), capture_output=True,
                             text=True).stdout.split('\n')
        return out[:len(names)] if len(out) >= len(names) else names
    except OSError:
        return names


def mangle_class(c):
    return '%d%s' % (len(c), c)


def disasm(space, va, n):
    """Disassemble n bytes at va with llvm-objdump (Command Line Tools). It cannot read a
    fileset, so the bytes go into a minimal MH_OBJECT whose __text sits at va, with the
    symbols of that range. Returns [(address, instruction text)]; call/jmp targets and
    rip-relative loads get '; <symbol>' when the address has one."""
    import os, re, tempfile
    code = space.bytes(va, n)
    if code is None:
        return []
    syms = []
    a = va
    while True:
        if a in space.syms and a < va + n:
            syms.append((a, space.syms[a]))
        a = space.next_symbol(a)
        if a is None or a >= va + n:
            break
    strtab = b'\0'
    nl = b''
    for addr, name in syms:
        nl += struct.pack('<IBBHQ', len(strtab), 0x0e, 1, 0, addr)
        strtab += name.encode() + b'\0'
    ncmds, segsz, symsz = 2, 72 + 80, 24
    off = 32 + segsz + symsz
    symoff = off + ((len(code) + 7) & ~7)
    stroff = symoff + len(nl)
    hdr = struct.pack('<IiiIIIII', 0xfeedfacf, 0x01000007, 3, 1, ncmds, segsz + symsz, 0, 0)
    seg = struct.pack('<II16sQQQQiiII', 0x19, segsz, b'', va, n, off, n, 7, 7, 1, 0)
    sect = struct.pack('<16s16sQQIIIIIIII', b'__text', b'__TEXT', va, n, off, 4, 0, 0,
                       0x80000400, 0, 0, 0)
    st = struct.pack('<IIIIII', 0x2, symsz, symoff, len(syms), stroff, len(strtab))
    body = hdr + seg + sect + st + bytes(code)
    body += b'\0' * (symoff - len(body)) + nl + strtab
    fd, path = tempfile.mkstemp(suffix='.o')
    try:
        os.write(fd, body)
        os.close(fd)
        txt = subprocess.run(['xcrun', 'llvm-objdump', '--macho', '-d', '--no-show-raw-insn',
                              '--print-imm-hex', '--x86-asm-syntax=intel', path], capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    rows = []
    for line in txt.split('\n'):
        mm = re.match(r'([0-9a-f]{8,16}):\s+(.*)$', line)
        if mm:
            rows.append((int(mm.group(1), 16), re.sub(r'\s+', ' ', mm.group(2).strip())))
    res = []
    for k, (addr, ins) in enumerate(rows):
        nxt = rows[k + 1][0] if k + 1 < len(rows) else va + n
        t = None
        j = re.match(r'(call|jmp|j\w+) 0x([0-9a-f]+)$', ins)
        if j:
            t = space.name(int(j.group(2), 16))
        r = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ins)
        if r and t is None:
            tv = nxt + int(r.group(2), 16) * (1 if r.group(1) == '+' else -1)
            t = space.syms.get(tv)
            if t is None and ins.startswith(('mov ', 'call ', 'jmp ')) and 'qword ptr [rip' in ins:
                pv = space.ptr(tv)
                t = space.syms.get(pv) if pv else None
                t = '[' + t + ']' if t else None
            if t is None and ins.startswith('lea '):
                kc, o = space.at(tv)
                if kc is not None:
                    e = kc.m.find(b'\0', o, o + 120)
                    sv = kc.m[o:e] if e > o else b''
                    if len(sv) >= 3 and all(32 <= c < 127 for c in sv):
                        t = '"%s"' % sv.decode()
            if t is None:
                t = '0x%x' % tv
        res.append((addr, ins + ('  ; ' + t if t else '')))
    return res


LEA_RSI, LEA_RDX, MOV_RDX = b'\x48\x8d\x35', b'\x48\x8d\x15', b'\x48\x8b\x15'
MOV_ECX, CALL, JMP = 0xb9, 0xe8, 0xe9


def metaclass_hits(kcs, space, classes):
    """Every OSMetaClass registration of the given classes: each class's constructor calls
    OSMetaClass::OSMetaClass(this, "Name", &Super::gMetaClass, sizeof); on x86_64 that loads
    the name with `lea rsi,[rip+...]` and the size with `mov ecx,imm32`. Returns
    ({class: [hit]}, {class: number of name strings}); see inspect() for a hit."""
    found = {c: [] for c in classes}
    nstr = {c: 0 for c in classes}
    for kc in kcs:
        m = kc.m
        # VA of each class name string ("\0Name\0"). Not only __TEXT: where the
        # strings and the code end up differs between collections, and the
        # protection bits of a collection's segments are not reliable.
        strva = {}
        for name, va, fo, fs, prot in kc.segs:
            if name in ('__LINKEDIT', '__PRELINK_INFO'):
                continue
            for c in classes:
                pat = b'\0' + c.encode() + b'\0'
                at = m.find(pat, fo, fo + fs)
                while at >= 0:
                    strva[va + at + 1 - fo] = c
                    nstr[c] += 1
                    at = m.find(pat, at + 1, fo + fs)
        # every `lea rsi,[rip+disp]` in code that points at one of them
        for name, va, fo, fs, prot in kc.segs:
            if 'TEXT' not in name and not prot & 4:
                continue
            at = m.find(LEA_RSI, fo, fo + fs)
            while at >= 0:
                target = va + (at - fo) + 7 + rel32(m, at + 3)
                c = strva.get(target)
                if c:
                    found[c].append(inspect(kc, at, va - fo, space))
                at = m.find(LEA_RSI, at + 1, fo + fs)
    return found, nstr


def class_sizes(space, found):
    """{class: (sorted sizes, set of super names, confirmed)} from metaclass_hits()."""
    res = {}
    for c, hits in found.items():
        hits = [h for h in hits if h['size'] is not None]
        conf = [h for h in hits if h['call'] and h['call'].startswith('__ZN11OSMetaClassC')]
        use = conf or hits
        res[c] = (sorted({h['size'] for h in use}), {h['super'] for h in use if h['super']}, bool(conf))
    return res


def inspect(kc, at, delta, space):
    """Around the `lea rsi` at file offset at: the size (mov ecx,imm32), the super class
    (lea rdx, or mov rdx from a GOT slot), and the function called next."""
    m = kc.m
    lo, hi = max(at - 40, 0), at + 48
    size = sup = call = None
    best = None
    for i in range(lo, hi):
        if m[i] == MOV_ECX and (best is None or abs(i - at) < abs(best - at)):
            v = struct.unpack_from('<I', m, i + 1)[0]
            if 0x10 <= v <= 0x100000 and v % 8 == 0:
                best, size = i, v
        if sup is None and m[i:i + 3] in (LEA_RDX, MOV_RDX):
            t = i + delta + 7 + rel32(m, i + 3)
            if m[i:i + 3] == MOV_RDX:
                t = space.ptr(t)
            sup = space.name(t, follow=False) if t is not None else None
    for i in range(at + 7, hi):
        if m[i] in (CALL, JMP):
            t = space.name(i + delta + 5 + rel32(m, i + 1))
            if t:
                call = t
                break
    return {'size': size, 'super': sup, 'call': call, 'va': at + delta}
