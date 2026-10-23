#!/usr/bin/env python3
"""Turn dqvc.txt (tools/dqvc_build.py) into runtime/dqvc_schedule.h: the 52
sales as plain numbers (start date, item number, price, quantity), without
the item names or sale titles, so the program has DQVC's schedule built in.

  dqvc_to_c.py <dqvc.txt> <runtime/dqvc_schedule.h>
"""
import sys
weeks, items = [], []
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.strip()
    if not line or line.startswith('#'): continue
    if line.startswith('week'):
        mo, d = line.split()[1].split('/')
        weeks.append([int(mo), int(d), len(items), 0])
        continue
    f = line.split()
    if len(f) >= 3 and weeks:
        items.append((int(f[0]), int(f[1]), int(f[2]))); weeks[-1][3] += 1
out = ['/* DQVC: the 52 sales the original service ran every year (July 2010 - 2014),',
       ' * as numbers only: the date each started in 2011-12, then each item\'s number,',
       ' * price and quantity. Made by tools/dqvc_to_c.py from dqvc.txt (which',
       ' * tools/dqvc_build.py makes from the lists on yab\'s DQ9 Tools). */',
       '#pragma once', '#include <stdint.h>',
       'typedef struct { uint8_t month, day; uint16_t first, count; } DqvcWeek;',
       'typedef struct { uint16_t id; uint8_t qty; uint32_t price; } DqvcItem;',
       'static const DqvcWeek DQVC_WEEKS[%d] = {' % len(weeks)]
for w in weeks: out.append('    { %d, %d, %d, %d },' % tuple(w))
out.append('};')
out.append('static const DqvcItem DQVC_ITEMS[%d] = {' % len(items))
row = []
for i, (iid, price, qty) in enumerate(items):
    row.append('{ %d, %d, %d }' % (iid, qty, price))
    if len(row) == 6: out.append('    ' + ', '.join(row) + ','); row = []
if row: out.append('    ' + ', '.join(row) + ',')
out.append('};')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
print(len(weeks), 'weeks,', len(items), 'items')
