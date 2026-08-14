#!/usr/bin/env python3
"""S6 -- camera<->lidar calibration feasibility. Single entry point.

    ./.venv/bin/python run_all.py            # full run  (~6 min)
    ./.venv/bin/python run_all.py --quick    # smoke run (~1 min)

Writes plots/*.png, results/tables.md and results/results.json.
Everything is seeded, so a re-run reproduces the numbers in REPORT.md exactly.
"""

from __future__ import annotations

import argparse
import json
import time
import warnings
from pathlib import Path

import numpy as np

# numpy 2.0 on macOS/Accelerate emits spurious FP warnings from plain matmul
# (reproducible on `np.random.rand(40000,3) @ R.T`, which cannot divide).
# Verified not to produce non-finite output; silenced so real warnings show.
warnings.filterwarnings("ignore", message=".*encountered in matmul.*")

import matplotlib.pyplot as plt

from experiments import budget as B
from experiments import style as S
from experiments.campaign import (METHODS, build_session, extrinsic_errors,
                                  cad_nominal, method_supported,
                                  split_half_gate, trial_batch)
from sim import targets as tg
from sim.rig import CAM, MOUNTS
from solver.extrinsic import solve_extrinsic

ROOT = Path(__file__).resolve().parent
PLOTS = ROOT / "plots"
RESULTS = ROOT / "results"

N_POSES = (3, 5, 8, 12)
NOISE_MM = (5, 10, 20, 30)
BENCH_POSES = (12, 20, 30, 45)
PLOT_METHODS = ("board_a2", "board_a1", "board_xl", "wall", "corner")
SHORT = {"board_a2": "A2 board", "board_a1": "A1 board", "board_xl": "XL board",
         "wall": "bare wall", "corner": "corner pts"}

# Recommended wizard configuration per sensor, carried into the error budget.
BUDGET_CONFIGS = [
    ("mid360", "board_a1", 8, None, "Mid-360 - A1 board, 8 poses"),
    ("d6", "board_a1", 12, None, "COIN-D6 - A1 board, 12 poses (30 mm noise)"),
    ("d6", "board_a1", 12, 0.010, "COIN-D6 - A1 board, 12 poses (10 mm noise)"),
]


def pct(a, q):
    return float(np.percentile(a, q)) if len(a) else float("nan")


# ============================================================== experiment 1

def exp_poses(trials: int) -> dict:
    out = {}
    for mkey, mount in MOUNTS.items():
        for meth in PLOT_METHODS:
            if not method_supported(meth, mount):
                continue
            for n in N_POSES:
                r = trial_batch(mount, n, meth, trials, seed=n * 31 + len(meth))
                out[f"{mkey}|{meth}|{n}"] = r
                print(f"  {mkey:7s} {meth:9s} N={n:2d}  "
                      f"px@3m med {np.median(r['reproj_px_3m']):7.2f} "
                      f"p90 {pct(r['reproj_px_3m'], 90):8.2f}  "
                      f"rot {np.median(r['rot_deg']):6.3f} deg  "
                      f"trans {np.median(r['trans_mm']):6.1f} mm")
    return out


def exp_pose_style(trials: int) -> dict:
    """Does the wizard actually have to instruct roll/elevation variation?"""
    out = {}
    for mkey, meth, n in (("d6", "board_a1", 12), ("mid360", "board_a1", 8)):
        for style in ("diverse", "translate"):
            r = trial_batch(MOUNTS[mkey], n, meth, trials, seed=7, pose_style=style)
            out[f"{mkey}|{style}"] = r
            print(f"  {mkey:7s} {style:9s} px@3m med "
                  f"{np.median(r['reproj_px_3m']):8.2f}  fail {r['n_fail']}")
    return out


def exp_noise(trials: int) -> dict:
    out = {}
    for mkey, meth, n in (("d6", "board_a1", 12), ("mid360", "board_a1", 8)):
        for mm in NOISE_MM:
            r = trial_batch(MOUNTS[mkey], n, meth, trials, seed=mm,
                            range_sigma_m=mm * 1e-3)
            out[f"{mkey}|{mm}"] = r
            print(f"  {mkey:7s} sigma={mm:2d} mm  px@3m med "
                  f"{np.median(r['reproj_px_3m']):7.2f} p90 "
                  f"{pct(r['reproj_px_3m'], 90):8.2f}")
    return out


