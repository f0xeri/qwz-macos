#!/usr/bin/env python3
"""Dump the C++ ABI of IOKit classes from this Mac's kernel collections: super class and
vtable slots (what each slot points to, demangled), as a text file the header generator reads.

usage: scripts/kc-abi.py [-o FILE] CLASS ...
       scripts/kc-abi.py --check [CLASS ...]   (default: the classes QwzAirport subclasses)
       scripts/kc-abi.py --returns [CLASS ...] (default: IO80211InfraProtocol, IO80211Controller)

Read-only, needs no root. Native Wi-Fi (docs/native-wifi-research.md §8) subclasses Apple's
IO80211/Skywalk classes, which have no public headers. Our headers (airport/abi/) are generated
from this dump, so a slot can only be where this macOS build has it.

How: the vtable of class C is the symbol __ZTV<C>; its slots start 16 bytes in (offset to top,
RTTI). Each slot is a chained pointer to the method's code; the symbol there names it
(mangled names carry the argument types, not the return type). The super class comes from
C::superClass, a pointer to Super::gMetaClass. A slot whose code is __cxa_pure_virtual is pure:
a subclass must implement it. Its name and argument types are taken from the first subclass in
the collections (usually Apple's own driver, AppleBCMWLAN*) whose vtable has a real method there.

Output, one block per class:
  class C : Super  slots N (super M)  bundle com.apple...
  <index> <kind> <mangled name or 0xADDRESS> | <demangled>
kind: new (first appears in C), over (C overrides the super class's slot), inh (inherited),
pure (pure virtual in C; the name after '|' is the subclass's method, '= Sub::name(...)').

--check compares our headers (airport/abi/, built with the Tahoe defines) with this Mac: for
each class it compiles a probe subclass, reads the probe's vtable from the object file (each
slot's relocation names the method, mangled, so argument types are compared exactly), the
header's names of pure slots from clang's -fdump-vtable-layouts, and sizeof(class). Errors:
a different slot count (our own virtuals would land on Apple's slots), a different size (our
fields would overlap Apple's), a slot whose method differs, a slot the header implements but
the Mac leaves pure. Not errors: a slot the header names _RESERVED... where the Mac has a real
method (the kernel's linker patches a subclass's reserved pads from the parent's vtable), and a
slot pure in the header but implemented on the Mac (we only have to implement it). Exit status 1 on any error. Run it before adding a build to QwzAirport's
allowlist.

--returns checks what our generated stubs (scripts/airport-stubs.py) assume: that every pure
virtual request returns an int-sized IOReturn. Mangled names carry no return type, so it looks
at Apple's own implementation of each pure slot (in AppleBCMWLAN*) and at what it leaves in
eax/al/rax before `ret`, following tail calls and the forwarders that call the same-named
method of AppleBCMWLANCore through its vtable. A slot that returns through `al` (bool) or a
pointer while airport/abi declares an int (or the other way round) is listed: a
kIOReturnUnsupported stub would read as "true" or as a pointer there. 'call' means the value
comes from another call (not followed), 'unknown' that no return was recognized; neither is an
error. Exit status 1 on any mismatch."""
import os, re, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kclib import KCS, KC, Space, demangle, disasm, mangle_class, metaclass_hits, class_sizes  # noqa: E402

PURE = '___cxa_pure_virtual'


def vtable(space, c):
    va = space.addr.get('__ZTV' + mangle_class(c))
    if va is None:
        return None
    end = space.next_symbol(va) or va + 8 * 4096
    slots = []
    for p in range(va + 16, end, 8):
        t = space.ptr(p)
        if not t:
            break
        slots.append(t)
    return slots


def superclass(space, c):
    va = space.addr.get('__ZN%s10superClassE' % mangle_class(c))
    if va is None:
        return None
    t = space.ptr(va)
    n = space.syms.get(t) if t else None
    if not n or not n.endswith('10gMetaClassE'):
        return None
    # __ZN<len><Name>10gMetaClassE
    s = n[4:]
    i = 0
    while s[i].isdigit():
        i += 1
    return s[i:i + int(s[:i])]


