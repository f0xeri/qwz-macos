#!/usr/bin/env python3
"""Dump the C++ ABI of IOKit classes from this Mac's kernel collections: super class and
vtable slots (what each slot points to, demangled), as a text file the header generator reads.

usage: scripts/kc-abi.py [-o FILE] CLASS ...

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
pure (pure virtual in C; the name after '|' is the subclass's method, '= Sub::name(...)')."""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kclib import KCS, KC, Space, demangle, mangle_class  # noqa: E402

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


def main():
    args = sys.argv[1:]
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
