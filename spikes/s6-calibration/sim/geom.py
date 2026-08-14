"""SE(3) helpers for the S6 calibration spike.

Conventions
-----------
Camera frame (OpenCV):  x right, y down, z forward.
A rigid transform is stored as (R, t) with  X_a = R @ X_b + t  meaning
"R,t maps frame-b coordinates into frame-a coordinates" (written T_ab).
"""

from __future__ import annotations

import numpy as np
from scipy.spatial.transform import Rotation


# ---------------------------------------------------------------- rotations

def rotvec(r_xyz_deg) -> np.ndarray:
    """Rotation matrix from an XYZ-extrinsic Euler triple given in degrees."""
    return Rotation.from_euler("xyz", np.asarray(r_xyz_deg, float), degrees=True).as_matrix()


def exp_so3(w: np.ndarray) -> np.ndarray:
    """Rotation matrix from an axis-angle vector (radians)."""
    return Rotation.from_rotvec(np.asarray(w, float)).as_matrix()


def log_so3(R: np.ndarray) -> np.ndarray:
    return Rotation.from_matrix(R).as_rotvec()


def rot_angle_deg(R_a: np.ndarray, R_b: np.ndarray) -> float:
    """Geodesic angle between two rotations, in degrees."""
    return float(np.degrees(np.linalg.norm(log_so3(R_a.T @ R_b))))


def random_small_rotation(sigma_deg: float, rng: np.random.Generator) -> np.ndarray:
    """Rotation with per-axis Gaussian angle error of `sigma_deg`."""
    w = rng.normal(0.0, np.radians(sigma_deg), 3)
    return exp_so3(w)


def random_unit(rng: np.random.Generator, n: int = 1) -> np.ndarray:
    v = rng.normal(size=(n, 3))
    return v / np.linalg.norm(v, axis=1, keepdims=True)


# ---------------------------------------------------------------- transforms

class SE3:
    __slots__ = ("R", "t")

    def __init__(self, R: np.ndarray, t: np.ndarray):
        self.R = np.asarray(R, float).reshape(3, 3)
        self.t = np.asarray(t, float).reshape(3)

    @staticmethod
    def identity() -> "SE3":
        return SE3(np.eye(3), np.zeros(3))

    def __matmul__(self, other: "SE3") -> "SE3":
        return SE3(self.R @ other.R, self.R @ other.t + self.t)

    def inv(self) -> "SE3":
        Rt = self.R.T
        return SE3(Rt, -Rt @ self.t)

    def apply(self, P: np.ndarray) -> np.ndarray:
        """Transform an (N,3) or (3,) array of points."""
        P = np.atleast_2d(np.asarray(P, float))
        return P @ self.R.T + self.t

    def as_vec6(self) -> np.ndarray:
        return np.concatenate([log_so3(self.R), self.t])

    @staticmethod
    def from_vec6(v: np.ndarray) -> "SE3":
        v = np.asarray(v, float)
        return SE3(exp_so3(v[:3]), v[3:])

    def __repr__(self) -> str:
        rpy = Rotation.from_matrix(self.R).as_euler("xyz", degrees=True)
        return f"SE3(rpy_deg={np.round(rpy, 3)}, t_mm={np.round(self.t * 1e3, 1)})"


def look_at(eye: np.ndarray, target: np.ndarray, roll_deg: float = 0.0,
            world_up: np.ndarray = np.array([0.0, 0.0, 1.0])) -> SE3:
    """World<-camera transform for a camera at `eye` looking at `target`.

    Camera z is the viewing direction, y points 'down' in the image.
    """
    eye = np.asarray(eye, float)
    z = np.asarray(target, float) - eye
    z /= np.linalg.norm(z)
    up = np.asarray(world_up, float)
    if abs(np.dot(up, z)) > 0.98:                 # degenerate: pick another up
        up = np.array([0.0, 1.0, 0.0])
    x = np.cross(z, -up)                          # image-x right, image-y down
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    R = np.column_stack([x, y, z])
    R = R @ exp_so3(np.array([0.0, 0.0, np.radians(roll_deg)]))
    return SE3(R, eye)