# ============================================================== experiment 2

def exp_gate(trials: int) -> dict:
    """Split-half repeatability as the wizard's user-facing quality gate."""
    out = {}
    for mkey, meth, n in (("d6", "board_a2", 12), ("d6", "board_a1", 12),
                          ("mid360", "board_a2", 8), ("mid360", "board_a1", 8)):
        mount = MOUNTS[mkey]
        kind = METHODS[meth][0]
        gates, actual = [], []
        for i in range(trials):
            rng = np.random.default_rng(i * 104729 + 7)
            obs = build_session(mount, n, rng, method=meth)
            if obs is None:
                continue
            res = solve_extrinsic(obs, cad_nominal(mount.T_cl, rng), kind="plane")
            g = split_half_gate(obs, mount, rng, kind)
            if g is None:
                continue
            gates.append(g)
            actual.append(extrinsic_errors(mount.T_cl, res.T_cl)["reproj_px_3m"])
        out[f"{mkey}|{meth}|{n}"] = {"gate": np.array(gates),
                                     "actual": np.array(actual)}
        print(f"  {mkey:7s} {meth:9s} gate med {np.median(gates):8.2f} px  "
              f"actual med {np.median(actual):7.2f} px  "
              f"ratio {np.median(gates) / max(np.median(actual), 1e-9):.2f}")
    return out


def exp_d6_bench(trials: int) -> dict:
    """The D6 cannot be calibrated by a 12-pose handheld wizard at its
    specified range noise. Can a one-off BENCH procedure (tripod, big target,
    many poses) close it without any change to the sensor?"""
    out = {}
    for meth in ("board_a1", "board_xl"):
        for n in BENCH_POSES:
            r = trial_batch(MOUNTS["d6"], n, meth, trials, seed=n)
            out[f"{meth}|{n}"] = r
            print(f"  d6      {meth:9s} N={n:2d}  px@3m med "
                  f"{np.median(r['reproj_px_3m']):7.2f} p90 "
                  f"{pct(r['reproj_px_3m'], 90):7.2f}  "
                  f"rot {np.median(r['rot_deg']):.3f} deg")
    return out


# ============================================================== experiment 3

def exp_budget(pose_results: dict, noise_results: dict, n_mc: int) -> dict:
    out = {"configs": {}}
    for mkey, meth, n, noise, label in BUDGET_CONFIGS:
        key = (f"{mkey}|{noise * 1000:.0f}" if noise is not None
               else f"{mkey}|{meth}|{n}")
        src = noise_results if noise is not None else pose_results
        pool = src[key]["pool"]
        mount = MOUNTS[mkey]
        cfg = {"label": label, "n_pool": len(pool),
               "ext_px3m_med": float(np.median(src[key]["reproj_px_3m"])),
               "by_jitter": {}}
        for jm in B.JITTER_MS:
            sc = B.Scenario(f"{jm:.0f} ms", jm)
            cell = {"by_range": {}, "breakdown": B.breakdown(mount, pool, sc, 3.0, n_mc)}
            for r in B.RANGES_M:
                cell["by_range"][r] = B.summarise(
                    B.monte_carlo(mount, pool, sc, r, n_mc, seed=int(jm * 10 + r)))
            cfg["by_jitter"][jm] = cell
            m3 = cell["by_range"][3.0]
            print(f"  {label:46s} jitter {jm:4.0f} ms  "
                  f"px@3m med {m3['median']:6.2f} p95 {m3['p95']:7.2f}")
        out["configs"][label] = cfg

    # motion gating: how slowly must the user turn for the budget to close?
    out["gating"] = {}
    turns = [0, 5, 10, 15, 20, 30, 45, 60]
    for mkey, meth, n, noise, label in BUDGET_CONFIGS[:1]:
        pool = pose_results[f"{mkey}|{meth}|{n}"]["pool"]
        for jm in B.JITTER_MS:
            vals = []
            for w in turns:
                sc = B.Scenario("", jm, turn_dps=w)
                vals.append(B.summarise(B.monte_carlo(
                    MOUNTS[mkey], pool, sc, 3.0, n_mc, seed=int(jm + w)))["median"])
            out["gating"][jm] = {"turn_dps": turns, "px3m": vals}
    return out


