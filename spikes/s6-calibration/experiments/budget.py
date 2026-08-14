"""Colorization error budget.

Propagates every error source into the ONE number the product is judged on:
how far, in pixels, a lidar point lands from where its colour actually is.

The chain, for a point at range r that a keyframe is about to colour:

    p_lidar --[extrinsic Test]--> camera frame
            --[rig moved during the time-sync error]-->
            --[ARCore relative pose error, keyframe vs trajectory]-->
            --> project --> compare against the truth projection

Physics worth stating explicitly, because it decides the verdict:

  * A ROTATION error (extrinsic, sync-during-turn, ARCore) displaces the point
    laterally by theta * r, and the projection divides by r again -- so its
    pixel cost is RANGE-INDEPENDENT: err_px = f * theta.
  * A TRANSLATION error displaces by a fixed distance, so its pixel cost DECAYS
    with range: err_px = f * |dt_perp| / r. Only the component perpendicular to
    the viewing ray counts -- walking straight at a point you are colouring
    costs almost nothing, which is why the raw f*v*dt/r figure overstates it.
  * Time-sync error is BOTH: during dt the rig translates by v*dt and rotates by
    w*dt. At a 30 deg/s turn the rotation half dominates everything else.
  * ROLLING SHUTTER is a time-sync error in disguise: the effective exposure
    time varies down the image by the readout time, so it behaves like an extra
    per-row time offset. It is included, and it is correctable.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.spatial.transform import Rotation

from sim.rig import CAM, Camera, MountCase

# --- spec'd operating point (task brief + spec 3.2) --------------------------
JITTER_MS = (5.0, 15.0, 30.0)
WALK_SPEED = 1.0          # m/s forward
TURN_RATE = 30.0          # deg/s
RANGES_M = (1.0, 3.0, 8.0)

# ARCore pose noise, absolute (~1 cm / 0.5 deg). What actually matters for
# colorization is the RELATIVE error between the trajectory pose used to place
# the point and the keyframe pose used to view it -- typically < 0.5 s apart, so
# the slow-drift part is common-mode and cancels. For two errors of std s and
# correlation rho, the relative std is s*sqrt(2(1-rho)).
ARCORE_ABS_TRANS_M = 0.010
ARCORE_ABS_ROT_DEG = 0.5
ARCORE_LOCAL_RHO = 0.95   # sub-second correlation; 0.0 = fully independent

# Phone full-res rolling-shutter readout. Acts like a row-dependent time offset.
ROLLING_SHUTTER_MS = 20.0

ACCEPT_FRACTION = 0.005                       # spec'd: 0.5% of image width
ACCEPT_PX = ACCEPT_FRACTION * CAM.width       # = 20.2 px
AR_ACCEPT_FRACTION = 0.015                    # see REPORT.md for the rationale
AR_ACCEPT_PX = AR_ACCEPT_FRACTION * CAM.width


def arcore_relative(rho: float = ARCORE_LOCAL_RHO):
    f = np.sqrt(2.0 * (1.0 - rho))
    return ARCORE_ABS_TRANS_M * f, ARCORE_ABS_ROT_DEG * f


@dataclass
class Scenario:
    label: str
    jitter_ms: float
    speed_mps: float = WALK_SPEED
    turn_dps: float = TURN_RATE
    rolling_shutter_ms: float = ROLLING_SHUTTER_MS
    arcore_rho: float = ARCORE_LOCAL_RHO


TERMS = ("extrinsic", "sync_rot", "sync_trans", "rolling_shutter",
         "arcore", "lidar_noise")


def _sample_motion(n, sc: Scenario, rng):
    """Rig velocity and angular velocity in the camera frame.

    Handheld walking: mostly forward (+z), with gait sway (lateral) and bob
    (vertical). Turning is mostly yaw about the vertical axis, which in this
    camera frame is +y (y points down), plus some pitch/roll wobble.
    """
    v = np.column_stack([rng.normal(0, 0.20, n),            # lateral sway
                         rng.normal(0, 0.25, n),            # vertical bob
                         np.full(n, sc.speed_mps)])         # forward
    w_axis = np.column_stack([rng.normal(0, 0.20, n),
                              rng.choice([-1.0, 1.0], n),   # yaw, either way
                              rng.normal(0, 0.20, n)])
    w_axis /= np.linalg.norm(w_axis, axis=1, keepdims=True)
    w = w_axis * np.radians(sc.turn_dps)
    return v, w


def _apply_se3_batch(R_batch, t_batch, X):
    return np.einsum("nij,nj->ni", R_batch, X) + t_batch


def monte_carlo(mount: MountCase, ext_pool: np.ndarray, sc: Scenario,
                range_m: float, n: int = 40_000, cam: Camera = CAM,
                seed: int = 0, terms: tuple = TERMS) -> np.ndarray:
    """Pixel colorization error for `n` random points at `range_m`.

    `ext_pool` is an (M,6) array of extrinsic ERROR transforms (se3 vectors, in
    the lidar frame) drawn from the solver experiments, so the budget inherits
    the real, non-Gaussian spread the wizard actually produces.
    `terms` selects which error sources are active -- pass a single term to get
    that term's isolated contribution.
    """
    rng = np.random.default_rng(seed)
    T = mount.T_cl

    d = cam.sample_fov_directions(n, rng)
    X_true = d * range_m                                  # truth, camera frame
    p_l = T.inv().apply(X_true)                           # what the lidar saw

    # 1. lidar range noise (radial in the lidar frame)
    p = p_l.copy()
    if "lidar_noise" in terms:
        rr = np.linalg.norm(p, axis=1, keepdims=True)
        p = p + (p / rr) * rng.normal(0, mount.lidar.range_sigma_m, (n, 1))

    # 2. extrinsic error, applied in the lidar frame: X = T_true . E . p
    if "extrinsic" in terms and len(ext_pool):
        e = ext_pool[rng.integers(0, len(ext_pool), n)]
        p = _apply_se3_batch(Rotation.from_rotvec(e[:, :3]).as_matrix(), e[:, 3:], p)
    X = p @ T.R.T + T.t

    # 3. time-sync: the rig moved during the timestamp error, so the point is
    #    placed relative to the wrong rig pose.
    v, w = _sample_motion(n, sc, rng)
    dt = np.zeros(n)
    if "sync_rot" in terms or "sync_trans" in terms:
        dt = rng.normal(0, sc.jitter_ms * 1e-3, n)
    if "rolling_shutter" in terms:
        # effective exposure instant varies linearly down the image
        row = cam.project(X_true)[:, 1] / cam.height
        dt = dt + (row - 0.5) * sc.rolling_shutter_ms * 1e-3

    rot_dt = dt if ("sync_rot" in terms or "rolling_shutter" in terms) else np.zeros(n)
    tr_dt = dt if ("sync_trans" in terms or "rolling_shutter" in terms) else np.zeros(n)
    R_sync = Rotation.from_rotvec(w * rot_dt[:, None]).as_matrix()
    X = _apply_se3_batch(R_sync, v * tr_dt[:, None], X)

    # 4. ARCore relative pose error between trajectory pose and keyframe pose
    if "arcore" in terms:
        s_t, s_r = arcore_relative(sc.arcore_rho)
        R_ar = Rotation.from_rotvec(rng.normal(0, np.radians(s_r), (n, 3))).as_matrix()
        X = _apply_se3_batch(R_ar, rng.normal(0, s_t, (n, 3)), X)

    uv = cam.project(X)
    return np.linalg.norm(uv - cam.project(X_true), axis=1)


def summarise(err_px: np.ndarray) -> dict:
    return {"median": float(np.median(err_px)),
            "rms": float(np.sqrt(np.mean(err_px ** 2))),
            "p95": float(np.percentile(err_px, 95))}


def breakdown(mount: MountCase, ext_pool: np.ndarray, sc: Scenario,
              range_m: float, n: int = 40_000, seed: int = 0) -> dict:
    """Isolated contribution of each term, plus the all-terms total."""
    out = {t: summarise(monte_carlo(mount, ext_pool, sc, range_m, n,
                                    seed=seed + i, terms=(t,)))
           for i, t in enumerate(TERMS)}
    out["TOTAL"] = summarise(monte_carlo(mount, ext_pool, sc, range_m, n, seed=seed + 99))
    return out
