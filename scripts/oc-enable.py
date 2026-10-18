#!/usr/bin/env python3
"""Add/enable or disable QwzWifi in OpenCore config.plist (with backup).
usage: oc-enable.py on|probe|off [path/to/EFI/OC]   (default: the path in local/oc-dir)
  on     kext enabled, boot-arg -qwzon            (normal run)
  probe  kext enabled, boot-args -qwzon -qwzprobe (stage 0: read-only probe)
  off    kext disabled, no QwzWifi boot-args"""
import plistlib, shutil, sys, time, os
mode = sys.argv[1] if len(sys.argv) > 1 else ''
if mode not in ('on', 'probe', 'off'):
    sys.exit(__doc__)
local = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'local', 'oc-dir')
if len(sys.argv) > 2:
    oc = sys.argv[2]
elif os.path.exists(local):
    oc = open(local).read().strip()
else:
    sys.exit('no OpenCore path: pass it, or put it into local/oc-dir')
cfg = os.path.join(oc, 'config.plist')
bak = cfg + '.before-qwz-' + time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(cfg, bak); print('backup:', bak)
p = plistlib.load(open(cfg, 'rb'))
add = p['Kernel']['Add']
ent = next((k for k in add if k['BundlePath'] == 'QwzWifi.kext'), None)
if ent is None:
    ent = {'Arch': 'x86_64', 'BundlePath': 'QwzWifi.kext', 'Comment': 'Qualcomm WCN7850 (dev)',
           'Enabled': True, 'ExecutablePath': 'Contents/MacOS/QwzWifi', 'MaxKernel': '',
           'MinKernel': '', 'PlistPath': 'Contents/Info.plist'}
    i = next(i for i, k in enumerate(add) if k['BundlePath'] == 'Lilu.kext') + 1
    add.insert(i, ent)
ent['Enabled'] = (mode != 'off')
nv = p['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']
args = [a for a in nv['boot-args'].split() if a not in ('-qwzon', '-qwzprobe')]
if mode != 'off': args.append('-qwzon')
if mode == 'probe': args.append('-qwzprobe')
nv['boot-args'] = ' '.join(args)
plistlib.dump(p, open(cfg, 'wb'))
print('QwzWifi', mode, '| boot-args:', nv['boot-args'])