# ================================================================== plotting

def plot_scene():
    """What the two sensors actually get to see on the calibration target."""
    rng = np.random.default_rng(4)
    fig, axes = plt.subplots(1, 3, figsize=(11.2, 4.0))

    # (a) top-down layout of the prescribed wizard viewpoints
    ax = axes[0]
    slots = tg.wizard_slots(8, "diverse")
    poses = [tg.pose_from_slot(s, rng, (1.1, 1.9)) for s in slots]
    ax.plot([-tg.BOARD_A1.width_m / 2, tg.BOARD_A1.width_m / 2], [0, 0],
            color=S.INK, linewidth=3, solid_capstyle="butt")
    ax.text(0, 0.07, "target", ha="center", color=S.INK_2, fontsize=8)
    for i, p in enumerate(poses):
        ax.plot(p.t[0], p.t[2], "o", color=S.SERIES[0], markersize=6,
                markeredgecolor=S.SURFACE, markeredgewidth=1.5)
        fwd = p.R[:, 2] * 0.28
        ax.annotate("", xy=(p.t[0] + fwd[0], p.t[2] + fwd[2]), xytext=(p.t[0], p.t[2]),
                    arrowprops=dict(arrowstyle="->", color=S.INK_MUTED, lw=1))
    ax.set_title("a. Prescribed wizard viewpoints")
    ax.set_xlabel("x (m)")
    ax.set_ylabel("distance from target (m)")
    ax.set_aspect("equal")
    S.tidy(ax)

    # (b, c) lidar footprint on the target
    for ax, mkey, col in ((axes[1], "d6", S.SERIES[1]), (axes[2], "mid360", S.SERIES[2])):
        mount = MOUNTS[mkey]
        bw, bh = tg.BOARD_A1.width_m, tg.BOARD_A1.height_m
        ax.add_patch(plt.Rectangle((-bw / 2, -bh / 2), bw, bh, facecolor="none",
                                   edgecolor=S.INK_MUTED, linewidth=1.2))
        for p in poses[:4]:
            P = tg.observe_board_lidar(p, mount.T_cl, mount.lidar, rng, board=tg.BOARD_A1)
            if P is None:
                continue
            W = (p @ mount.T_cl).apply(P)
            ax.plot(W[:, 0], -W[:, 1], ".", color=col, markersize=2.5, alpha=0.6)
        ax.set_title(f"{'b' if mkey == 'd6' else 'c'}. {mount.lidar.name} returns "
                     f"on the target")
        ax.set_xlabel("x (m)")
        ax.set_aspect("equal")
        ax.set_xlim(-bw / 2 - 0.1, bw / 2 + 0.1)
        ax.set_ylim(-bh / 2 - 0.1, bh / 2 + 0.1)
        S.tidy(ax)
    axes[1].set_ylabel("up (m)")

    fig.suptitle("Simulated calibration wizard: 8 prescribed poses, A1 target",
                 x=0.008, ha="left", fontsize=11.5, weight="600", color=S.INK)
    fig.tight_layout(rect=(0, 0.01, 1, 0.92))
    fig.savefig(PLOTS / "p0_scene.png")
    plt.close(fig)


