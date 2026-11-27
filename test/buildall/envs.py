#!/usr/bin/env python3
"""Print one line per PlatformIO env from platformio.ini: NAME TARGET SDKCONFIG_DEFAULTS.

The base-* envs are skipped; the target comes from the base env at the end of the `extends`
chain (`env:base-<target>`), the defaults from the first env in the chain that sets them.
usage: envs.py [platformio.ini]
"""
import configparser
import re
import sys

c = configparser.ConfigParser(interpolation=None, strict=False)
c.read(sys.argv[1] if len(sys.argv) > 1 else 'platformio.ini')
for sec in c.sections():
    if not sec.startswith('env:') or sec.startswith('env:base-'):
        continue
    name, chain = sec[4:], [sec]
    while c.has_option(chain[-1], 'extends'):
        chain.append(c[chain[-1]]['extends'].strip())
    target = chain[-1].replace('env:base-', '') if chain[-1].startswith('env:base-') else 'esp32'
    args = next((c[s]['board_build.cmake_extra_args'] for s in chain
                 if c.has_option(s, 'board_build.cmake_extra_args')), '')
    m = re.search(r'-DSDKCONFIG_DEFAULTS=([^"\s]+)', args)
    if m:
        print(name, target, m[1])
