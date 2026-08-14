"""Shared plot styling.

Palette is the validated categorical default (validate_palette.js: all checks
PASS on the light surface; two slots carry a contrast WARN, so every series is
DIRECT-LABELLED and every figure has a matching table in REPORT.md -- the
documented relief rule).
"""

from __future__ import annotations

import matplotlib
import numpy as np
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
INK_MUTED = "#8a8880"
GRID = "#e3e2dd"

# fixed categorical order -- never cycled, never reassigned by rank
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
LIMIT = "#e34948"          # status: the acceptance threshold line


def apply():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "font.family": "DejaVu Sans", "font.size": 9,
        "axes.edgecolor": GRID, "axes.labelcolor": INK_2,
        "axes.titlecolor": INK, "axes.titlesize": 10.5,
        "axes.titleweight": "600", "axes.titlelocation": "left",
        "axes.titlepad": 9, "axes.labelsize": 9,
        "axes.spines.top": False, "axes.spines.right": False,
        "xtick.color": INK_2, "ytick.color": INK_2,
        "xtick.labelsize": 8.5, "ytick.labelsize": 8.5,
        "xtick.direction": "out", "ytick.direction": "out",
        "grid.color": GRID, "grid.linewidth": 0.8,
        "legend.frameon": False, "legend.fontsize": 8.5,
        "lines.linewidth": 2.0, "lines.markersize": 5.5,
        "figure.dpi": 150,
    })


def tidy(ax, ygrid=True):
    ax.set_axisbelow(True)
    ax.grid(axis="y" if ygrid else "x", linewidth=0.8, color=GRID)
    ax.tick_params(length=0)


def limit_line(ax, y, label, color=LIMIT, x=0.99, ha="right"):
    ax.axhline(y, color=color, linewidth=1.4, linestyle=(0, (4, 3)), zorder=1)
    ax.text(x, y, f" {label} ", transform=ax.get_yaxis_transform(),
            ha=ha, va="bottom", fontsize=8, color=color, weight="600")


def plain_ticks(ax, axis="both"):
    """Log axes default to 10^n / 6x10^0 labels; use plain numbers instead."""
    from matplotlib.ticker import NullFormatter, ScalarFormatter
    for a in ((ax.xaxis, ax.yaxis) if axis == "both"
              else (ax.xaxis if axis == "x" else ax.yaxis,)):
        a.set_major_formatter(ScalarFormatter())
        a.set_minor_formatter(NullFormatter())


def label_ends(ax, series, dx=0.12, min_gap_frac=0.055):
    """Direct-label each line at its right end, nudged apart so labels from
    series that converge do not overprint. `series` = [(xs, ys, text, color)].
    Required relief for the two low-contrast palette slots."""
    if not series:
        return
    lo, hi = ax.get_ylim()
    log = ax.get_yscale() == "log"

    def to_frac(v):
        return ((np.log10(v) - np.log10(lo)) / (np.log10(hi) - np.log10(lo))
                if log else (v - lo) / (hi - lo))

    def from_frac(f):
        return (10 ** (np.log10(lo) + f * (np.log10(hi) - np.log10(lo)))
                if log else lo + f * (hi - lo))

    items = sorted(((to_frac(s[1][-1]), s) for s in series), key=lambda z: z[0])
    placed = []
    for f, s in items:
        # single assignment, never a loop: `(a + g) - a < g` can round to True
        # forever, so re-testing the gap after nudging can spin indefinitely.
        if placed:
            f = max(f, placed[-1] + min_gap_frac)
        placed.append(f)
        xs, _, text, color = s
        ax.text(xs[-1] + dx, from_frac(f), text, color=color, fontsize=8.5,
                weight="600", va="center", ha="left")