def plot_poses(res: dict):
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.1), sharey=True)
    for ax, mkey in zip(axes, ("d6", "mid360")):
        mount = MOUNTS[mkey]
        labels = []
        for ci, meth in enumerate(PLOT_METHODS):
            if not method_supported(meth, mount):
                continue
            med, lo, hi = [], [], []
            for n in N_POSES:
                a = res[f"{mkey}|{meth}|{n}"]["reproj_px_3m"]
                med.append(np.median(a))
                lo.append(pct(a, 10))
                hi.append(pct(a, 90))
            c = S.SERIES[ci]
            ax.fill_between(N_POSES, lo, hi, color=c, alpha=0.13, linewidth=0)
            ax.plot(N_POSES, med, "-o", color=c, markeredgecolor=S.SURFACE,
                    markeredgewidth=1.5, label=SHORT[meth])
            labels.append((list(N_POSES), med, SHORT[meth], c))
        ax.set_yscale("log")
        ax.set_xticks(N_POSES)
        ax.set_xlim(2.6, 16.8)
        S.plain_ticks(ax, "y")
        S.label_ends(ax, labels)
        ax.set_xlabel("wizard poses N")
        ax.set_title(f"{'a' if mkey == 'd6' else 'b'}. {mount.lidar.name}")
        S.limit_line(ax, B.ACCEPT_PX, f"colorization budget {B.ACCEPT_PX:.0f} px", x=0.55)
        S.tidy(ax)
    axes[0].set_ylabel("reprojection error at 3 m (px, median)")
    fig.suptitle("Extrinsic accuracy vs number of wizard poses  "
                 "(band = 10th-90th percentile over trials)",
                 x=0.008, ha="left", fontsize=11.5, weight="600", color=S.INK)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(PLOTS / "p1_poses.png")
    plt.close(fig)


def plot_noise(res: dict):
    fig, ax = plt.subplots(figsize=(6.6, 4.2))
    for ci, (mkey, lbl, spec_mm) in enumerate(
            (("d6", "COIN-D6 (A1 board, 12 poses)", 30),
             ("mid360", "Mid-360 (A1 board, 8 poses)", 20))):
        med = [np.median(res[f"{mkey}|{mm}"]["reproj_px_3m"]) for mm in NOISE_MM]
        hi = [pct(res[f"{mkey}|{mm}"]["reproj_px_3m"], 90) for mm in NOISE_MM]
        c = S.SERIES[ci]
        ax.fill_between(NOISE_MM, med, hi, color=c, alpha=0.13, linewidth=0)
        ax.plot(NOISE_MM, med, "-o", color=c, markeredgecolor=S.SURFACE,
                markeredgewidth=1.5, label=lbl)
        i = NOISE_MM.index(spec_mm)
        ax.plot([spec_mm], [med[i]], "o", color=c, markersize=11, alpha=0.28)
        ax.annotate(f"spec {spec_mm} mm\n{med[i]:.0f} px", (spec_mm, med[i]),
                    textcoords="offset points", xytext=(-6, 12), ha="right",
                    fontsize=8, color=c, weight="600")
    ax.set_xscale("log")
    ax.set_yscale("log")
    S.plain_ticks(ax)
    ax.set_xticks(NOISE_MM)
    ax.set_xticklabels([str(m) for m in NOISE_MM])
    ax.set_yticks([1, 2, 5, 10, 20, 50])
    ax.set_xlabel("lidar range noise, 1 sigma (mm)")
    ax.set_ylabel("reprojection error at 3 m (px, median)")
    ax.set_title("Extrinsic accuracy vs lidar range noise")
    S.limit_line(ax, B.ACCEPT_PX, f"colorization budget {B.ACCEPT_PX:.0f} px", x=0.42)
    ax.legend(loc="upper left")
    S.tidy(ax)
    fig.tight_layout()
    fig.savefig(PLOTS / "p2_noise.png")
    plt.close(fig)


def plot_budget_range(bud: dict):
    labels = [c[4] for c in BUDGET_CONFIGS]
    fig, axes = plt.subplots(1, len(labels), figsize=(12.4, 4.1), sharey=True)
    for ax, lbl in zip(axes, labels):
        cfg = bud["configs"][lbl]
        labels = []
        for ci, jm in enumerate(B.JITTER_MS):
            xs = list(B.RANGES_M)
            ys = [cfg["by_jitter"][jm]["by_range"][r]["median"] for r in xs]
            c = S.SERIES[ci]
            ax.plot(xs, ys, "-o", color=c, markeredgecolor=S.SURFACE,
                    markeredgewidth=1.5, label=f"{jm:.0f} ms")
            labels.append((xs, ys, f"  {jm:.0f} ms", c))
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks(list(B.RANGES_M))
        ax.set_xticklabels([f"{r:g}" for r in B.RANGES_M])
        ax.set_xlim(0.85, 14)
        ax.set_ylim(8, 130)
        ax.set_yticks([10, 20, 30, 50, 80, 120])
        S.plain_ticks(ax, "y")
        S.label_ends(ax, labels, dx=0.4)
        ax.set_xlabel("point range (m)")
        ax.set_title(lbl, fontsize=9.5)
        S.limit_line(ax, B.ACCEPT_PX, f"colorization {B.ACCEPT_PX:.0f} px",
                     x=0.015, ha="left")
        S.limit_line(ax, B.AR_ACCEPT_PX, f"AR overlay {B.AR_ACCEPT_PX:.0f} px",
                     color=S.INK_MUTED, x=0.015, ha="left")
        S.tidy(ax)
    axes[0].set_ylabel("total colorization error (px, median)")
    fig.suptitle("Total colorization error vs range, by time-sync jitter  "
                 "(1 m/s walk, 30 deg/s turn)",
                 x=0.006, ha="left", fontsize=11.5, weight="600", color=S.INK)
    fig.tight_layout(rect=(0, 0, 1, 0.91))
    fig.savefig(PLOTS / "p3_budget_range.png")
    plt.close(fig)


