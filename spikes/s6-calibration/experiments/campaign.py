"""Assemble a full simulated wizard session and solve it."""

from __future__ import annotations

import dataclasses

import numpy as np

from sim import targets as tg
from sim.geom import SE3, random_small_rotation, rot_angle_deg
from sim.rig import CAM, MOUNTS, MountCase, reprojection_error_from_extrinsic
from solver.extrinsic import PlaneObs, PointObs, solve_extrinsic

# The bracket's CAD nominal: what the wizard starts the solve from. A machined
# or 3D-printed phone bracket is good to a few degrees and a couple of cm.
CAD_ROT_ERR_DEG = 4.0
CAD_TRANS_ERR_MM = 25.0

# Method keys used across the experiments.
METHODS = {
    "board_a2": ("plane", tg.BOARD, "A2 checkerboard (0.54 x 0.34 m)"),
    "board_a1": ("plane", tg.BOARD_A1, "A1 checkerboard (0.80 x 0.60 m)"),
    "board_xl": ("plane", tg.BOARD_XL, "XL board (1.16 x 0.90 m)"),
    "wall": ("wall", tg.WallPatch(), "Bare wall / doorframe reveal (ARCore plane)"),
    "corner": ("corner", None, "Doorframe corner vertices (3D lidar only)"),
}


def cad_nominal(T_true: SE3, rng: np.random.Generator) -> SE3:
    return SE3(T_true.R @ random_small_rotation(CAD_ROT_ERR_DEG, rng),
               T_true.t + rng.normal(0, CAD_TRANS_ERR_MM * 1e-3, 3))


def method_supported(method: str, mount: MountCase) -> bool:
    """A 2D scanner samples a single 1D slice of the scene, so the features it
    finds at a 'corner' (the bend in its profile) are NOT repeatable world
    points -- they slide along the corner line as the rig moves. Point-to-point
    corner correspondence is therefore available to the 3D sensor only.
    Note this contradicts spec 3.3, which proposes corner/doorframe capture for
    the D6 wizard; see REPORT.md.
    """
    return not (method == "corner" and mount.lidar.kind == "2d")


def build_session(mount: MountCase, n_poses: int, rng: np.random.Generator,
                  method: str = "board_a2", pose_style: str = "diverse",
                  range_sigma_m: float | None = None, retries: int = 6):
    """Simulate the CAPTURE half of a wizard session. Returns the observation
    list (or None if the user could not complete the prescribed poses)."""
    if not method_supported(method, mount):
        return None
    kind, target, _ = METHODS[method]
    spec = mount.lidar
    if range_sigma_m is not None:
        spec = dataclasses.replace(spec, range_sigma_m=range_sigma_m)

    dist = {"plane": (1.1, 1.9), "wall": (1.3, 2.4), "corner": (1.9, 3.0)}[kind]
    obs = []
    # The wizard prescribes N viewpoints; the user hits each sloppily and
    # retries that slot if the capture is rejected (target not fully in frame,
    # view too grazing, scan plane missing the target).
    for slot in tg.wizard_slots(n_poses, pose_style):
        for _ in range(retries):
            pose = tg.pose_from_slot(slot, rng, dist_range=dist)
            if kind == "plane":
                cam_obs = tg.observe_board_camera(pose, CAM, rng, board=target)
                if cam_obs is None:
                    continue
                P_l = tg.observe_board_lidar(pose, mount.T_cl, spec, rng, board=target)
                if P_l is None or len(P_l) < 3:
                    continue
                obs.append(PlaneObs(cam_obs[0], cam_obs[1], P_l, spec.range_sigma_m))
            elif kind == "wall":
                n_cam, d_cam = tg.observe_wall_camera(pose, rng, target)
                P_l = tg.observe_board_lidar(pose, mount.T_cl, spec, rng, board=target)
                if P_l is None or len(P_l) < 3:
                    continue
                # residual sigma must also carry the camera-plane error, which
                # here dominates: ~d * sin(1.5 deg) at 2 m is ~50 mm.
                sig = float(np.hypot(spec.range_sigma_m,
                                     np.hypot(d_cam * np.radians(target.normal_sigma_deg),
                                              target.offset_sigma_m)))
                obs.append(PlaneObs(n_cam, d_cam, P_l, sig))
            else:
                cam_obs = tg.observe_corner_camera(pose, CAM, rng)
                if cam_obs is None:
                    continue
                P_c, vis = cam_obs
                P_l, sig = tg.observe_corner_lidar(pose, mount.T_cl, spec, rng, vis)
                obs.append(PointObs(P_c, P_l, float(np.hypot(sig, 0.015))))
            break

    return obs if len(obs) >= n_poses else None


