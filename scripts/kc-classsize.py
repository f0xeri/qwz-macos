#!/usr/bin/env python3
"""Print the object size of IOKit classes from this Mac's kernel collections.

usage: scripts/kc-classsize.py [-v] [CLASS ...]   (default: the classes native Wi-Fi subclasses)
       -v  also print the segments, the hits per class and the code around them

Read-only, needs no root. Native Wi-Fi (docs/native-wifi-research.md §8) subclasses Apple's
IO80211/Skywalk classes through reverse-engineered headers. A vtable check (kc-abi.py) shows the
method slots; this shows the sizes, which decide where a subclass's own fields start. Every
class registers itself with OSMetaClass::OSMetaClass(this, "Name", &Super::gMetaClass, sizeof);
on x86_64 that call loads the name with `lea rsi,[rip+...]` and the size with `mov ecx,imm32`.
The script finds the name string, every instruction that loads it into rsi, and the size next
to it. A hit counts as confirmed when a call to OSMetaClass's constructor follows."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kclib import KCS, KC, Space, demangle, metaclass_hits  # noqa: E402

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

def main():
    args = sys.argv[1:]
    verbose = '-v' in args
    classes = [a for a in args if a != '-v'] or list(EXPECT)
    kcs = [KC(p) for p in KCS]
    space = Space(kcs)

    found, nstr = metaclass_hits(kcs, space, classes, verbose)

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


if __name__ == '__main__':
    main()