TERM_LABELS = {
    "extrinsic": "Extrinsic (wizard)",
    "sync_rot": "Time sync x turn rate",
    "sync_trans": "Time sync x walk speed",
    "rolling_shutter": "Rolling shutter (20 ms)",
    "arcore": "ARCore relative pose",
    "lidar_noise": "Lidar range noise",
}


def plot_breakdown(bud: dict):
    labels = [c[4] for c in BUDGET_CONFIGS[:2]]
    terms = list(B.TERMS)
    fig, axes = plt.subplots(1, 2, figsize=(11.8, 4.4))
    y = np.arange(len(terms))
    h = 0.26
    for ax, lbl in zip(axes, labels):
        cfg = bud["configs"][lbl]
        vmax = 0.0
        for ci, jm in enumerate(B.JITTER_MS):
            bd = cfg["by_jitter"][jm]["breakdown"]
            vals = [bd[t]["median"] for t in terms]
            vmax = max(vmax, max(vals))
            c = S.SERIES[ci]
            ax.barh(y + (ci - 1) * h, vals, height=h - 0.035, color=c,
                    edgecolor=S.SURFACE, linewidth=2, label=f"{jm:.0f} ms jitter")
            for yy, v in zip(y + (ci - 1) * h, vals):
                if v > 0.4:
                    ax.text(v + vmax * 0.014, yy, f"{v:.0f}", va="center",
                            fontsize=7.5, color=S.INK_2)
        ax.set_xlim(0, vmax * 1.13)
        ax.set_yticks(y)
        ax.set_yticklabels([TERM_LABELS[t] for t in terms])
        ax.invert_yaxis()
        ax.set_xlabel("isolated contribution at 3 m (px, median)")
        ax.set_title(lbl, fontsize=9.5)
        ax.axvline(B.ACCEPT_PX, color=S.LIMIT, linewidth=1.4, linestyle=(0, (4, 3)))
        ax.text(B.ACCEPT_PX, 0.015, f" budget {B.ACCEPT_PX:.0f} px", color=S.LIMIT,
                fontsize=8, weight="600", ha="left", va="bottom",
                transform=ax.get_xaxis_transform())
        ax.grid(axis="x", linewidth=0.8, color=S.GRID)
        ax.set_axisbelow(True)
        ax.tick_params(length=0)
    axes[0].legend(loc="lower right")
    fig.suptitle("Where the budget goes: isolated error contributions at 3 m",
                 x=0.006, ha="left", fontsize=11.5, weight="600", color=S.INK)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(PLOTS / "p4_breakdown.png")
    plt.close(fig)


def plot_gating(bud: dict):
    fig, ax = plt.subplots(figsize=(6.8, 4.2))
    labels = []
    for ci, jm in enumerate(B.JITTER_MS):
        g = bud["gating"][jm]
        c = S.SERIES[ci]
        ax.plot(g["turn_dps"], g["px3m"], "-o", color=c,
                markeredgecolor=S.SURFACE, markeredgewidth=1.5, label=f"{jm:.0f} ms")
        labels.append((g["turn_dps"], g["px3m"], f"  {jm:.0f} ms jitter", c))
    ax.set_xlabel("rig turn rate while the keyframe is taken (deg/s)")
    ax.set_ylabel("total colorization error at 3 m (px, median)")
    ax.set_title("Motion gating: how slowly must the user turn?")
    ax.set_xlim(-2, 86)
    S.limit_line(ax, B.ACCEPT_PX, f"colorization budget {B.ACCEPT_PX:.0f} px", x=0.40)
    S.tidy(ax)
    S.label_ends(ax, labels, dx=1.2)
    fig.tight_layout()
    fig.savefig(PLOTS / "p5_gating.png")
    plt.close(fig)


