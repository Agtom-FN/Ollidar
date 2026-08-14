"""Wizard target models and the synthetic observations they produce.

World frame used throughout this module (== the calibration-target frame):
    origin = centre of the target, on the wall
    +x right along the wall, +y DOWN (gravity), +z out of the wall into the room
so the target plane is  z = 0  with normal +z, and the rig stands at z > 0.
The room is the box  x in [-3,3], y in [-1.5, 1.2], z in [0, 5].

Two candidate targets are simulated, because the wizard design has to choose
between them (spec 3.5: "checkerboard/corner refinement for Mid-360"):

  PLANE  -- a printed checkerboard on a rigid backing, standing clear of the
            wall. Camera: detect corners, solve PnP -> board plane in the camera
            frame. Lidar: the points that land on the board. Residual = each
            lidar point must lie on the camera-observed plane
            (Zhang & Pless 2004 / Unnikrishnan & Hebert 2005).

  CORNER -- a doorframe, i.e. no printed target at all. Camera: the frame
            vertices triangulated across ARCore keyframes -> metric but noisy
            3D points. Lidar: the same vertices from plane/line intersection.
            Residual = 3D point-to-point.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.optimize import least_squares

from .geom import SE3, look_at
from .rig import Camera, LidarSpec

WORLD_UP = np.array([0.0, -1.0, 0.0])       # +y is down in this world


# ----------------------------------------------------------------- room scene

@dataclass(frozen=True)
class Room:
    """The synthetic room the rig stands in. Surfaces only -- used for scene
    plots and for the colorization geometry sanity check."""
    x: tuple = (-3.0, 3.0)
    y: tuple = (-1.5, 1.2)      # ceiling .. floor  (+y down)
    z: tuple = (0.0, 5.0)       # target wall .. far wall

    def sample_points(self, n: int, rng: np.random.Generator) -> np.ndarray:
        lo = np.array([self.x[0], self.y[0], self.z[0]])
        hi = np.array([self.x[1], self.y[1], self.z[1]])
        face = rng.integers(0, 6, n)
        P = rng.uniform(lo, hi, size=(n, 3))
        for f, ax, val in ((0, 0, lo[0]), (1, 0, hi[0]), (2, 1, lo[1]),
                           (3, 1, hi[1]), (4, 2, lo[2]), (5, 2, hi[2])):
            P[face == f, ax] = val
        return P


ROOM = Room()


# --------------------------------------------------------------- plane target

@dataclass(frozen=True)
class Checkerboard:
    """A2-printable board, 8x5 inner corners at 65 mm (520 x 260 mm pattern).
    Big enough for the Mid-360 to land a few hundred points on it at 1.5 m,
    small enough for one person to carry and mount on a light stand."""
    cols: int = 8
    rows: int = 5
    square_m: float = 0.065
    margin_m: float = 0.040

    @property
    def width_m(self) -> float:
        return (self.cols - 1) * self.square_m + 2 * self.margin_m

    @property
    def height_m(self) -> float:
        return (self.rows - 1) * self.square_m + 2 * self.margin_m

    def corners_board(self) -> np.ndarray:
        xs = (np.arange(self.cols) - (self.cols - 1) / 2) * self.square_m
        ys = (np.arange(self.rows) - (self.rows - 1) / 2) * self.square_m
        gx, gy = np.meshgrid(xs, ys)
        return np.column_stack([gx.ravel(), gy.ravel(), np.zeros(gx.size)])


BOARD = Checkerboard()

# Larger printed/plywood variants, to test whether target EXTENT is what the
# 2D sensor is actually short of.
BOARD_A1 = Checkerboard(cols=8, rows=6, square_m=0.100, margin_m=0.050)   # 0.80 x 0.60 m
BOARD_XL = Checkerboard(cols=9, rows=7, square_m=0.130, margin_m=0.060)   # 1.16 x 0.90 m


@dataclass(frozen=True)
class WallPatch:
    """A bare wall / doorframe reveal: no printed target at all. Huge extent,
    but the camera can only recover its plane from ARCore plane detection,
    which is far less accurate than checkerboard PnP."""
    width_m: float = 4.0
    height_m: float = 2.4
    normal_sigma_deg: float = 1.5
    offset_sigma_m: float = 0.015

# A 0.90 x 2.05 m doorway on the target wall (z = 0), vertices in world coords.
DOORFRAME_CORNERS = np.array([
    [-0.45, -0.85, 0.0],
    [0.45, -0.85, 0.0],
    [-0.45, 1.20, 0.0],
    [0.45, 1.20, 0.0],
])


# ------------------------------------------------------------- wizard poses

def wizard_slots(n: int, style: str = "diverse") -> np.ndarray:
    """The N viewpoints the wizard PRESCRIBES, as (azimuth, elevation, roll)
    in degrees. These are the on-screen instructions, not the user's execution.

    style="diverse"   : azimuth, elevation AND roll all varied -- what a
                        well-designed wizard must instruct. Roll matters
                        enormously for the 2D sensor: rolling the phone is the
                        only thing that changes the direction of the D6 scan
                        line across the target.
    style="translate" : the user only steps sideways / back with the phone held
                        upright. The realistic failure mode of a lazy wizard;
                        simulated to show it is not observable enough.
    """
    if style == "diverse":
        # R3 low-discrepancy sequence: evenly and decorrelatedly covers the
        # (azimuth, elevation, roll) cube for ANY n -- which is exactly what the
        # wizard wants when it has to prescribe 3 viewpoints or 12.
        i = np.arange(n) + 1
        a = np.array([0.8191725134, 0.6710436067, 0.5497004779])   # 1/g, 1/g^2, 1/g^3
        u = np.mod(0.5 + i[:, None] * a[None, :], 1.0)
        az = -38 + 76 * u[:, 0]
        el = -24 + 50 * u[:, 1]
        roll = -60 + 120 * u[:, 2]
    elif style == "translate":
        az = np.linspace(-16, 16, n)
        el = np.full(n, 4.0)
        roll = np.zeros(n)
    else:
        raise ValueError(style)
    return np.column_stack([az, el, roll])


def pose_from_slot(slot, rng: np.random.Generator, dist_range=(1.1, 1.9),
                   jitter_deg: float = 5.0) -> SE3:
    """One rig pose (world<-camera): the prescribed slot plus how sloppily a
    human actually hits it."""
    a, e, r = np.radians(slot[0] + rng.normal(0, jitter_deg)), \
        np.radians(slot[1] + rng.normal(0, jitter_deg)), \
        float(slot[2] + rng.normal(0, jitter_deg))
    d = rng.uniform(*dist_range)
    # spherical about the target in the z>0 half space; +y is down, so positive
    # elevation puts the rig ABOVE the target centre.
    eye = d * np.array([np.sin(a) * np.cos(e), -np.sin(e), np.cos(a) * np.cos(e)])
    aim = rng.normal(0, 0.05, 3) * np.array([1.0, 1.0, 0.0])
    return look_at(eye, aim, roll_deg=r, world_up=WORLD_UP)


def wizard_poses(n: int, rng: np.random.Generator, style: str = "diverse",
                 dist_range=(1.1, 1.9)) -> list[SE3]:
    return [pose_from_slot(s, rng, dist_range) for s in wizard_slots(n, style)]


# ------------------------------------------------- camera-side measurements

def observe_board_camera(T_wc: SE3, cam: Camera, rng: np.random.Generator,
                         corner_px_sigma: float = 0.3, board: Checkerboard = BOARD):
    """Detect the checkerboard and solve PnP from noisy corner pixels.

    Returns (n_cam, d_cam) with  n . X = d  for X on the board plane in camera
    coordinates, or None if the board is not usably detectable from this pose.
    """
    T_cw = T_wc.inv()                       # board frame == world frame
    P_b = board.corners_board()
    X_cam = T_cw.apply(P_b)
    if not cam.in_view(X_cam, margin=40).all():
        return None

    n_true = T_cw.R @ np.array([0.0, 0.0, 1.0])
    view = X_cam / np.linalg.norm(X_cam, axis=1, keepdims=True)
    incidence = np.degrees(np.arccos(np.clip(np.abs(view @ n_true), 0, 1))).mean()
    if incidence > 62.0:                    # too grazing: detector accuracy collapses
        return None

    uv = cam.project(X_cam) + rng.normal(0, corner_px_sigma, (len(P_b), 2))

    def resid(v):
        return (cam.project(SE3.from_vec6(v).apply(P_b)) - uv).ravel()

    seed = T_cw.as_vec6() + np.concatenate([rng.normal(0, 0.02, 3), rng.normal(0, 0.02, 3)])
    sol = least_squares(resid, seed, method="lm", xtol=1e-12, ftol=1e-12, max_nfev=200)
    T_cb = SE3.from_vec6(sol.x)

    n = T_cb.R @ np.array([0.0, 0.0, 1.0])
    d = float(n @ T_cb.t)
    if d < 0:
        n, d = -n, -d
    return n, d


def observe_wall_camera(T_wc: SE3, rng: np.random.Generator,
                        wall: WallPatch = WallPatch()):
    """The target plane as ARCore's plane detector delivers it: right plane,
    but a degree or so of normal error and ~15 mm of offset error. No printed
    target needed -- this is the 'just point it at a doorframe' option."""
    from .geom import random_small_rotation
    T_cw = T_wc.inv()
    n = T_cw.R @ np.array([0.0, 0.0, 1.0])
    d = float(n @ T_cw.t)
    if d < 0:
        n, d = -n, -d
    n = random_small_rotation(wall.normal_sigma_deg, rng) @ n
    return n / np.linalg.norm(n), d + rng.normal(0, wall.offset_sigma_m)


def observe_corner_camera(T_wc: SE3, cam: Camera, rng: np.random.Generator,
                          corners_world: np.ndarray = DOORFRAME_CORNERS,
                          triang_sigma_m: float = 0.015):
    """Doorframe vertices as the camera pipeline delivers them: triangulated
    across ARCore keyframes, so metric but noisy (~15 mm laterally, ~2x that
    along the viewing ray where the triangulation baseline is short)."""
    X_cam = T_wc.inv().apply(corners_world)
    vis = cam.in_view(X_cam, margin=60)
    if vis.sum() < 3:
        return None
    X = X_cam[vis]
    ray = X / np.linalg.norm(X, axis=1, keepdims=True)
    X_noisy = (X + rng.normal(0, triang_sigma_m, X.shape)
               + rng.normal(0, triang_sigma_m * 2.0, (len(X), 1)) * ray)
    return X_noisy, vis


# -------------------------------------------------- lidar-side measurements

def _clip_line_to_rect(base: np.ndarray, dirv: np.ndarray, hw: float, hh: float):
    """Interval of t for which base + t*dir stays inside |x|<hw, |y|<hh."""
    t_lo, t_hi = -1e9, 1e9
    for ax, half in ((0, hw), (1, hh)):
        b, dv = base[ax], dirv[ax]
        if abs(dv) < 1e-9:
            if abs(b) > half:
                return None
            continue
        a, c = (-half - b) / dv, (half - b) / dv
        t_lo, t_hi = max(t_lo, min(a, c)), min(t_hi, max(a, c))
    return (t_lo, t_hi) if t_hi - t_lo > 0.05 else None


def observe_board_lidar(T_wc: SE3, T_cl: SE3, spec: LidarSpec,
                        rng: np.random.Generator, board: Checkerboard = BOARD):
    """Lidar returns that land on the board, in the LIDAR frame, with range
    noise. A 2D sensor returns a LINE of points (scan-plane / board
    intersection); a 3D sensor returns a patch."""
    T_wl = T_wc @ T_cl
    T_lw = T_wl.inv()
    hw = board.width_m / 2 - 0.025          # segmentation drops edge/mixed returns
    hh = board.height_m / 2 - 0.025
    if hw <= 0 or hh <= 0:
        return None

    if spec.kind == "2d":
        o, R = T_wl.t, T_wl.R
        e1, e2 = R[:, 0], R[:, 1]           # the scan plane (lidar z = 0) in world
        nz = np.array([e1[2], e2[2]])
        if np.linalg.norm(nz) < 1e-6:
            return None                     # scan plane parallel to the board
        base_ab = -o[2] * nz / (nz @ nz)
        base_w = o + base_ab[0] * e1 + base_ab[1] * e2
        dab = np.array([-nz[1], nz[0]]) / np.linalg.norm(nz)
        dir_w = dab[0] * e1 + dab[1] * e2
        dir_w = dir_w / np.linalg.norm(dir_w)
        seg = _clip_line_to_rect(base_w, dir_w, hw, hh)
        if seg is None:
            return None
        t0, t1 = seg
        mid = base_w + 0.5 * (t0 + t1) * dir_w
        rmid = np.linalg.norm(mid - o)
        step = 2 * rmid * np.tan(np.radians(spec.ang_res_deg) / 2)
        # 10 Hz rotation x dwell => the same profile is re-observed dwell*10 times
        n_line = int(np.clip((t1 - t0) / max(step, 1e-4), 3, spec.max_target_pts))
        reps = max(1, int(spec.dwell_s * 10))
        tt = np.concatenate([np.linspace(t0, t1, n_line)
                             + rng.uniform(-step / 2, step / 2) for _ in range(reps)])
        tt = np.clip(tt, t0, t1)[:spec.max_target_pts]
        P_w = base_w[None, :] + tt[:, None] * dir_w[None, :]
    else:
        k = spec.max_target_pts
        P_w = np.column_stack([rng.uniform(-hw, hw, k), rng.uniform(-hh, hh, k), np.zeros(k)])

    P_l = T_lw.apply(P_w)
    r = np.linalg.norm(P_l, axis=1, keepdims=True)
    return P_l + (P_l / r) * rng.normal(0, spec.range_sigma_m, (len(P_l), 1))


def observe_corner_lidar(T_wc: SE3, T_cl: SE3, spec: LidarSpec,
                         rng: np.random.Generator, vis_mask: np.ndarray,
                         corners_world: np.ndarray = DOORFRAME_CORNERS):
    """Doorframe vertices recovered by the lidar.

    3D lidar: the vertex comes from intersecting fitted planes/edges over
    hundreds of returns, so it beats raw range noise by ~sqrt(N), with a small
    irreducible fitting/edge bias.
    2D lidar: only the BEND in the scan profile is available, fitted from ~15
    returns per side, so it is barely better than raw range noise.
    """
    T_wl = T_wc @ T_cl
    P_l = T_wl.inv().apply(corners_world[vis_mask])
    sigma = (spec.range_sigma_m / np.sqrt(150.0) + 0.004 if spec.kind == "3d"
             else spec.range_sigma_m * 0.8 + 0.006)
    return P_l + rng.normal(0, sigma, P_l.shape), sigma
