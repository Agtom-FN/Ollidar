#include "scanengine/eigen_demo.hpp"

#include <Eigen/Dense>

namespace scanengine {

double eigen_smoke_test() {
    // A stand-in for the kind of small dense solve poses/ and merge/ do
    // constantly (e.g. rigid-transform least squares).
    Eigen::Matrix3d A;
    A << 4, 1, 2,
         1, 3, 0,
         2, 0, 5;
    const Eigen::Vector3d b(1, 2, 3);
    const Eigen::Vector3d x = A.ldlt().solve(b);
    return (A * x - b).norm();
}

}  // namespace scanengine