def plot_d6_bench(bench: dict):
    fig, ax = plt.subplots(figsize=(6.8, 4.2))
    labels = []
    for ci, meth in enumerate(("board_a1", "board_xl")):
        med = [np.median(bench[f"{meth}|{n}"]["reproj_px_3m"]) for n in BENCH_POSES]
        hi = [pct(bench[f"{meth}|{n}"]["reproj_px_3m"], 90) for n in BENCH_POSES]
        c = S.SERIES[ci]
        ax.fill_between(BENCH_POSES, med, hi, color=c, alpha=0.13, linewidth=0)
        ax.plot(BENCH_POSES, med, "-o", color=c, markeredgecolor=S.SURFACE,
                markeredgewidth=1.5, label=SHORT[meth])
        labels.append((list(BENCH_POSES), med, f"  {SHORT[meth]}", c))
    ax.set_yscale("log")
    ax.set_xticks(BENCH_POSES)
    ax.set_xlim(10, 56)
    ax.set_yticks([5, 10, 20, 40, 80])
    S.plain_ticks(ax, "y")
    ax.set_xlabel("poses captured")
    ax.set_ylabel("reprojection error at 3 m (px, median)")
    ax.set_title("COIN-D6 at its specified 30 mm range noise:\n"
                 "a bench procedure closes what a handheld wizard cannot")
    S.limit_line(ax, B.ACCEPT_PX, f"colorization budget {B.ACCEPT_PX:.0f} px", x=0.44)
    S.tidy(ax)
    S.label_ends(ax, labels, dx=0.8)
    ax.axvspan(10, 13, color=S.INK_MUTED, alpha=0.08, linewidth=0)
    ax.text(11.5, ax.get_ylim()[1] * 0.75, "handheld\nwizard", fontsize=8,
            color=S.INK_MUTED, ha="center", va="top")
    fig.tight_layout()
    fig.savefig(PLOTS / "p6_d6_bench.png")
    plt.close(fig)


# =================================================================== tables