def run_session(mount: MountCase, n_poses: int, rng: np.random.Generator,
                method: str = "board_a2", pose_style: str = "diverse",
                range_sigma_m: float | None = None, retries: int = 6):
    """Capture + solve one wizard session. Returns (SolveResult, errors)."""
    obs = build_session(mount, n_poses, rng, method, pose_style, range_sigma_m, retries)
    if obs is None:
        return None
    kind = METHODS[method][0]
    res = solve_extrinsic(obs, cad_nominal(mount.T_cl, rng),
                          kind="point" if kind == "corner" else "plane")
    return res, extrinsic_errors(mount.T_cl, res.T_cl)


def split_half_gate(obs, mount: MountCase, rng: np.random.Generator,
                    kind: str) -> float | None:
    """The wizard's user-facing quality gate.

    Solve the extrinsic twice on two disjoint halves of the captured poses and
    report how far apart the two answers place a point at 3 m, in pixels. This
    is a DIRECT empirical estimate of the calibration's own repeatability: it
    sees the actual noise realisation and the actual pose geometry, which the
    linearised covariance does not (with a fixed prescribed pose set the
    covariance is nearly constant across sessions and cannot rank them).
    """
    if len(obs) < 4:
        return None
    a, b = obs[0::2], obs[1::2]
    k = "point" if kind == "corner" else "plane"
    Ta = solve_extrinsic(a, cad_nominal(mount.T_cl, rng), kind=k).T_cl
    Tb = solve_extrinsic(b, cad_nominal(mount.T_cl, rng), kind=k).T_cl
    return reprojection_error_from_extrinsic(Ta, Tb, 3.0,
                                             rng=np.random.default_rng(12345))


def extrinsic_errors(T_true: SE3, T_est: SE3) -> dict:
    # the error transform expressed in the LIDAR frame, so the budget can
    # re-apply it to any mount: X_est = T_true . E . p_lidar
    E = (T_true.inv() @ T_est).as_vec6()
    e = {"rot_deg": rot_angle_deg(T_true.R, T_est.R),
         "trans_mm": float(np.linalg.norm(T_true.t - T_est.t) * 1e3),
         "err_se3": E}
    for r in (1.0, 3.0, 8.0):
        # fixed rng: the same FOV sample set for every trial, so differences
        # between trials are differences in the extrinsic, not in the sampling
        e[f"reproj_px_{r:g}m"] = reprojection_error_from_extrinsic(
            T_true, T_est, r, rng=np.random.default_rng(12345))
    return e


def trial_batch(mount: MountCase, n_poses: int, method: str, n_trials: int,
                seed: int = 0, pose_style: str = "diverse",
                range_sigma_m: float | None = None):
    """Repeat a wizard session n_trials times; returns a dict of arrays."""
    keys = ("rot_deg", "trans_mm", "reproj_px_1m", "reproj_px_3m", "reproj_px_8m")
    acc = {k: [] for k in keys}
    pool, n_fail = [], 0
    for i in range(n_trials):
        rng = np.random.default_rng((seed * 100003 + i * 7919) % (2 ** 32))
        out = run_session(mount, n_poses, rng, method=method,
                          pose_style=pose_style, range_sigma_m=range_sigma_m)
        if out is None:
            n_fail += 1
            continue
        for k in keys:
            acc[k].append(out[1][k])
        pool.append(out[1]["err_se3"])
    res = {k: np.array(v) for k, v in acc.items()}
    res["pool"] = np.array(pool) if pool else np.zeros((0, 6))
    res["n_fail"] = n_fail
    res["n_ok"] = len(acc["rot_deg"])
    return res
