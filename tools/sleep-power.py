#!/usr/bin/env python3
"""Sleep power from a prova-sonno.sh CSV (epoch, mAh, mV, mA, sleeps).

The gauge reports the charge in 44 mAh steps. Reference points are the last step before the sleep and the
first real step after the wake (the first change after the wake is the gauge refreshing a stale value and is
skipped). Between them: drain = mAh(before) - mAh(after); the awake part of it is the integral of the sampled
current (samples less than 5 s apart); the rest happened during the sleep, whose length is the S0ixSleep
wait time (ms= in the .stat file next to the CSV)."""
import sys, pathlib

csv = pathlib.Path(sys.argv[1])
rows = []
for line in csv.read_text().splitlines():
    try:
        t, c, v, a, s = line.split(',')
        rows.append((int(t), int(c), int(v), int(a), int(s)))
    except ValueError:
        pass
stat = dict(x.split('=', 1) for x in csv.with_suffix('.stat').read_text().split() if '=' in x)
ms = int(stat.get('ms', 0))
# sleep gap
gap = max(range(1, len(rows)), key=lambda i: rows[i][0] - rows[i - 1][0])
pre, post = rows[:gap], rows[gap:]
def real_steps(seg):
    """Indexes of genuine gauge steps: a drop of 1-2 steps (44-88 mAh) from a value stable for >= 10 s.
    Refreshes and glitches right after a wake (2420 -> 1320 -> 2244) are not."""
    out, since = [], seg[0][0] if seg else 0
    for i in range(1, len(seg)):
        if seg[i][1] != seg[i - 1][1]:
            d = seg[i - 1][1] - seg[i][1]
            if 0 < d <= 88 and seg[i][0] - since >= 10:
                out.append(i)
            since = seg[i][0]
    return out
pre_steps, post_steps = real_steps(pre), real_steps(post)
if not pre_steps or not post_steps:
    sys.exit('missing a gauge step: %d before, %d after the sleep' % (len(pre_steps), len(post_steps)))
i0, i1 = pre_steps[-1], post_steps[0]
t0, c0 = pre[i0][0], pre[i0][1]
t1, c1 = post[i1][0], post[i1][1]
awake = 0.0
for seg in (pre[i0:], post[:i1 + 1]):
    for (ta, _, _, aa, _), (tb, _, _, ab, _) in zip(seg, seg[1:]):
        if tb - ta < 5:
            awake += -(aa + ab) / 2 * (tb - ta) / 3600.0   # mAh drawn (current is negative on battery)
drain = c0 - c1
sleep_mAh = drain - awake
volts = [r[2] for r in rows] or [15000]
v = sum(volts) / len(volts) / 1000.0
hours = ms / 3.6e6
print('gauge %d -> %d mAh: %d mAh, of which awake %.0f mAh -> sleep %.0f mAh in %.1f min' %
      (c0, c1, drain, awake, sleep_mAh, ms / 60000.0))
print('sleep current %.0f mA, power %.2f W (at %.2f V); package %s mW, pc %s' %
      (sleep_mAh / hours if hours else 0, sleep_mAh / hours * v / 1000 if hours else 0, v,
       stat.get('pkg_mw'), stat.get('pc')))
print('uncertainty: about +/- %.2f W (gauge step timing +/- 5 s, current integral +/- 10 %%)' %
      ((2 * 1.5 + 0.1 * awake) / hours * v / 1000 if hours else 0))
