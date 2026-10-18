#!/usr/bin/env python3
"""Print the object size of IOKit classes from this Mac's kernel collections.

usage: scripts/kc-classsize.py [-v] [CLASS ...]   (default: the classes native Wi-Fi subclasses)
       -v  also print the segments, the hits per class and the code around them

Read-only, needs no root. Native Wi-Fi (docs/native-wifi-research.md §8) subclasses Apple's
IO80211/Skywalk classes through reverse-engineered headers. A vtable check (kcvtab.py) shows the
method slots; this shows the sizes, which decide where a subclass's own fields start. Every
class registers itself with OSMetaClass::OSMetaClass(this, "Name", &Super::gMetaClass, sizeof);
on x86_64 that call loads the name with `lea rsi,[rip+...]` and the size with `mov ecx,imm32`.
The script finds the name string, every instruction that loads it into rsi, and the size next
to it. A hit counts as confirmed when a call to OSMetaClass's constructor follows."""
import mmap, re, struct, subprocess, sys

KCS = ['/System/Library/KernelCollections/BootKernelExtensions.kc',
       '/System/Library/KernelCollections/SystemKernelExtensions.kc']

# Sizes the Tahoe headers of laobamac/itlwm 738d4c6 (include/Airport/macOS26/) and
# MacKernelSDK 3f75008 imply, computed with clang. A mismatch means those headers are wrong
# for this macOS build.
EXPECT = {
    'OSObject': 0x10, 'IOService': 0x88, 'IOWorkLoop': 0x48,
    'IONetworkController': 0x110, 'IOEthernetController': 0x118,
    'IONetworkInterface': 0x110, 'IOEthernetInterface': 0x148,
    'IOSkywalkInterface': 0xb0, 'IOSkywalkNetworkInterface': 0xe0,
    'IOSkywalkEthernetInterface': 0x120, 'IO80211SkywalkInterface': 0x128,
    'IO80211InfraInterface': 0x130, 'IO80211InfraProtocol': 0x130,
    'IO80211Controller': 0x128, 'IO80211WorkQueue': 0x50,
}

LEA_RSI, LEA_RDX, MOV_RDX = b'\x48\x8d\x35', b'\x48\x8d\x15', b'\x48\x8b\x15'
MOV_ECX, CALL, JMP, JMP_IND = 0xb9, 0xe8, 0xe9, b'\xff\x25'


class KC:
    def __init__(self, path):
        self.path = path
        f = open(path, 'rb')
        self.m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        self.segs, self.entries = [], []
        for cmd, p in self.cmds(0):
            if cmd == 0x19:                                 # LC_SEGMENT_64
                name = self.m[p + 8:p + 24].rstrip(b'\0').decode()
                va, vs, fo, fs, _maxp, initp = struct.unpack_from('<QQQQii', self.m, p + 24)
                self.segs.append((name, va, fo, fs, initp))
            elif cmd == 0x80000035:                         # LC_FILESET_ENTRY
                self.entries.append(struct.unpack_from('<Q', self.m, p + 16)[0])
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
        for fileoff in self.entries:
            for cmd, p in self.cmds(fileoff):
                if cmd != 0x2:                              # LC_SYMTAB
                    continue
                symoff, nsyms, stroff, _ss = struct.unpack_from('<IIII', self.m, p + 8)
                for i in range(nsyms):
                    strx, typ, _sect, _desc, value = struct.unpack_from('<IBBHQ', self.m, symoff + 16 * i)
                    if typ & 0xe0 or (typ & 0x0e) != 0x0e:  # debug entries, undefined
                        continue
                    end = self.m.find(b'\0', stroff + strx)
                    yield value, self.m[stroff + strx:end].decode(errors='replace')


