#!/usr/bin/env python3
"""Relate the ANIM tick to real time: animations with a single sound event
near t=0 whose sound clip has one length, ratio = anim duration (ticks) /
sound length (ms). A peak at 1.0 means 1 tick = 1 ms (docs/data-findings.md).

usage: s2pkg anim-events <Res> > anim_events.tsv
       tick_vs_sound.py <s2pkg> <Res> anim_events.tsv
"""
import collections, json, os, re, statistics, subprocess, sys

s2pkg, res, tsv = sys.argv[1:4]
rows = [l.rstrip('\n').split('\t') for l in open(tsv)]
cache_path = 'sndlen.json'
lengths = json.load(open(cache_path)) if os.path.exists(cache_path) else {}
by, duration = collections.defaultdict(list), {}
for r in rows:
    duration[r[0]] = int(r[1])
    m = re.search(r'sound_name=([^|]*)', r[6]) if r[5] == '1014' else None
    if m:
        by[r[0]].append((int(r[4]), m.group(1).strip()))
for name in {n for ev in by.values() for _, n in ev}:
    if name not in lengths:
        out = subprocess.run([s2pkg, 'sound-info', res, name], capture_output=True, text=True).stdout
        lengths[name] = [float(x) for x in re.findall(r' ([0-9.]+)s$', out, re.M)]
json.dump(lengths, open(cache_path, 'w'))
ratios = []
for anim, ev in by.items():
    if len(ev) != 1 or ev[0][0] > 100:
        continue
    ls = lengths.get(ev[0][1]) or []
    if not ls or max(ls) - min(ls) > 0.05 * max(ls) or max(ls) < 1.5:
        continue
    ratios.append(duration[anim] / (statistics.mean(ls) * 1000))
hist = collections.Counter(round(x, 1) for x in ratios)
print(len(ratios), 'animations;', sorted(hist.items()))
