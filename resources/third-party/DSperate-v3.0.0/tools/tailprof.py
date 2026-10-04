#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Profile of the slow frames only: perf samples of the emulation thread cut to
# each frame's emu slice, the p<TAIL>+ frames (default 95) against typical
# (the middle 20%), per bucket and per symbol, sorted by what the tail adds.
#
# On the device (SDL frontend; timestamps are CLOCK_MONOTONIC on both sides):
#   DS_PERF_MAP=1 DS_FRAME_SERIES=s.series ./dsperate ... &
#   perf record -F 4999 --clockid monotonic -t $(pidof dsperate) -o s.data
#   perf script -i s.data -F time,ip,sym > s.script   # while /tmp/perf-<pid>.map exists
# Then: tools/tailprof.py s.series s.script [TAIL_PCT]
# DS_PROFILE=1 DS_PROFILE_TAIL=95 gives the same split by timed stage.
import bisect, collections, re, sys

series, script = sys.argv[1], sys.argv[2]
tail_pct = float(sys.argv[3]) if len(sys.argv) > 3 else 95.0
HZ = 4999.0

frames = []   # (emu_ms, start_ns, end_ns)
for l in open(series):
    p = l.split()
    if len(p) >= 4: frames.append((float(p[0]), int(p[2]), int(p[3])))
frames_by_start = sorted(range(len(frames)), key=lambda i: frames[i][1])
starts = [frames[i][1] for i in frames_by_start]

order = sorted(range(len(frames)), key=lambda i: frames[i][0])
n = len(order)
tail = set(order[int(n * tail_pct / 100):])
mid = set(order[int(n * 0.4):int(n * 0.6)])

cnt = {'tail': collections.Counter(), 'mid': collections.Counter()}
outside = 0
for l in open(script):
    m = re.match(r'\s*(\d+\.\d+):\s*([0-9a-f]+)\s*(.*)', l)
    if not m: continue
    t = int(round(float(m.group(1)) * 1e9)); sym = m.group(3).strip() or '[unknown]'
    if sym == '[unknown]': sym = '0x' + m.group(2)
    elif m.group(2).startswith('ffff'): sym = '[k] ' + sym
    k = bisect.bisect_right(starts, t) - 1
    if k < 0: outside += 1; continue
    i = frames_by_start[k]
    if t > frames[i][2]: outside += 1; continue
    if i in tail: cnt['tail'][sym] += 1
    elif i in mid: cnt['mid'][sym] += 1

ms = 1000.0 / HZ
def per_frame(c, nf): return {s: v * ms / nf for s, v in c.items()}
T = per_frame(cnt['tail'], len(tail)); M = per_frame(cnt['mid'], len(mid))

rules = [('jit code', r'^jit[79]_'), ('jit stubs', r'jit_stub'), ('jit helpers/slice', r'jit_h_|ds::jit::|ds_slice|bios_sha'),
         ('3d geometry', r'Gpu3D|gxfifo|TextureCache|Renderer3D|Polygon|finalise_list'), ('2d', r'Engine2D|kern::|output_line|gpu::Gpu::'),
         ('dma', r'dma::'), ('spu', r'spu::'), ('io/bus/mem', r'io::|mem::|Bus::|PageTable'), ('scheduler', r'Scheduler|sched::'),
         ('interp', r'interp::'), ('libc/kernel', r'memcmp|memcpy|memset|\[k\]|^0xffff'), ('unresolved', r'^\[unknown\]|^0x')]
def bucket(s):
    for k, r in rules:
        if re.search(r, s): return k
    return 'other'
BT, BM = collections.Counter(), collections.Counter()
for s, v in T.items(): BT[bucket(s)] += v
for s, v in M.items(): BM[bucket(s)] += v

emu = lambda idx: sum(frames[i][0] for i in idx) / max(1, len(idx))
print(f"frames {n}: tail p{tail_pct:.0f}+ {len(tail)} frames, emu {emu(tail):.2f} ms; typical {len(mid)} frames, emu {emu(mid):.2f} ms; samples outside frames {outside}")
print(f"sampled emu-thread ms/frame: tail {sum(T.values()):.2f}  typical {sum(M.values()):.2f}")
print(f"{'bucket':20s} {'typ ms':>8s} {'tail ms':>8s} {'delta':>8s}")
for k in sorted(set(BT) | set(BM), key=lambda k: -(BT[k] - BM[k])):
    print(f"{k:20s} {BM[k]:8.3f} {BT[k]:8.3f} {BT[k]-BM[k]:+8.3f}")
print("top symbols by what the tail adds:")
for s in sorted(set(T) | set(M), key=lambda s: -(T.get(s, 0) - M.get(s, 0)))[:25]:
    print(f"  {M.get(s,0):7.3f} {T.get(s,0):7.3f} {T.get(s,0)-M.get(s,0):+7.3f}  {s[:110]}")