def all_supers(space):
    """{class: super class} for every class with a C::superClass symbol."""
    res = {}
    for n in space.addr:
        m = re.match(r'__ZN(\d+)(\w+)10superClassE$', n)
        if m and len(m.group(2)) >= int(m.group(1)):
            c = m.group(2)[:int(m.group(1))]
            s = superclass(space, c)
            if s:
                res[c] = s
    return res


def subclasses(sups, c):
    out = []
    for k in sorted(sups):
        x = k
        while x in sups:
            x = sups[x]
            if x == c:
                out.append(k)
                break
    return out


CHECK_CLASSES = ['IO80211Controller', 'IO80211WorkQueue', 'IOSkywalkInterface',
                 'IOSkywalkNetworkInterface', 'IOSkywalkEthernetInterface',
                 'IO80211SkywalkInterface', 'IO80211InfraInterface', 'IO80211InfraProtocol']
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR_DEFINES = ['-DQWZAIRPORT', '-D__PRIVATE_SPI__', '-DAIRPORT_WCL', '-D__IO80211_TARGET=__MAC_26_0',
               '-DIO80211FAMILY_V2']


def method_key(n):
    """A mangled member name without its class: __ZN17IO80211Controller5startEP9IOService ->
    5startEP9IOService (K for const methods kept). Destructors stay D0Ev/D1Ev."""
    m = re.match(r'__ZN(K?)(\d+)', n or '')
    if not m:
        return n
    rest = n[len(m.group(0)):]
    return m.group(1) + rest[int(m.group(2)):]


def method_name(sig):
    """The bare method name of a demangled or clang-printed signature."""
    m = re.search(r'(~?[A-Za-z_][A-Za-z0-9_]*)\s*\(', sig or '')
    return m.group(1) if m else sig


def macho_object(path):
    """Sections, symbols and relocations of a 64-bit MH_OBJECT (our probe)."""
    d = open(path, 'rb').read()
    _mg, _ct, _st, _ft, ncmds, _sz, _fl, _r = struct.unpack_from('<IiiIIIII', d, 0)
    p, sects, syms = 32, [], []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, p)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', d, p + 64)[0]
            for k in range(nsects):
                q = p + 72 + 80 * k
                sn, sg = d[q:q + 16].rstrip(b'\0').decode(), d[q + 16:q + 32].rstrip(b'\0').decode()
                addr, sz, off, _al, reloff, nreloc = struct.unpack_from('<QQIIII', d, q + 32)
                sects.append({'name': (sg, sn), 'addr': addr, 'size': sz, 'off': off,
                              'reloff': reloff, 'nreloc': nreloc})
        elif cmd == 0x2:
            symoff, nsyms, stroff, _ss = struct.unpack_from('<IIII', d, p + 8)
            for i in range(nsyms):
                strx, typ, sect, _desc, value = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
                e = d.index(b'\0', stroff + strx)
                syms.append((d[stroff + strx:e].decode(), typ, sect, value))
        p += size
    for sc in sects:
        rel = {}
        for i in range(sc['nreloc']):
            addr, info = struct.unpack_from('<iI', d, sc['reloff'] + 8 * i)
            if info >> 27 & 1:                                  # r_extern
                rel[addr] = syms[info & 0xffffff][0]
        sc['rel'] = rel
    return d, sects, syms