class Space:
    """All collections together: symbols by address, and pointers between collections."""
    def __init__(self, kcs):
        self.kcs, self.syms = kcs, {}
        for kc in kcs:
            for value, name in kc.symbols():
                self.syms.setdefault(value, name)

    def at(self, va):
        for kc in self.kcs:
            o = kc.off(va)
            if o is not None:
                return kc, o
        return None, None

    def ptr(self, va):
        """The pointer stored at va (a GOT slot), decoding a kernel-cache chained pointer."""
        kc, o = self.at(va)
        if kc is None:
            return None
        raw = struct.unpack_from('<Q', kc.m, o)[0]
        if raw in self.syms:
            return raw
        level = (raw >> 30) & 3                 # dyld_chained_ptr_64_kernel_cache_rebase
        if level < len(self.kcs):
            return self.kcs[level].base + (raw & 0x3fffffff)
        return None

    def name(self, va, follow=True):
        """Symbol at va; through one `jmp [rip+GOT]` stub if va is a stub."""
        if va in self.syms:
            return self.syms[va]
        kc, o = self.at(va)
        if follow and kc is not None and kc.m[o:o + 2] == JMP_IND:
            t = self.ptr(va + 6 + rel32(kc.m, o + 2))
            return self.syms.get(t) if t is not None else None
        return None


def demangle(names):
    # Mach-O symbols carry an extra leading underscore that only Apple's c++filt strips
    plain = [n[1:] if n.startswith('__Z') else n for n in names]
    try:
        out = subprocess.run(['c++filt'], input='\n'.join(plain), capture_output=True,
                             text=True).stdout.split('\n')
        return out[:len(names)] if len(out) >= len(names) else names
    except OSError:
        return names


def rel32(m, at):
    return struct.unpack_from('<i', m, at)[0]


def main():
    args = sys.argv[1:]
    verbose = '-v' in args
    classes = [a for a in args if a != '-v'] or list(EXPECT)
    kcs = [KC(p) for p in KCS]
    space = Space(kcs)

    found = {c: [] for c in classes}
    nstr = {c: 0 for c in classes}
    for kc in kcs:
        m = kc.m
        if verbose:
            print('# %s' % kc.path)
            for name, va, fo, fs, prot in kc.segs:
                print('#   %-16s va 0x%x fileoff 0x%x size 0x%x prot %d' % (name, va, fo, fs, prot))
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

    if verbose:
        for c in classes:
            print('# %-28s %d name strings, %d `lea rsi` to them' % (c, nstr[c], len(found[c])))
            for h in found[c][:3]:
                kc, o = space.at(h['va'])
                print('#   at 0x%x: %s' % (h['va'], kc.m[o - 24:o + 40].hex(' ')))
                print('#     size %s super %s call %s' % (fmt(h['size']), h['super'], h['call']))

    names = []
    for hits in found.values():
        for h in hits:
            names += [h['call'] or '', h['super'] or '']
    dem = dict(zip(names, demangle(names)))

    print('%-28s %-8s %-8s %s' % ('class', 'size', 'expect', 'super class / notes'))
    bad = 0
    for c in classes:
        hits = [h for h in found[c] if h['size'] is not None]
        conf = [h for h in hits if h['call'] and h['call'].startswith('__ZN11OSMetaClassC')]
        use = conf or hits
        sizes = sorted({h['size'] for h in use})
        exp = EXPECT.get(c)
        if not use:
            print('%-28s %-8s %-8s no constructor call found' % (c, '?', fmt(exp)))
            bad += 1
            continue
        sup = {dem.get(h['super'], h['super']) for h in use if h['super']}
        note = ', '.join(sorted(s.replace('::gMetaClass', '') for s in sup)) or '?'
        if not conf:
            note += '  (unconfirmed: no OSMetaClass constructor call next to it)'
        if len(sizes) > 1:
            note += '  (several candidates)'
        st = '' if exp is None else ('OK' if sizes == [exp] else 'DIFF')
        bad += st == 'DIFF'
        print('%-28s %-8s %-8s %s %s' % (c, '/'.join(fmt(s) for s in sizes), fmt(exp), note, st))
    sys.exit(1 if bad else 0)


def fmt(v):
    return '-' if v is None else '0x%x' % v


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


if __name__ == '__main__':
    main()
