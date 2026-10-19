#!/usr/bin/env python3
"""Check that a kext links the way `kmutil load` links it: every undefined symbol must come from
a library in its OSBundleLibraries, either a KPI symbol set of the kernel or the exports of a
kext. Lists what does not resolve, and anything taken from com.apple.kpi.private.

usage: scripts/kpi-check.py [--opencore] build/QwzWifi.kext [build/QwzAirport.kext ...]

--opencore: explicitly allow _thread_call_isactive in our two bundles only.
The user chose native xnu calls for OpenCore injection; kmutil load cannot link it.

Read-only, no root. Why not `kmutil libraries -p X.kext --undef-symbols`: it silently leaves
out symbols it cannot place (thread_call_isactive, exported by no KPI set, passed it). OpenCore's
linker resolves against the whole kernel symbol table, so such a symbol works when the kext is
injected at boot and fails only with kmutil load. The KPI sets are the kernel's
__LINKINFO,__symbolsets plist; a kext's exports are the external symbols of its fileset entry in
the boot/system kernel collections."""
import os, plistlib, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kclib import KCS, KC  # noqa: E402

KERNEL = '/System/Library/Kernels/kernel'


def symbol_sets():
    """{kpi bundle id: set of symbol names} from the kernel's __symbolsets section."""
    d = open(KERNEL, 'rb').read()
    off = 0
    if struct.unpack_from('>I', d, 0)[0] == 0xcafebabe:            # fat: the x86_64 slice
        for i in range(struct.unpack_from('>I', d, 4)[0]):
            ct, _cs, o, _sz, _al = struct.unpack_from('>IIIII', d, 8 + 20 * i)
            if ct == 0x01000007:
                off = o
    ncmds = struct.unpack_from('<I', d, off + 16)[0]
    q = off + 32
    blob = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, q)
        if cmd == 0x19:
            for k in range(struct.unpack_from('<I', d, q + 64)[0]):
                s = q + 72 + 80 * k
                if d[s:s + 16].rstrip(b'\0') == b'__symbolsets':
                    _addr, sz, fo = struct.unpack_from('<QQI', d, s + 32)
                    blob = d[off + fo:off + fo + sz]
        q += size
    if blob is None:
        sys.exit('kpi-check.py: no __symbolsets in %s' % KERNEL)
    sets = {}
    for st in plistlib.loads(blob)['SymbolsSets']:
        names = sets.setdefault(st['CFBundleIdentifier'], set())
        for x in st['Symbols']:
            if 'SymbolName' in x:
                names.add(x['SymbolName'])
            elif 'SymbolPrefix' in x:
                names.add(x['SymbolPrefix'] + '*')
    return sets


def kext_exports():
    """{bundle id: set of external symbols} for every kext in the kernel collections."""
    ex = {}
    for p in KCS:
        for _value, name, bundle, external in KC(p).symbols():
            if external:
                ex.setdefault(bundle, set()).add(name)
    return ex


def in_set(names, sym):
    return sym in names or any(n.endswith('*') and sym.startswith(n[:-1]) for n in names)


def check(kext, sets, exports, opencore=False):
    info = plistlib.load(open(os.path.join(kext, 'Contents/Info.plist'), 'rb'))
    binary = os.path.join(kext, 'Contents/MacOS', info['CFBundleExecutable'])
    libs = list(info.get('OSBundleLibraries', {}))
    undef = subprocess.run(['nm', '-u', binary], capture_output=True, text=True).stdout.split()
    unresolved, private, used = [], [], {}
    pads = 0
    injected = []
    for sym in undef:
        if '_RESERVED' in sym:
            # a reserved vtable pad of a parent class: the kernel's linker fills the slot
            # from the parent's vtable, whether or not the pad symbol still exists
            pads += 1
            continue
        frm = [l for l in libs if (l in sets and in_set(sets[l], sym)) or sym in exports.get(l, ())]
        if not frm and opencore and sym == '_thread_call_isactive' and info['CFBundleIdentifier'] in (
                'com.local.QwzWifi', 'com.local.QwzAirport'):
            injected.append(sym)
            continue
        if not frm:
            where = [k for k, v in sets.items() if in_set(v, sym)] + \
                    [k for k, v in exports.items() if sym in v]
            unresolved.append((sym, where))
            continue
        for l in frm[:1]:
            used[l] = used.get(l, 0) + 1
        if frm == ['com.apple.kpi.private']:
            private.append(sym)
    name = os.path.basename(kext.rstrip('/'))
    print('%s: %d undefined symbols, from %s; %d reserved pads' % (name, len(undef),
          ', '.join('%s %d' % kv for kv in sorted(used.items())), pads))
    for sym in injected:
        print('    OPENCORE-ONLY (not a KPI export): %s' % sym)
    for l in libs:
        if l not in used:
            print('    library not needed: %s' % l)
    for sym, where in unresolved:
        print('    UNRESOLVED %s (%s)' % (sym, ', '.join(where[:3]) + ' not in OSBundleLibraries'
              if where else 'exported by nothing: only the full kernel symbol table has it'))
    for sym in private:
        print('    PRIVATE KPI %s' % sym)
    return not unresolved and not private


def main():
    args = sys.argv[1:]
    opencore = bool(args and args[0] == '--opencore')
    if opencore:
        args = args[1:]
    if not args:
        sys.exit(__doc__)
    sets, exports = symbol_sets(), kext_exports()
    ok = all([check(k, sets, exports, opencore) for k in args])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
