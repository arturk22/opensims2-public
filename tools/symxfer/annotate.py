#!/usr/bin/env python3
"""Annotate Ghidra C output: replace virtual calls on `this` (param_1) with
the slot's name from a vtables.py dump of the function's class (first,
primary vtable only).
usage: annotate.py <vtables dump> <decompiled.c>  > annotated.c
"""
import re, sys
slots = {}
blocks = 0
for line in open(sys.argv[1]):
    if line.startswith('#'):
        blocks += 1
        continue
    if blocks == 1:
        p = line.split(None, 2)
        if len(p) == 3:
            name = p[2].strip()
            name = re.sub(r'\(.*', '', name)
            slots[int(p[0], 16)] = name
src = open(sys.argv[2]).read()
out = re.sub(r'\(\*\*\(code \*\*\)\(\*param_1 \+ (0x[0-9a-f]+)\)\)', lambda m: '%s/*%s*/' % (m.group(0), slots.get(int(m.group(1), 16), '?')), src)
out = re.sub(r'\*\(code \*\*\)\(\*param_1 \+ (0x[0-9a-f]+)\);', lambda m: '%s /*%s*/' % (m.group(0), slots.get(int(m.group(1), 16), '?')), out)
sys.stdout.write(out)