def header_layouts(classes):
    """{class: (slots [mangled or PURE], {index: clang signature}, sizeof)} from airport/abi/."""
    sdk = subprocess.run(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], capture_output=True,
                         text=True).stdout.strip()
    kh = sdk + '/System/Library/Frameworks/Kernel.framework/Headers'
    src = ['#include <Airport/Apple80211.h>']
    for c in classes:
        src.append('class QwzProbe_%s : public %s { public: QwzProbe_%s(); };' % (c, c, c))
        src.append('QwzProbe_%s::QwzProbe_%s() : %s((const OSMetaClass *)nullptr) {}' % (c, c, c))
        src.append('extern "C" const unsigned long qwz_sizeof_%s = sizeof(%s);' % (c, c))
    tmp = tempfile.mkdtemp()
    cpp, obj = os.path.join(tmp, 'probe.cpp'), os.path.join(tmp, 'probe.o')
    open(cpp, 'w').write('\n'.join(src) + '\n')
    r = subprocess.run(['clang++', '-x', 'c++', '-arch', 'x86_64', '-mmacosx-version-min=11.0',
                        '-mkernel', '-std=gnu++17', '-fapple-kext', '-fno-rtti', '-fno-exceptions',
                        '-w', '-isysroot', sdk, '-I' + kh, '-DKERNEL', '-DKERNEL_PRIVATE',
                        '-DDRIVER_PRIVATE'] + HDR_DEFINES +
                       ['-I' + os.path.join(ROOT, 'airport/abi'), '-Xclang', '-fdump-vtable-layouts',
                        '-c', cpp, '-o', obj], capture_output=True, text=True)
    if r.returncode:
        sys.exit('probe does not compile:\n' + r.stderr[-3000:])
    # clang's layout dump: names for every slot, including pure ones
    sigs, cur = {}, None
    for line in r.stdout.split('\n'):
        m = re.match(r"(?:Original map|VTable|Vtable) for '(\w+)' \(\d+ entries\)", line)
        if m:
            cur = m.group(1)[len('QwzProbe_'):] if m.group(1).startswith('QwzProbe_') else None
            if cur:
                sigs[cur] = {}
            continue
        m = re.match(r'\s*(\d+) \| (.*)$', line)
        if m and cur:
            sigs[cur][int(m.group(1)) - 2] = m.group(2)
    d, sects, syms = macho_object(obj)
    res = {}
    for c in classes:
        vname = '__ZTV' + mangle_class('QwzProbe_' + c)
        va = [s for s in syms if s[0] == vname][0]
        sc = sects[va[2] - 1]
        later = sorted(s[3] for s in syms if s[2] == va[2] and s[3] > va[3])
        end = later[0] if later else sc['addr'] + sc['size']
        slots = []
        for a in range(va[3] + 16, end, 8):
            n = sc['rel'].get(a - sc['addr'])
            if n is None:
                break
            slots.append(n)
        sz = [s for s in syms if s[0] == '_qwz_sizeof_' + c][0]
        ss = sects[sz[2] - 1]
        size = struct.unpack_from('<Q', d, ss['off'] + sz[3] - ss['addr'])[0]
        res[c] = (slots, sigs.get(c, {}), size)
    return res


def check(classes):
    kcs = [KC(p) for p in KCS]
    space = Space(kcs)
    sups = all_supers(space)
    found, _n = metaclass_hits(kcs, space, classes)
    sizes = class_sizes(space, found)
    hdr = header_layouts(classes)
    build = subprocess.run(['sysctl', '-n', 'kern.osversion'], capture_output=True, text=True).stdout.strip()
    print('kc-abi.py --check: airport/abi/ against macOS %s' % build)
    errors = 0
    for c in classes:
        vt = vtable(space, c)
        hslots, hsigs, hsize = hdr[c]
        msizes = sizes[c][0]
        names = [space.syms.get(t) or '0x%x' % t for t in vt]
        subs = [vtable(space, k) for k in subclasses(sups, c)]
        bad, notes, pads = [], 0, 0
        for i in range(min(len(names), len(hslots))):
            k, h = names[i], hslots[i]
            if k == PURE:
                impl = next((space.syms.get(s[i]) for s in subs
                             if s and i < len(s) and space.syms.get(s[i]) not in (None, PURE)), None)
                if h != PURE:
                    bad.append('%d: Mac pure (%s), header has %s' % (i, impl, h))
                elif impl and method_name(demangle([impl])[0]) != method_name(hsigs.get(i)):
                    bad.append('%d: pure, Mac %s, header %s' % (i, demangle([impl])[0], hsigs.get(i)))
            elif h == PURE:
                if method_name(demangle([k])[0]) != method_name(hsigs.get(i)):
                    bad.append('%d: Mac %s, header pure %s' % (i, demangle([k])[0], hsigs.get(i)))
                else:
                    notes += 1      # Mac has a default, the header makes us implement it
            elif method_key(k) != method_key(h) and '_RESERVED' in h and '_RESERVED' not in k:
                pads += 1           # a reserved pad in the header; kxld patches it from the Mac's
            elif method_key(k) != method_key(h):
                bad.append('%d: Mac %s, header %s' % (i, demangle([k])[0], demangle([h])[0]))
        if len(names) != len(hslots):
            bad.insert(0, 'slot count: Mac %d, header %d' % (len(names), len(hslots)))
        if msizes != [hsize]:
            bad.insert(0, 'size: Mac %s, header 0x%x' % ('/'.join('0x%x' % x for x in msizes) or '?', hsize))
        print('%-28s slots %3d/%-3d size 0x%x/0x%x  %s%s' % (
            c, len(names), len(hslots), msizes[0] if msizes else 0, hsize,
            'OK' if not bad else '%d ERRORS' % len(bad),
            ''.join(['  (%d slots pure in the header only)' % notes if notes else '',
                     '  (%d reserved pads in the header)' % pads if pads else ''])))
        for b in bad[:40]:
            print('    ' + b)
        errors += len(bad)
    sys.exit(1 if errors else 0)


