"""6-DoF camera<-lidar extrinsic solver (prototype).

Two residual families, matching the two candidate wizard targets:

  plane  : r_ij = n_i . (R p_ij + t) - d_i          (one scalar per lidar point)
           n_i, d_i = the target plane as measured by the camera at pose i.
           Observability: a 3D lidar puts 3 independent constraints per pose
           (offset + 2 in-plane slopes); a 2D lidar puts only 2 (offset +
           1 slope), because its returns lie on a line. Hence the 2D sensor
           needs strictly more, and more varied, poses.

  point  : r_ij = R p_ij + t - P_ij                  (three scalars per feature)
           P_ij = camera-side 3D vertex, p_ij = lidar-side 3D vertex.

Minimised with scipy least_squares (Trust Region Reflective) with a soft-L1
loss so a mis-segmented board or a mis-associated corner does not drag the
solution. PRODUCTION NOTE: the shipped solver is C++/Ceres inside
engine/color/ + engine/slam/pushbroom/ (spec tasks A8/A11); this prototype
exists to size the error, fix the residual definition, and set the wizard's
pose count and quality gate.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy.optimize import least_squares

from sim.geom import SE3


@dataclass
class PlaneObs:
    """One wizard pose: camera-measured target plane + lidar returns on it."""
    n_cam: np.ndarray          # (3,) unit normal in camera frame
    d_cam: float               # n . X = d for X on the plane
    pts_lidar: np.ndarray      # (K,3) in lidar frame
    sigma: float = 0.02        # metres, lidar range noise


@dataclass
class PointObs:
    """One wizard pose: matched 3D vertices in camera and lidar frames."""
    pts_cam: np.ndarray        # (K,3)
    pts_lidar: np.ndarray      # (K,3)
    sigma: float = 0.02


@dataclass
class SolveResult:
    T_cl: SE3
    cost: float
    rms_residual_m: float
    n_residuals: int
    n_poses: int
    cond: float                             # condition number of J^T J
    sigma_rot_deg: float                    # 1-sigma from the linearised covariance
    sigma_trans_mm: float
    success: bool = True
    detail: dict = field(default_factory=dict)


# ----------------------------------------------------------------- residuals

def _plane_residuals(v, obs: list[PlaneObs]) -> np.ndarray:
    T = SE3.from_vec6(v)
    out = []
    for o in obs:
        X = o.pts_lidar @ T.R.T + T.t
        out.append((X @ o.n_cam - o.d_cam) / o.sigma)
    return np.concatenate(out)


def _point_residuals(v, obs: list[PointObs]) -> np.ndarray:
    T = SE3.from_vec6(v)
    out = []
    for o in obs:
        out.append(((o.pts_lidar @ T.R.T + T.t) - o.pts_cam).ravel() / o.sigma)
    return np.concatenate(out)


# -------------------------------------------------------------------- solve

def solve_extrinsic(obs, T_init: SE3, kind: str = "plane",
                    loss: str = "soft_l1") -> SolveResult:
    """Estimate camera<-lidar from `obs`.

    `T_init` is the bracket's CAD nominal -- the wizard always has one, since
    the mount is a designed part. It is deliberately several degrees and a few
    centimetres off the truth in the experiments.
    """
    fun = _plane_residuals if kind == "plane" else _point_residuals
    v0 = T_init.as_vec6()
    # Stage 1: plain L2 from the CAD nominal. A robust kernel must NOT be used
    # here -- at a 4 deg / 25 mm start every residual looks like an outlier and
    # soft-L1 stalls the solve. Stage 2: re-solve with the robust kernel from
    # the (now close) L2 solution, so the kernel only rejects real outliers.
    s1 = least_squares(fun, v0, args=(obs,), method="trf",
                       xtol=1e-14, ftol=1e-14, gtol=1e-14, max_nfev=400)
    sol = least_squares(fun, s1.x, args=(obs,), loss=loss, f_scale=2.5,
                        method="trf", xtol=1e-14, ftol=1e-14, gtol=1e-14,
                        max_nfev=400)

    J = sol.jac
    H = J.T @ J
    try:
        cond = float(np.linalg.cond(H))
        m = max(len(sol.fun) - 6, 1)
        s2 = float(2 * sol.cost / m)                 # residuals are already whitened
        C = np.linalg.inv(H) * s2
        sig = np.sqrt(np.clip(np.diag(C), 0, None))
        # residuals are normalised by sigma [m], so C is in (rad^2, m^2)
        sigma_rot = float(np.degrees(np.linalg.norm(sig[:3]) / np.sqrt(3)))
        sigma_tr = float(np.linalg.norm(sig[3:]) / np.sqrt(3) * 1e3)
    except np.linalg.LinAlgError:
        cond, sigma_rot, sigma_tr = np.inf, np.inf, np.inf

    sigmas = np.array([o.sigma for o in obs])
    rms = float(np.sqrt(np.mean(sol.fun ** 2)) * float(np.mean(sigmas)))

    return SolveResult(T_cl=SE3.from_vec6(sol.x), cost=float(sol.cost),
                       rms_residual_m=rms, n_residuals=len(sol.fun),
                       n_poses=len(obs), cond=cond, sigma_rot_deg=sigma_rot,
                       sigma_trans_mm=sigma_tr, success=bool(sol.success))