def write_tables(poses, styles, noise, bench, gate, bud, meta):
    L = ["# S6 generated results", "",
         f"_Generated by `run_all.py` ({meta['trials']} trials/cell, "
         f"{meta['mc']} Monte-Carlo samples/cell, {meta['runtime_s']:.0f} s)._",
         f"_Camera: {CAM.width}x{CAM.height}, {CAM.focal_equiv_mm:.0f} mm-equiv, "
         f"fx = {CAM.fx:.0f} px, HFOV {CAM.hfov_deg:.1f} deg. "
         f"Acceptance = 0.5% of width = {B.ACCEPT_PX:.1f} px at 3 m._", ""]

    L += ["## T1. Extrinsic error vs number of wizard poses", "",
          "Reprojection error at 3 m, in pixels (median / 90th pct), "
          "with rotation and translation error at the median.", "",
          "| Sensor | Target | N | rot (deg) | trans (mm) | px @1 m | px @3 m | p90 @3 m | px @8 m |",
          "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for mkey, mount in MOUNTS.items():
        for meth in PLOT_METHODS:
            if not method_supported(meth, mount):
                continue
            for n in N_POSES:
                r = poses[f"{mkey}|{meth}|{n}"]
                L.append(f"| {mount.lidar.name} | {METHODS[meth][2]} | {n} | "
                         f"{np.median(r['rot_deg']):.3f} | {np.median(r['trans_mm']):.1f} | "
                         f"{np.median(r['reproj_px_1m']):.1f} | "
                         f"**{np.median(r['reproj_px_3m']):.1f}** | "
                         f"{pct(r['reproj_px_3m'], 90):.1f} | "
                         f"{np.median(r['reproj_px_8m']):.1f} |")

    L += ["", "## T2. Pose diversity matters", "",
          "| Sensor | Pose style | px @3 m (median) | p90 |",
          "| --- | --- | ---: | ---: |"]
    for k, r in styles.items():
        mkey, st = k.split("|")
        nice = {"diverse": "varied azimuth + elevation + ROLL",
                "translate": "sideways steps only, phone upright"}[st]
        L.append(f"| {MOUNTS[mkey].lidar.name} | {nice} | "
                 f"{np.median(r['reproj_px_3m']):.1f} | {pct(r['reproj_px_3m'], 90):.1f} |")

    L += ["", "## T3. Extrinsic error vs lidar range noise", "",
          "| Sensor | 1-sigma range noise | px @3 m (median) | p90 |",
          "| --- | ---: | ---: | ---: |"]
    for mkey in ("d6", "mid360"):
        for mm in NOISE_MM:
            r = noise[f"{mkey}|{mm}"]
            tag = " **(spec)**" if (mkey, mm) in (("d6", 30), ("mid360", 20)) else ""
            L.append(f"| {MOUNTS[mkey].lidar.name} | {mm} mm{tag} | "
                     f"{np.median(r['reproj_px_3m']):.1f} | {pct(r['reproj_px_3m'], 90):.1f} |")

    L += ["", "## T4. COIN-D6 bench calibration (tripod, many poses, 30 mm noise)", "",
          "The D6 at its specified range noise cannot be calibrated by a "
          "handheld 12-pose wizard. A one-off bench procedure can.", "",
          "| Target | Poses | px @3 m (median) | p90 | rot (deg) |",
          "| --- | ---: | ---: | ---: | ---: |"]
    for meth in ("board_a1", "board_xl"):
        for n in BENCH_POSES:
            r = bench[f"{meth}|{n}"]
            L.append(f"| {METHODS[meth][2]} | {n} | "
                     f"**{np.median(r['reproj_px_3m']):.1f}** | "
                     f"{pct(r['reproj_px_3m'], 90):.1f} | "
                     f"{np.median(r['rot_deg']):.3f} |")

    L += ["", "## T5. Split-half quality gate", "",
          "Gate = how far apart two independent halves of the capture place a "
          "point at 3 m. It tracks the true error in SCALE (usable as a "
          "pass/fail threshold), roughly 2x it.", "",
          "| Sensor | Target | Gate (px, median) | True error (px, median) | Gate / true |",
          "| --- | --- | ---: | ---: | ---: |"]
    for k, v in gate.items():
        mkey, meth, n = k.split("|")
        gm, am = np.median(v["gate"]), np.median(v["actual"])
        L.append(f"| {MOUNTS[mkey].lidar.name} | {METHODS[meth][2]}, {n} poses | "
                 f"{gm:.1f} | {am:.1f} | {gm / max(am, 1e-9):.2f} |")

    L += ["", "## T6. Colorization error budget (TOTAL)", "",
          "Median total error, 1 m/s walk + 30 deg/s turn, all terms active.", "",
          "| Configuration | Jitter | px @1 m | px @3 m | p95 @3 m | px @8 m | 3 m verdict |",
          "| --- | ---: | ---: | ---: | ---: | ---: | --- |"]
    for _, _, _, _, lbl in BUDGET_CONFIGS:
        cfg = bud["configs"][lbl]
        for jm in B.JITTER_MS:
            c = cfg["by_jitter"][jm]
            m3 = c["by_range"][3.0]
            v = ("PASS" if m3["median"] <= B.ACCEPT_PX
                 else ("AR only" if m3["median"] <= B.AR_ACCEPT_PX else "FAIL"))
            L.append(f"| {lbl} | {jm:.0f} ms | {c['by_range'][1.0]['median']:.1f} | "
                     f"**{m3['median']:.1f}** | {m3['p95']:.1f} | "
                     f"{c['by_range'][8.0]['median']:.1f} | {v} |")

    L += ["", "## T7. Budget breakdown at 3 m (isolated contributions, px median)", "",
          "| Configuration | Jitter | " + " | ".join(TERM_LABELS[t] for t in B.TERMS)
          + " | TOTAL |",
          "| --- | ---: |" + " ---: |" * (len(B.TERMS) + 1)]
    for _, _, _, _, lbl in BUDGET_CONFIGS[:2]:
        for jm in B.JITTER_MS:
            bd = bud["configs"][lbl]["by_jitter"][jm]["breakdown"]
            row = " | ".join(f"{bd[t]['median']:.1f}" for t in B.TERMS)
            L.append(f"| {lbl} | {jm:.0f} ms | {row} | **{bd['TOTAL']['median']:.1f}** |")

    L += ["", "## T8. Motion gating (Mid-360, recommended calibration)", "",
          "Median total error at 3 m as a function of how fast the rig is "
          "turning when the keyframe is taken.", "",
          "| Turn rate | " + " | ".join(f"{j:.0f} ms jitter" for j in B.JITTER_MS) + " |",
          "| ---: |" + " ---: |" * len(B.JITTER_MS)]
    turns = bud["gating"][B.JITTER_MS[0]]["turn_dps"]
    for i, w in enumerate(turns):
        L.append(f"| {w} deg/s | "
                 + " | ".join(f"{bud['gating'][j]['px3m'][i]:.1f}" for j in B.JITTER_MS)
                 + " |")

    (RESULTS / "tables.md").write_text("\n".join(L) + "\n")


def to_jsonable(o):
    if isinstance(o, dict):
        return {str(k): to_jsonable(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [to_jsonable(v) for v in o]
    if isinstance(o, np.ndarray):
        return {"median": float(np.median(o)), "p90": pct(o, 90),
                "p95": pct(o, 95), "n": int(o.size)} if o.ndim == 1 else None
    if isinstance(o, (np.floating, np.integer)):
        return float(o)
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="fast smoke run")
    ap.add_argument("--trials", type=int, default=None)
    args = ap.parse_args()
    trials = args.trials or (10 if args.quick else 40)
    mc = 8_000 if args.quick else 40_000

    PLOTS.mkdir(exist_ok=True)
    RESULTS.mkdir(exist_ok=True)
    S.apply()
    t0 = time.time()

    print(f"\nS6 calibration spike -- {trials} trials/cell, {mc} MC samples/cell")
    print(f"camera: {CAM.width}x{CAM.height}  fx={CAM.fx:.0f}px  "
          f"HFOV={CAM.hfov_deg:.1f}deg  acceptance={B.ACCEPT_PX:.1f}px\n")

    print("[1/5] extrinsic accuracy vs wizard poses")
    poses = exp_poses(trials)
    print("\n[2/5] pose-diversity ablation")
    styles = exp_pose_style(trials)
    print("\n[3/5] extrinsic accuracy vs lidar range noise")
    noise = exp_noise(trials)
    print("\n[4/6] D6 bench calibration (many poses, tripod)")
    bench = exp_d6_bench(max(trials // 2, 12))
    print("\n[5/6] split-half quality gate")
    gate = exp_gate(max(trials, 20))
    print("\n[6/6] colorization error budget")
    bud = exp_budget(poses, noise, mc)

    print("\nplotting ...")
    for name, fn, arg in (("p0_scene", plot_scene, None),
                          ("p1_poses", plot_poses, poses),
                          ("p2_noise", plot_noise, noise),
                          ("p3_budget_range", plot_budget_range, bud),
                          ("p4_breakdown", plot_breakdown, bud),
                          ("p5_gating", plot_gating, bud),
                          ("p6_d6_bench", plot_d6_bench, bench)):
        t = time.time()
        fn() if arg is None else fn(arg)
        print(f"  {name}: {time.time() - t:.1f} s")

    meta = {"trials": trials, "mc": mc, "runtime_s": time.time() - t0}
    write_tables(poses, styles, noise, bench, gate, bud, meta)
    (RESULTS / "results.json").write_text(json.dumps(
        {"meta": meta, "budget": to_jsonable(bud)}, indent=1))

    print(f"\ndone in {meta['runtime_s']:.0f} s")
    print(f"  plots   -> {PLOTS}")
    print(f"  tables  -> {RESULTS / 'tables.md'}")


if __name__ == "__main__":
    main()
