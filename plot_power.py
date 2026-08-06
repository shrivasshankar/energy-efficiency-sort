#!/usr/bin/env python3
"""Plot where a sort's joules actually go, from power_log.py CSVs.

    ./plot_power.py --records 1e9 --idle 3.54 split.csv merge.csv -o energy.png

Splits the area under the power curve into three bands, because that is the
question "is there anything algorithmic to improve" actually reduces to:

    idle        platform draw that exists whether or not you are sorting.
                Only shrinks by finishing sooner, or by turning the display off.
    SoC         CPU + GPU + ANE + RAM. The ONLY band any algorithmic change
                touches. If it is a third of the total, a 2x faster comparator
                caps out at a 17% energy win.
    rest        SSD, regulators, display backlight, board. Shrinks with I/O
                volume and with wall time, not with cleverness.
"""

import argparse
import csv
import sys


def load(path):
    t, sysw, socw = [], [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            t.append(float(row["seconds"]))
            sysw.append(float(row.get("sys_power", row.get("watts", 0))))
            socw.append(float(row.get("soc_power", 0)))
    if len(t) < 2:
        sys.exit(f"{path}: need at least 2 samples")
    return t, sysw, socw


def integrate(t, w):
    return sum((t[i + 1] - t[i]) * (w[i + 1] + w[i]) / 2.0
               for i in range(len(t) - 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--records", type=float, default=0)
    ap.add_argument("--idle", type=float, default=0.0, help="measured idle watts")
    ap.add_argument("-o", "--out", default="energy.png")
    ap.add_argument("--title", default="")
    args = ap.parse_args()

    phases, offset = [], 0.0
    for path in args.csv:
        t, sysw, socw = load(path)
        phases.append((path.rsplit(".", 1)[0], t, sysw, socw, offset,
                       integrate(t, sysw), integrate(t, socw)))
        offset += t[-1]
    total_span = offset

    sys_j = sum(p[5] for p in phases)
    soc_j = sum(p[6] for p in phases)
    idle_j = args.idle * total_span
    rest_j = sys_j - soc_j - idle_j

    print(f"  {'band':<28} {'joules':>9}   {'share':>6}")
    print(f"  {'idle (platform)':<28} {idle_j:9.0f}   {100*idle_j/sys_j:5.1f}%")
    print(f"  {'SoC (algorithm can touch)':<28} {soc_j:9.0f}   {100*soc_j/sys_j:5.1f}%")
    print(f"  {'rest (SSD/board/display)':<28} {rest_j:9.0f}   {100*rest_j/sys_j:5.1f}%")
    print(f"  {'TOTAL':<28} {sys_j:9.0f}")
    print()
    for name, t, _, _, _, sj, soj in phases:
        print(f"  {name:<28} {sj:9.0f} J   SoC {100*soj/sj:4.1f}%")
    if args.records:
        print(f"\n  records/joule  {args.records/sys_j:,.0f}")
    print(f"\n  A 2x cheaper comparator would cut at most "
          f"{100*soc_j/sys_j/2:.0f}% of the total.")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("\n  (pip install matplotlib for the plot)")

    fig, ax = plt.subplots(figsize=(11, 4.8))
    for i, (name, t, sysw, socw, off, sj, soj) in enumerate(phases):
        x = [v + off for v in t]
        base = [args.idle] * len(x)
        soc_top = [args.idle + s for s in socw]
        ax.fill_between(x, 0, base, color="0.75",
                        label="idle (platform)" if i == 0 else None)
        ax.fill_between(x, base, soc_top, color="#d1495b", alpha=0.75,
                        label="SoC — the only band algorithms touch" if i == 0 else None)
        ax.fill_between(x, soc_top, sysw, color="#30638e", alpha=0.55,
                        label="SSD / board / display" if i == 0 else None)
        ax.plot(x, sysw, color="#16324f", linewidth=0.8)
        if off:
            ax.axvline(off, color="0.3", linestyle=":", linewidth=1)
        ax.annotate(f"{name}\n{sj:.0f} J", xy=(off + t[-1] / 2, max(sysw) * 0.93),
                    ha="center", fontsize=9)

    ax.set_xlim(0, total_span)
    ax.set_ylim(0, max(max(p[2]) for p in phases) * 1.1)
    ax.set_xlabel("seconds")
    ax.set_ylabel("watts (whole system)")
    ax.set_title(args.title or
                 f"{sys_j:.0f} J total  —  SoC {100*soc_j/sys_j:.0f}%, "
                 f"platform+I/O {100*(rest_j+idle_j)/sys_j:.0f}%")
    ax.legend(loc="lower right", fontsize=9, framealpha=0.92)
    ax.grid(alpha=0.2)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"  wrote {args.out}")


if __name__ == "__main__":
    main()