WRITE = re.compile(r'(mov|xor|lea|movzx|movsx|or|and|cmov\w+|sub|add|neg|sbb) (eax|al|rax|ax)\b')
FORWARD = re.compile(r'mov rax, qword ptr \[rax \+ (0x[0-9a-f]+)\]$')


def return_kinds(space, va, name, core, depth=0, seen=None):
    """How the function at va returns: a set of 'ioreturn', 'int', 'bool', 'ptr', 'unknown'."""
    seen = set() if seen is None else seen
    if va in seen or depth > 4:
        return set()
    seen.add(va)
    end = space.next_symbol(va)
    n = min((end or va + 0x600) - va, 0x1000)
    ins = [x[1] for x in disasm(space, va, n)]
    kinds = set()
    for k, x in enumerate(ins):
        if x.startswith('ret'):
            for y in reversed(ins[max(0, k - 12):k]):
                if y.startswith('call'):
                    kinds.add('call')      # passes on the callee's result
                    break
                if y.startswith('set') and re.search(r'\bal\b', y):
                    kinds.add('bool')
                    break
                if WRITE.match(y):
                    dst = y.split()[1].rstrip(',')
                    if dst == 'al':
                        kinds.add('bool')
                    elif re.search(r'0xe00002|0xe0003|0xe0082', y):
                        kinds.add('ioreturn')
                    elif dst == 'rax' and not y.startswith(('xor', 'sbb')):
                        kinds.add('ptr')
                    else:
                        kinds.add('int')
                    break
            else:
                kinds.add('unknown')
        elif x == 'jmp rax':
            # forwarder: this->field->object->vtable[off](...), the same method of the core
            f = next((FORWARD.match(y) for y in reversed(ins[max(0, k - 4):k]) if FORWARD.match(y)), None)
            t = core[int(f.group(1), 16) // 8] if f and core and int(f.group(1), 16) // 8 < len(core) else None
            tn = space.syms.get(t) if t else None
            if tn and method_name(demangle([tn])[0]) == method_name(demangle([name])[0]):
                kinds |= return_kinds(space, t, tn, core, depth + 1, seen)
            else:
                kinds.add('unknown')
        else:
            j = re.match(r'jmp 0x([0-9a-f]+)', x)
            if j and not va <= int(j.group(1), 16) < va + n:
                t = int(j.group(1), 16)
                kinds |= return_kinds(space, t, space.syms.get(t) or name, core, depth + 1, seen)
    return kinds


def header_returns():
    """{method name: set of return types} as airport/abi declares the pure virtuals."""
    decl = re.compile(r'^\s*virtual\s+(.+?)\s*\b(\w+)\s*\([^)]*\)\s*(?:const)?\s*=\s*0\s*;', re.M)
    res = {}
    d = os.path.join(ROOT, 'airport/abi/Airport/macOS26')
    for f in os.listdir(d):
        for ret, name in decl.findall(open(os.path.join(d, f)).read()):
            res.setdefault(name, set()).add(ret.replace(' ', ''))
    return res


def kind_of_type(t):
    if t == 'bool':
        return 'bool'
    if t.endswith('*'):
        return 'ptr'
    return 'int'


def returns(classes):
    kcs = [KC(p) for p in KCS]
    space = Space(kcs)
    sups = all_supers(space)
    core = vtable(space, 'AppleBCMWLANCore')
    hdr = header_returns()
    bad = 0
    for c in classes:
        vt = vtable(space, c)
        subs = [vtable(space, k) for k in subclasses(sups, c)]
        tally, odd = {}, []
        for i, t in enumerate(vt):
            if space.syms.get(t) != PURE:
                continue
            impl = next((s[i] for s in subs if s and i < len(s)
                         and space.syms.get(s[i]) not in (None, PURE)), None)
            if impl is None:
                kinds = {'no Apple implementation'}
            else:
                kinds = return_kinds(space, impl, space.syms[impl], core) or {'unknown'}
            key = '+'.join(sorted(kinds))
            tally[key] = tally.get(key, 0) + 1
            if impl is None:
                continue
            dem = demangle([space.syms[impl]])[0]
            want = {kind_of_type(t) for t in hdr.get(method_name(dem), set())}
            seen = kinds & {'int', 'ioreturn', 'bool', 'ptr'}
            seen = {'int' if k == 'ioreturn' else k for k in seen}
            if not want:
                odd.append('%d: %s: not declared pure in airport/abi' % (i, dem))
            elif seen - want - ({'int'} if want == {'bool'} else set()):   # xor eax,eax is false
                odd.append('%d: %s returns %s, airport/abi declares %s'
                           % (i, dem, '/'.join(sorted(seen)), '/'.join(sorted(hdr[method_name(dem)]))))
        print('%-24s pure slots by return: %s' % (c, ', '.join('%s %d' % kv for kv in sorted(tally.items()))))
        for o in odd:
            print('    ' + o)
        bad += len(odd)
    sys.exit(1 if bad else 0)


def main():
    args = sys.argv[1:]
    if args[:1] == ['--returns']:
        return returns(args[1:] or ['IO80211InfraProtocol', 'IO80211Controller'])
    if args[:1] == ['--check']:
        return check(args[1:] or CHECK_CLASSES)
    out = sys.stdout
    if args[:1] == ['-o']:
        out = open(args[1], 'w')
        args = args[2:]
    if not args:
        sys.exit(__doc__)
    kcs = [KC(p) for p in KCS]
    space = Space(kcs)
    import platform, subprocess
    build = subprocess.run(['sysctl', '-n', 'kern.osversion'], capture_output=True, text=True).stdout.strip()
    out.write('# kc-abi.py dump, macOS build %s, Darwin %s\n' % (build, platform.release()))

    bad = 0
    tables = {}
    sups = all_supers(space)
    for c in args:
        vt = vtable(space, c)
        if vt is None:
            print('%s: no vtable symbol __ZTV%s' % (c, mangle_class(c)), file=sys.stderr)
            bad += 1
            continue
        sup = superclass(space, c)
        svt = tables.get(sup) or (vtable(space, sup) if sup else None) or []
        tables[c] = vt
        names = [space.syms.get(t) or '0x%x' % t for t in vt]
        # names for the pure slots, from subclasses that implement them
        impl = {}
        subs = [(k, vtable(space, k)) for k in subclasses(sups, c)]
        for i, n in enumerate(names):
            if n != PURE:
                continue
            for k, kvt in subs:
                if kvt and i < len(kvt):
                    kn = space.syms.get(kvt[i])
                    if kn and kn != PURE:
                        impl[i] = kn
                        break
        dem = demangle(names)
        idem = dict(zip(impl, demangle(list(impl.values()))))
        own = space.owner.get(vt[0], '?')
        out.write('\nclass %s : %s  slots %d (super %d)  bundle %s\n'
                  % (c, sup or '?', len(vt), len(svt), space.owner.get(space.addr['__ZTV' + mangle_class(c)], '?')))
        for i, (t, n, d) in enumerate(zip(vt, names, dem)):
            if n == PURE:
                kind = 'pure'
            elif i >= len(svt):
                kind = 'new'
            elif svt[i] == t:
                kind = 'inh'
            else:
                kind = 'over'
            if i in impl:
                d = '= ' + idem[i]
            out.write('%4d %-4s %s | %s\n' % (i, kind, n, d))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
