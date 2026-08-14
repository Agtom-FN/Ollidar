"""Camera model, sensor models and the two ground-truth mount cases.

All figures traceable to the LidarScan tech spec (docs/LidarScan Tech Spec.md):
  * COIN-D6   -- 2D, 360 deg, 4,000 pts/s, 10 Hz, 0.9 deg resolution, 0.05-12 m,
                 mounted VERTICALLY (pushbroom).              [spec 2.1 / 3.3]
  * Mid-360   -- 3D, ~200,000 pts/s, 360 x 59 deg FOV, tilted mount. [spec 2.2]
  * Camera    -- phone main camera, 2-5 fps JPEG keyframes with ARCore
                 poses + intrinsics.                          [spec 3.5]
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .geom import SE3, rotvec


# --------------------------------------------------------------- camera

@dataclass(frozen=True)
class Camera:
    """Pinhole model for a typical phone main camera (26 mm equivalent)."""
    width: int = 4032
    height: int = 3024
    focal_equiv_mm: float = 26.0     # 35 mm-equivalent focal length
    sensor_width_mm: float = 36.0    # full-frame reference width

    @property
    def fx(self) -> float:
        # hfov = 2*atan(sensor_w / (2*f_equiv));  fx = (W/2) / tan(hfov/2)
        return (self.width / 2.0) / (self.sensor_width_mm / (2.0 * self.focal_equiv_mm))

    @property
    def fy(self) -> float:
        return self.fx

    @property
    def cx(self) -> float:
        return self.width / 2.0

    @property
    def cy(self) -> float:
        return self.height / 2.0

    @property
    def hfov_deg(self) -> float:
        return float(np.degrees(2 * np.arctan(self.sensor_width_mm / (2 * self.focal_equiv_mm))))

    @property
    def K(self) -> np.ndarray:
        return np.array([[self.fx, 0, self.cx], [0, self.fy, self.cy], [0, 0, 1.0]])

    def project(self, X_cam: np.ndarray) -> np.ndarray:
        """Project (N,3) camera-frame points to (N,2) pixels. No distortion:
        the spike studies extrinsic/time-sync error, and phone intrinsics are
        already well calibrated per-device by ARCore."""
        X = np.atleast_2d(np.asarray(X_cam, float))
        z = np.clip(X[:, 2], 1e-6, None)
        return np.column_stack([self.fx * X[:, 0] / z + self.cx,
                                self.fy * X[:, 1] / z + self.cy])

    def in_view(self, X_cam: np.ndarray, margin: float = 0.0) -> np.ndarray:
        X = np.atleast_2d(np.asarray(X_cam, float))
        uv = self.project(X)
        return ((X[:, 2] > 0.1)
                & (uv[:, 0] > margin) & (uv[:, 0] < self.width - margin)
                & (uv[:, 1] > margin) & (uv[:, 1] < self.height - margin))

    def sample_fov_directions(self, n: int, rng: np.random.Generator,
                              fill: float = 0.85) -> np.ndarray:
        """Unit directions in the camera frame spread over the image area."""
        u = rng.uniform(0.5 - fill / 2, 0.5 + fill / 2, n) * self.width
        v = rng.uniform(0.5 - fill / 2, 0.5 + fill / 2, n) * self.height
        d = np.column_stack([(u - self.cx) / self.fx, (v - self.cy) / self.fy, np.ones(n)])
        return d / np.linalg.norm(d, axis=1, keepdims=True)


# --------------------------------------------------------------- lidars

@dataclass(frozen=True)
class LidarSpec:
    name: str
    kind: str                 # "2d" or "3d"
    range_sigma_m: float      # 1-sigma range noise
    ang_res_deg: float        # angular sample spacing
    pts_per_s: float
    dwell_s: float            # per wizard pose, how long the user holds still
    max_target_pts: int       # points landing on the calibration target per pose


D6 = LidarSpec(name="COIN-D6", kind="2d", range_sigma_m=0.030, ang_res_deg=0.9,
               pts_per_s=4_000, dwell_s=1.5, max_target_pts=250)

MID360 = LidarSpec(name="Livox Mid-360", kind="3d", range_sigma_m=0.020, ang_res_deg=0.4,
                   pts_per_s=200_000, dwell_s=1.0, max_target_pts=600)


# --------------------------------------------------------- ground-truth mounts

def _mount_d6() -> SE3:
    """Case (a): D6 mounted VERTICALLY on the bracket, 15 cm from the camera.

    D6 body frame: it spins about its own +z and scans its x-y plane.
    'Vertical' mount => the scan plane is the camera's y-z plane (vertical,
    containing the forward axis), so walking sweeps floor -> wall -> ceiling.
      lidar +x -> camera +z (forward)
      lidar +y -> camera -y (up)
      lidar +z -> camera +x (spin axis points sideways)
    A small bracket misalignment is added: no bracket is perfect, and the whole
    point of the wizard is to recover it.
    """
    R_nominal = np.column_stack([[0, 0, 1.0], [0, -1.0, 0], [1.0, 0, 0]])
    R = R_nominal @ rotvec([1.5, -2.0, 0.8])
    t = np.array([0.015, 0.150, -0.030])          # +y is DOWN => 15 cm below camera
    return SE3(R, t)


def _mount_mid360() -> SE3:
    """Case (b): Mid-360 tilted 15 deg below the camera axis.

    Livox body frame: +x forward, +y left, +z up.
      lidar +x -> camera +z, lidar +y -> camera -x, lidar +z -> camera -y
    then pitched 15 deg down (rotation about camera +x).
    """
    R_nominal = np.column_stack([[0, 0, 1.0], [-1.0, 0, 0], [0, -1.0, 0]])
    R = rotvec([15.0, 0, 0]) @ R_nominal @ rotvec([0.9, 1.3, -0.6])
    t = np.array([0.0, 0.100, -0.045])
    return SE3(R, t)


@dataclass(frozen=True)
class MountCase:
    key: str
    label: str
    lidar: LidarSpec
    T_cl: SE3                 # camera <- lidar  (the thing we must estimate)

    @property
    def baseline_m(self) -> float:
        return float(np.linalg.norm(self.T_cl.t))


MOUNTS = {
    "d6": MountCase("d6", "COIN-D6 (2D, vertical, 15 cm)", D6, _mount_d6()),
    "mid360": MountCase("mid360", "Livox Mid-360 (3D, tilted 15 deg)", MID360, _mount_mid360()),
}


CAM = Camera()


# --------------------------------------------------------- error metric bridge

def reprojection_error_from_extrinsic(T_true: SE3, T_est: SE3, range_m: float,
                                      cam: Camera = CAM, n: int = 512,
                                      rng: np.random.Generator | None = None) -> float:
    """RMS pixel disagreement when colorizing a point at `range_m` using
    `T_est` instead of `T_true`. This is the number the product actually cares
    about; rotation/translation errors are only a means to it."""
    rng = rng or np.random.default_rng(0)
    d = cam.sample_fov_directions(n, rng)
    X_true = d * range_m
    p_l = T_true.inv().apply(X_true)              # what the lidar really measured
    X_est = T_est.apply(p_l)
    uv_t, uv_e = cam.project(X_true), cam.project(X_est)
    return float(np.sqrt(np.mean(np.sum((uv_e - uv_t) ** 2, axis=1))))
