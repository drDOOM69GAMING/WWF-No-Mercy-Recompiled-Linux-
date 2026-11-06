#!/usr/bin/env python3
"""Repair jump tables whose index N64Recomp constant-folded to zero.

N64Recomp models a `jr $v0` jump table as `switch (jr_addend_<jrvram> >> 2)`.
It normally derives the byte offset from an in-block `sll idx, 2; addu base,
idx` sequence. For a loop-carried induction pointer, the base is set before the
loop and advanced by 4 on each iteration, so N64Recomp may emit the degenerate
`gpr jr_addend_<jrvram> = 0;`. The resulting switch always selects case 0.

The known No Mercy case is the championship storyline-condition evaluator
(func_800EE1F0, ovl_a): its mask-select table at 0x801036D0 would always return
0x7, causing conditions in higher fields to take the loss/default path.

For each affected switch, this script finds the table base in its
`switch_error` default and the table-entry pointer register from the modeled
load above the switch, then calculates the live byte offset before dispatch.
The uint32_t cast is required because guest pointers in ctx are sign-extended.

This is idempotent and intentionally fails loudly if a zero addend cannot be
resolved, rather than leaving a silently collapsed switch in a build.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = '/* [nomercy fix_jraddend] */'

decl_re = re.compile(r'^([ \t]*)gpr jr_addend_([0-9A-Fa-f]+) = 0;[ \t]*$')
sw_re = re.compile(r'^([ \t]*)switch \(jr_addend_([0-9A-Fa-f]+) >> \d+\) \{[ \t]*$')
err_re = re.compile(r'default: switch_error\(__func__, 0x[0-9A-Fa-f]+, (0x[0-9A-Fa-f]+)\);')
load_re = re.compile(r'^[ \t]*ctx->r(\d+) = ADD32\(ctx->r\d+, 0X[0-9A-Fa-f]+\);[ \t]*$')

fixed = failed = 0

for path in sorted(glob.glob(os.path.join(ROOT, 'RecompiledFuncs', 'funcs_*.c'))):
    with open(path, 'rb') as f:
        src = f.read().decode('utf-8', errors='replace')
    newline = '\r\n' if '\r\n' in src else '\n'
    lines = src.splitlines()
    decls = {}
    for i, line in enumerate(lines):
        match = decl_re.match(line)
        if match and TAG not in line:
            decls[match.group(2)] = i

    inserts = {}
    tagged_decls = set()
    for i, line in enumerate(lines):
        switch = sw_re.match(line)
        if not switch:
            continue
        jrvram = switch.group(2)
        if jrvram not in decls:
            continue

        table = None
        for j in range(i + 1, min(i + 40, len(lines))):
            error = err_re.search(lines[j])
            if error:
                table = error.group(1)
                break
        if table is None:
            print('FAIL jr_addend_%s: no switch_error/table base after switch' % jrvram)
            failed += 1
            continue

        dest_reg = None
        for j in range(i - 1, max(i - 8, 0), -1):
            load = load_re.match(lines[j])
            if load:
                dest_reg = load.group(1)
                break
        if dest_reg is None:
            print('FAIL jr_addend_%s: no modeled table-load line above switch' % jrvram)
            failed += 1
            continue

        indent = switch.group(1)
        inserts[i] = ('%sjr_addend_%s = (uint32_t)ctx->r%s - %sU; %s'
                      % (indent, jrvram, dest_reg, table, TAG))
        tagged_decls.add(decls[jrvram])
        fixed += 1
        print('repaired jr_addend_%s: switch now indexed by ctx->r%s - %s (was const 0)'
              % (jrvram, dest_reg, table))

    if inserts or tagged_decls:
        output = []
        for i, line in enumerate(lines):
            if i in tagged_decls:
                line = line.rstrip() + ' ' + TAG
            if i in inserts:
                output.append(inserts[i])
            output.append(line)
        with open(path, 'wb') as f:
            f.write((newline.join(output) + newline).encode('utf-8'))

print('%d jump-table indices repaired, %d failures' % (fixed, failed))
sys.exit(1 if failed else 0)
