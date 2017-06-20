/* clang-format off */
#include "drake/solvers/rotation_constraint.h"
#include "drake/solvers/rotation_constraint_internal.h"
/* clang-format on */

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "drake/common/symbolic_expression.h"
#include "drake/math/cross_product.h"
#include "drake/math/gray_code.h"
#include "drake/solvers/mixed_integer_optimization_util.h"

using std::numeric_limits;
using drake::symbolic::Expression;

namespace drake {
namespace solvers {

MatrixDecisionVariable<3, 3> NewRotationMatrixVars(MathematicalProgram* prog,
                                                   const std::string& name) {
  MatrixDecisionVariable<3, 3> R = prog->NewContinuousVariables<3, 3>(name);

  // Forall i,j, -1 <= R(i,j) <=1.
  prog->AddBoundingBoxConstraint(-1, 1, R);

  // -1 <= trace(R) <= 3.
  // Proof sketch:
  //   orthonormal => |lambda_i|=1.
  //   R is real => eigenvalues either real or appear in complex conj pairs.
  //   Case 1: All real (lambda_i \in {-1,1}).
  //     det(R)=lambda_1*lambda_2*lambda_3=1 => lambda_1=lambda_2, lambda_3=1.
  //   Case 2: Two imaginary, pick conj(lambda_1) = lambda_2.
  //    => lambda_1*lambda_2 = 1.  =>  lambda_3 = 1.
  //    and also => lambda_1 + lambda_2 = 2*Re(lambda_1) \in [-2,2].
  prog->AddLinearConstraint(Eigen::RowVector3d::Ones(), -1, 3, R.diagonal());
  return R;
}

void AddBoundingBoxConstraintsImpliedByRollPitchYawLimits(
    MathematicalProgram* prog,
    const Eigen::Ref<const MatrixDecisionVariable<3, 3>>& R,
    RollPitchYawLimits limits) {
  // Based on the RPY to Rotation Matrix conversion:
  // [ cp*cy, cy*sp*sr - cr*sy, sr*sy + cr*cy*sp]
  // [ cp*sy, cr*cy + sp*sr*sy, cr*sp*sy - cy*sr]
  // [   -sp,            cp*sr,            cp*cr]
  // where cz = cos(z) and sz = sin(z), and using
  //  kRoll_NegPI_2_to_PI_2 = 1 << 1,   // => cos(r)>=0
  //  kRoll_0_to_PI = 1 << 2,           // => sin(r)>=0
  //  kPitch_NegPI_2_to_PI_2 = 1 << 3,  // => cos(p)>=0
  //  kPitch_0_to_PI = 1 << 4,          // => sin(p)>=0
  //  kYaw_NegPI_2_to_PI_2 = 1 << 5,    // => cos(y)>=0
  //  kYaw_0_to_PI = 1 << 6,            // => sin(y)>=0

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kYaw_NegPI_2_to_PI_2))
    prog->AddBoundingBoxConstraint(0, 1, R(0, 0));

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kYaw_0_to_PI))
    prog->AddBoundingBoxConstraint(0, 1, R(1, 0));

  if (limits & kPitch_0_to_PI) prog->AddBoundingBoxConstraint(-1, 0, R(2, 0));

  if ((limits & kRoll_NegPI_2_to_PI_2) && (limits & kYaw_NegPI_2_to_PI_2) &&
      (limits & kPitch_0_to_PI) && (limits & kRoll_0_to_PI) &&
      (limits & kYaw_0_to_PI))
    prog->AddBoundingBoxConstraint(0, 1, R(1, 1));

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kRoll_0_to_PI))
    prog->AddBoundingBoxConstraint(0, 1, R(2, 1));

  if ((limits & kRoll_0_to_PI) && (limits & kYaw_0_to_PI) &&
      (limits & kRoll_NegPI_2_to_PI_2) && (limits & kYaw_NegPI_2_to_PI_2) &&
      (limits & kPitch_0_to_PI))
    prog->AddBoundingBoxConstraint(0, 1, R(0, 2));

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kRoll_NegPI_2_to_PI_2))
    prog->AddBoundingBoxConstraint(0, 1, R(2, 2));
}

void AddBoundingBoxConstraintsImpliedByRollPitchYawLimitsToBinary(
    MathematicalProgram* prog,
    const Eigen::Ref<const MatrixDecisionVariable<3, 3>>& B,
    RollPitchYawLimits limits) {
  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kYaw_NegPI_2_to_PI_2))
    prog->AddBoundingBoxConstraint(1, 1, B(0, 0));

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kYaw_0_to_PI))
    prog->AddBoundingBoxConstraint(1, 1, B(1, 0));

  if (limits & kPitch_0_to_PI) prog->AddBoundingBoxConstraint(0, 0, B(2, 0));

  if ((limits & kRoll_NegPI_2_to_PI_2) && (limits & kYaw_NegPI_2_to_PI_2) &&
      (limits & kPitch_0_to_PI) && (limits & kRoll_0_to_PI) &&
      (limits & kYaw_0_to_PI))
    prog->AddBoundingBoxConstraint(1, 1, B(1, 1));

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kRoll_0_to_PI))
    prog->AddBoundingBoxConstraint(1, 1, B(2, 1));

  if ((limits & kRoll_0_to_PI) && (limits & kYaw_0_to_PI) &&
      (limits & kRoll_NegPI_2_to_PI_2) && (limits & kYaw_NegPI_2_to_PI_2) &&
      (limits & kPitch_0_to_PI))
    prog->AddBoundingBoxConstraint(1, 1, B(0, 2));

  if ((limits & kPitch_NegPI_2_to_PI_2) && (limits & kRoll_NegPI_2_to_PI_2))
    prog->AddBoundingBoxConstraint(1, 1, B(2, 2));
}

void AddRotationMatrixSpectrahedralSdpConstraint(
    MathematicalProgram* prog,
    const Eigen::Ref<const MatrixDecisionVariable<3, 3>>& R) {
  // TODO(russt): Clean this up using symbolic expressions!
  Eigen::Matrix4d F0 = Eigen::Matrix4d::Identity();
  Eigen::Matrix4d F11 = Eigen::Matrix4d::Zero();
  F11(0, 0) = -1;
  F11(1, 1) = 1;
  F11(2, 2) = 1;
  F11(3, 3) = -1;
  Eigen::Matrix4d F21 = Eigen::Matrix4d::Zero();
  F21(0, 2) = -1;
  F21(1, 3) = 1;
  F21(2, 0) = -1;
  F21(3, 1) = 1;
  Eigen::Matrix4d F31 = Eigen::Matrix4d::Zero();
  F31(0, 1) = 1;
  F31(1, 0) = 1;
  F31(2, 3) = 1;
  F31(3, 2) = 1;
  Eigen::Matrix4d F12 = Eigen::Matrix4d::Zero();
  F12(0, 2) = 1;
  F12(1, 3) = 1;
  F12(2, 0) = 1;
  F12(3, 1) = 1;
  Eigen::Matrix4d F22 = Eigen::Matrix4d::Zero();
  F22(0, 0) = -1;
  F22(1, 1) = -1;
  F22(2, 2) = 1;
  F22(3, 3) = 1;
  Eigen::Matrix4d F32 = Eigen::Matrix4d::Zero();
  F32(0, 3) = 1;
  F32(1, 2) = -1;
  F32(2, 1) = -1;
  F32(3, 0) = 1;
  Eigen::Matrix4d F13 = Eigen::Matrix4d::Zero();
  F13(0, 1) = 1;
  F13(1, 0) = 1;
  F13(2, 3) = -1;
  F13(3, 2) = -1;
  Eigen::Matrix4d F23 = Eigen::Matrix4d::Zero();
  F23(0, 3) = 1;
  F23(1, 2) = 1;
  F23(2, 1) = 1;
  F23(3, 0) = 1;
  Eigen::Matrix4d F33 = Eigen::Matrix4d::Zero();
  F33(0, 0) = 1;
  F33(1, 1) = -1;
  F33(2, 2) = 1;
  F33(3, 3) = -1;

  prog->AddLinearMatrixInequalityConstraint(
      {F0, F11, F21, F31, F12, F22, F32, F13, F23, F33},
      {R.col(0), R.col(1), R.col(2)});
}

namespace {

void AddOrthogonalConstraint(
    MathematicalProgram* prog,
    const Eigen::Ref<const VectorDecisionVariable<3>>& v1,
    const Eigen::Ref<const VectorDecisionVariable<3>>& v2) {
  // We do this by introducing
  //   |v1+v2|^2 = v1'v1 + 2v1'v2 + v2'v2 <= 2
  //   |v1-v2|^2 = v1'v1 - 2v1'v2 + v2'v2 <= 2
  // This is tight when v1'v1 = 1 and v2'v2 = 1.

  // TODO(russt): Consider generalizing this to |v1+alpha*v2|^2 <= 1+alpha^2,
  // for any real-valued alpha.  When |R1|<|R2|<=1 or |R2|<|R1|<=1,
  // different alphas represent different constraints.

  Eigen::Matrix<double, 5, 6> A;
  Eigen::Matrix<double, 5, 1> b;

  // |v1+v2|^2 <= 2
  // Implemented as a Lorenz cone using z = [ sqrt(2); v1+v2 ].
  Vector4<symbolic::Expression> z;
  z << std::sqrt(2), v1 + v2;
  prog->AddLorentzConeConstraint(z);

  // |v1-v2|^2 <= 2
  // Implemented as a Lorenz cone using z = [ sqrt(2); v1-v2 ].
  z.tail<3>() = v1 - v2;
  prog->AddLorentzConeConstraint(z);
}

}  // namespace

void AddRotationMatrixOrthonormalSocpConstraint(
    MathematicalProgram* prog,
    const Eigen::Ref<const MatrixDecisionVariable<3, 3>>& R) {
  // All columns should be unit length (but we can only write Ri'Ri<=1),
  // implemented as a rotated Lorenz cone with z = Ax+b = [1;1;R.col(i)].
  Eigen::Matrix<double, 5, 3> A = Eigen::Matrix<double, 5, 3>::Zero();
  A.bottomRows<3>() = Eigen::Matrix3d::Identity();
  Eigen::Matrix<double, 5, 1> b;
  b << 1, 1, 0, 0, 0;
  for (int i = 0; i < 3; i++) {
    prog->AddRotatedLorentzConeConstraint(A, b, R.col(i));
    prog->AddRotatedLorentzConeConstraint(A, b, R.row(i).transpose());
  }

  AddOrthogonalConstraint(prog, R.col(0), R.col(1));  // R0'*R1 = 0.
  AddOrthogonalConstraint(prog, R.col(1), R.col(2));  // R1'*R2 = 0.
  AddOrthogonalConstraint(prog, R.col(0), R.col(2));  // R0'*R2 = 0.

  // Same for the rows
  AddOrthogonalConstraint(prog, R.row(0).transpose(), R.row(1).transpose());
  AddOrthogonalConstraint(prog, R.row(1).transpose(), R.row(2).transpose());
  AddOrthogonalConstraint(prog, R.row(0).transpose(), R.row(2).transpose());
}

namespace {

// Decodes the discretization of the axes.
// For compactness, this method is referred to as phi(i) in the documentation
// below.  The implementation must give a valid number even for i<0 and
// i>num_binary_variables_per_half_axis.
double EnvelopeMinValue(int i, int num_binary_variables_per_half_axis) {
  return static_cast<double>(i) / num_binary_variables_per_half_axis;
}

/**
 * Given an orthant index, return the vector indicating whether each axis has
 * positive or negative value.
 * @param orthant The index of the orthant
 * @return mask If mask(i) = 1, then the i'th axis has positive value in the
 * given orthant. If mask(i) = -1, then the i'th axis has negative value in the
 * given orthant.
 */
Eigen::Vector3i OrthantToAxisMask(int orthant) {
  return internal::FlipVector(Eigen::Vector3i::Ones(), orthant);
}

// The half axis has interval (0, Φ(1), ..., Φ(N-1), 1). The full axis has
// interval (-1, -Φ(N-1), ..., -Φ(1), 0, Φ(1), ..., Φ(N-1), 1).
Eigen::Vector3i PositiveAxisIntervalIndexToFullAxisIntervalIndex(const Eigen::Ref<const Eigen::Vector3i>& interval_idx, int orthant, int num_intervals_per_half_axis) {
  const Eigen::Vector3i mask = OrthantToAxisMask(orthant);
  Eigen::Vector3i ret;
  for (int i = 0; i < 3; ++i) {
    ret(i) = mask(i) > 0 ? (interval_idx(i) + num_intervals_per_half_axis) : (num_intervals_per_half_axis - 1 - interval_idx(i));
  }
  return ret;
}

// Given the index of the active interval along each axis, an expression of
// binary variables, such that this expression is 0 if the binary variable b
// assignment equals to `interval_idx` using Gray code, otherwise the expression
// takes strictly positive value.
template<typename Scalar, typename Derived>
Scalar PickBinaryExpressionGivenInterval(int interval_idx, const Eigen::Ref<const Eigen::MatrixXi>& gray_codes, const Derived& b) {
  DRAKE_ASSERT(interval_idx >= 0 && interval_idx <= gray_codes.rows());
  DRAKE_ASSERT(b.rows() == gray_codes.cols());
  Scalar ret{0};
  for (int i = 0; i < gray_codes.cols(); ++i) {
    ret += gray_codes(interval_idx, i) ? 1 - b(i) : b(i);
  }
  return ret;
}

// Given (an integer enumeration of) the orthant, return a vector c with
// c(i) = a(i) if element i is positive in the indicated orthant, otherwise
// c(i) = b(i).
template <typename Derived>
Eigen::Matrix<Derived, 3, 1> PickPermutation(
    const Eigen::Matrix<Derived, 3, 1>& a,
    const Eigen::Matrix<Derived, 3, 1>& b, int orthant) {
  DRAKE_DEMAND(orthant >= 0 && orthant <= 7);
  Eigen::Vector3i mask = internal::FlipVector(Eigen::Vector3i::Ones(), orthant);
  Eigen::Matrix<Derived, 3, 1> c = a;
  for (int i = 0; i < 3; ++i) {
    if (mask(i) < 0) {
      c(i) = b(i);
    }
  }
  return c;
}

// Given two coordinates, find the (positive) third coordinate on that
// intersects with the unit circle.
double Intercept(double x, double y) {
  DRAKE_ASSERT(x * x + y * y <= 1);
  return std::sqrt(1 - x * x - y * y);
}

}  // namespace

namespace internal {
// Return a vector `c` of positive-integer-valued expressions, such that
// c(0) = c(1) = c(2) = 0 if the box is active, otherwise ∃i, c(i) >= 1
// xi, yi, zi are the indices of the intervals in the positive orthant.
// B_vec[i] are the binary variables along the i'th axis. These binary variables
// indicate which interval is active along each axis.
template<typename Scalar1, typename Scalar2>
Vector3<Scalar1> CalcBoxBinaryExpressionInOrthant(
    int xi, int yi, int zi, int orthant,
    const Eigen::Ref<const Eigen::MatrixXi>& gray_codes,
    const std::array<VectorX<Scalar2>, 3>& B_vec,
    int num_intervals_per_half_axis) {
  Vector3<Scalar1> orthant_c;
  const Eigen::Vector3i orthant_box_interval_idx =
      PositiveAxisIntervalIndexToFullAxisIntervalIndex(
          Eigen::Vector3i(xi, yi, zi), orthant, num_intervals_per_half_axis);
  for (int axis = 0; axis < 3; ++axis) {
    orthant_c(axis) = PickBinaryExpressionGivenInterval<Scalar1, VectorX<Scalar2>>(
        orthant_box_interval_idx(axis), gray_codes, B_vec[axis]);
  }
  return orthant_c;
}

// Given an axis-aligned box in the first orthant, computes and returns all the
// intersecting points between the edges of the box and the unit sphere.
// @param bmin  The vertex of the box closest to the origin.
// @param bmax  The vertex of the box farthest from the origin.
std::vector<Eigen::Vector3d> ComputeBoxEdgesAndSphereIntersection(
    const Eigen::Vector3d& bmin, const Eigen::Vector3d& bmax) {
  // Assumes the positive orthant (and bmax > bmin).
  DRAKE_ASSERT(bmin(0) >= 0 && bmin(1) >= 0 && bmin(2) >= 0);
  DRAKE_ASSERT(bmax(0) > bmin(0) && bmax(1) > bmin(1) && bmax(2) > bmin(2));

  // Assumes the unit circle intersects the box.
  DRAKE_ASSERT(bmin.lpNorm<2>() <= 1);
  DRAKE_ASSERT(bmax.lpNorm<2>() >= 1);

  std::vector<Eigen::Vector3d> intersections;

  if (bmin.lpNorm<2>() == 1) {
    // Then only the min corner intersects.
    intersections.push_back(bmin);
    return intersections;
  }

  if (bmax.lpNorm<2>() == 1) {
    // Then only the max corner intersects.
    intersections.push_back(bmax);
    return intersections;
  }

  // The box has at most 12 edges, each edge can intersect with the unit sphere
  // at most once, since the box is in the first orthant.
  intersections.reserve(12);

  // 1. Loop through each vertex of the box, add it to intersections if
  // the vertex is on the sphere.
  for (int i = 0; i < 8; ++i) {
    Eigen::Vector3d vertex{};
    for (int axis = 0; axis < 3; ++axis) {
      vertex(axis) = i & (1 << axis) ? bmin(axis) : bmax(axis);
    }
    if (vertex.norm() == 1) {
      intersections.push_back(vertex);
    }
  }

  // 2. Loop through each edge, find the intersection between each edge and the
  // unit sphere, if one exists.
  for (int axis = 0; axis < 3; ++axis) {
    // axis = 0 means edges along x axis;
    // axis = 1 means edges along y axis;
    // axis = 2 means edges along z axis;
    int fixed_axis1 = (axis + 1) % 3;
    int fixed_axis2 = (axis + 2) % 3;
    // 4 edges along each axis;

    // First finds the two end points on the edge.
    Eigen::Vector3d pt_closer, pt_farther;
    pt_closer(axis) = bmin(axis);
    pt_farther(axis) = bmax(axis);
    std::array<double, 2> fixed_axis1_val = {
        {bmin(fixed_axis1), bmax(fixed_axis1)}};
    std::array<double, 2> fixed_axis2_val = {
        {bmin(fixed_axis2), bmax(fixed_axis2)}};
    for (double val1 : fixed_axis1_val) {
      pt_closer(fixed_axis1) = val1;
      pt_farther(fixed_axis1) = pt_closer(fixed_axis1);
      for (double val2 : fixed_axis2_val) {
        pt_closer(fixed_axis2) = val2;
        pt_farther(fixed_axis2) = pt_closer(fixed_axis2);

        // Determines if there is an intersecting point between the edge and the
        // sphere. If the intersecting point is not the vertex of the box, then
        // push this intersecting point to intersections directly.
        if (pt_closer.norm() < 1 && pt_farther.norm() > 1) {
          Eigen::Vector3d pt_intersect{};
          pt_intersect(fixed_axis1) = pt_closer(fixed_axis1);
          pt_intersect(fixed_axis2) = pt_closer(fixed_axis2);
          pt_intersect(axis) =
              Intercept(pt_intersect(fixed_axis1), pt_intersect(fixed_axis2));
          intersections.push_back(pt_intersect);
        }
      }
    }
  }
  return intersections;
}

/**
 * Compute the outward unit length normal of the triangle, with the three
 * vertices being `pt0`, `pt1` and `pt2`.
 * @param pt0 A vertex of the triangle, in the first orthant (+++).
 * @param pt1 A vertex of the triangle, in the first orthant (+++).
 * @param pt2 A vertex of the triangle, in the first orthant (+++).
 * @param n The unit length normal vector of the triangle, pointing outward from
 * the origin.
 * @param d The intersecpt of the plane. Namely nᵀ * x = d for any point x on
 * the triangle.
 */
void ComputeTriangleOutwardNormal(const Eigen::Vector3d& pt0,
                                  const Eigen::Vector3d& pt1,
                                  const Eigen::Vector3d& pt2,
                                  Eigen::Vector3d* n, double* d) {
  DRAKE_DEMAND((pt0.array() >= 0).all());
  DRAKE_DEMAND((pt1.array() >= 0).all());
  DRAKE_DEMAND((pt2.array() >= 0).all());
  *n = (pt2 - pt0).cross(pt1 - pt0);
  // If the three points are almost colinear, then throw an error.
  double n_norm = n->norm();
  if (n_norm < 1E-3) {
    throw std::runtime_error("The points are almost colinear.");
  }
  *n = (*n) / n_norm;
  if (n->sum() < 0) {
    (*n) *= -1;
  }
  *d = pt0.dot(*n);
  DRAKE_DEMAND((n->array() >= 0).all());
}

/**
 * For the vertices in `pts`, determine if these vertices are co-planar. If they
 * are, then compute that plane nᵀ * x = d.
 * @param pts The vertices to be checked.
 * @param n The unit length normal vector of the plane, points outward from the
 * origin. If the vertices are not co-planar, leave `n` to 0.
 * @param d The intersecpt of the plane. If the vertices are not co-planar, set
 * this to 0.
 * @return If the vertices are co-planar, set this to true. Otherwise set to
 * false.
 */
bool AreAllVerticesCoPlanar(const std::vector<Eigen::Vector3d>& pts,
                            Eigen::Vector3d* n, double* d) {
  DRAKE_DEMAND(pts.size() >= 3);
  ComputeTriangleOutwardNormal(pts[0], pts[1], pts[2], n, d);
  // Determine if the other vertices are on the plane nᵀ * x = d.
  bool pts_on_plane = true;
  for (int i = 3; i < static_cast<int>(pts.size()); ++i) {
    if (std::abs(n->dot(pts[i]) - *d) > 1E-10) {
      pts_on_plane = false;
      n->setZero();
      *d = 0;
      break;
    }
  }
  return pts_on_plane;
}

/*
 * For the intersection region between the surface of the unit sphere, and the
 * interior of a box aligned with the axes, use a half space relaxation for
 * the intersection region as nᵀ * v >= d
 * @param[in] pts. The vertices containing the intersecting points between edges
 * of the box and the surface of the unit sphere.
 * @param[out] n. The unit length normal vector of the halfspace, pointing
 * outward.
 * @param[out] d. The intercept of the halfspace.
 */
void ComputeHalfSpaceRelaxationForBoxSphereIntersection(
    const std::vector<Eigen::Vector3d>& pts, Eigen::Vector3d* n, double* d) {
  DRAKE_DEMAND(pts.size() >= 3);
  // We first prove that for a given normal vector n, and ANY unit
  // length vector v within the intersection region between the
  // surface of the unit sphere and the interior of the axis-aligned
  // box, the minimal of nᵀ * v, always occurs at one of the vertex of
  // the intersection region, if the box and the vector n are in the
  // same orthant. Namely min nᵀ * v = min(nᵀ * pts.col(i))
  // To see this, for any vector v in the intersection region, suppose
  // it is on an arc, aligned with one axis. Without loss of
  // generality we assume the aligned axis is x axis, namely
  // v(0) = t, box_min(0) <= t <= box_max(0)
  // and v(1)² + v(2)² = 1 - t², with the bounds
  // box_min(1) <= v(1) <= box_max(1)
  // box_min(2) <= v(2) <= box_max(2)
  // And the inner product nᵀ * v =
  // n(0) * t + s * (n(1) * cos(α) + n(2) * sin(α))
  // where we define s = sqrt(1 - t²)
  // Using the property of trigonometric function, we know that
  // the minimal of (n(1) * cos(α) + n(2) * sin(α)) is obtained at
  // the boundary of α. Thus we know that the minimal of nᵀ * v is
  // always obtained at one of the vertex pts.col(i).

  // To find the tightest bound d satisfying nᵀ * v >= d for all
  // vector v in the intersection region, we use the fact that for
  // a given normal vector n, the minimal of nᵀ * v is always obtained
  // at one of the vertices pts.col(i), and formulate the following
  // SOCP to find the normal vector n
  // max d
  // s.t d <= nᵀ * pts.col(i)
  //     nᵀ * n <= 1

  // First compute a plane coinciding with a triangle, formed by 3 vertices
  // in the intersection region. If all the vertices are on that plane, then the
  // normal of the plane is n, and we do not need to run the optimization.
  // If there are only 3 vertices in the intersection region, then the normal
  // vector n is the normal of the triangle, formed by these three vertices.

  bool pts_on_plane = AreAllVerticesCoPlanar(pts, n, d);
  if (pts_on_plane) {
    return;
  }

  // If there are more than 3 vertices in the intersection region, and these
  // vertices are not co-planar, then we find the normal vector `n` through an
  // optimization, whose formulation is mentioned above.
  MathematicalProgram prog_normal;
  auto n_var = prog_normal.NewContinuousVariables<3>();
  auto d_var = prog_normal.NewContinuousVariables<1>();
  prog_normal.AddLinearCost(-d_var(0));
  for (const auto& pt : pts) {
    prog_normal.AddLinearConstraint(n_var.dot(pt) >= d_var(0));
  }

  // TODO(hongkai.dai): This optimization is expensive, especially if we have
  // multiple rotation matrices, all relaxed with the same number of binary
  // variables per half axis, the result `n` and `d` are the same. Should
  // consider hard-coding the result, to avoid repeated computation.

  Vector4<symbolic::Expression> lorentz_cone_vars;
  lorentz_cone_vars << 1, n_var;
  prog_normal.AddLorentzConeConstraint(lorentz_cone_vars);
  prog_normal.Solve();
  *n = prog_normal.GetSolution(n_var);
  *d = prog_normal.GetSolution(d_var(0));

  DRAKE_DEMAND((*n)(0) > 0 && (*n)(1) > 0 && (*n)(2) > 0);
  DRAKE_DEMAND(*d > 0 && *d < 1);
}

/**
 * For the intersection region between the surface of the unit sphere, and the
 * interior of a box aligned with the axes, relax this nonconvex intersection
 * region to its convex hull. This convex hull has some planar facets (formed
 * by the triangles connecting the vertices of the intersection region). This
 * function computes these planar facets. It is guaranteed that any point x on
 * the intersection region, satisfies A * x <= b.
 * @param[in] pts The vertices of the intersection region. Same as the `pts` in
 * ComputeHalfSpaceRelaxationForBoxSphereIntersection()
 * @param[out] A The rows of A are the normal vector of facets. Each row of A is
 * a unit length vector.
 * @param b b(i) is the interscept of the i'th facet.
 * @pre pts[i] are all in the first orthant, namely (pts[i].array() >=0).all()
 * should be true.
 */
void ComputeInnerFacetsForBoxSphereIntersection(
    const std::vector<Eigen::Vector3d>& pts,
    Eigen::Matrix<double, Eigen::Dynamic, 3>* A, Eigen::VectorXd* b) {
  for (const auto& pt : pts) {
    DRAKE_DEMAND((pt.array() >= 0).all());
  }
  A->resize(0, 3);
  b->resize(0);
  // Loop through each triangle, formed by connecting the vertices of the
  // intersection region. We write the plane coinciding with the triangle as
  // cᵀ * x >= d. If all the vertices of the intersection region satisfies
  // cᵀ * pts[i] >= d, then we know the intersection region satisfies
  // cᵀ * x >= d for all x being a point in the intersection region. Here we
  // use the proof in the ComputeHalfSpaceRelaxationForBoxSphereIntersection(),
  // that the minimal value of cᵀ * x over all x inside the intersection
  // region, occurs at one of the vertex of the intersection region.
  for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
    for (int j = i + 1; j < static_cast<int>(pts.size()); ++j) {
      for (int k = j + 1; k < static_cast<int>(pts.size()); ++k) {
        // First compute the triangle formed by vertices pts[i], pts[j] and
        // pts[k].
        Eigen::Vector3d c;
        double d;
        ComputeTriangleOutwardNormal(pts[i], pts[j], pts[k], &c, &d);
        // A halfspace cᵀ * x >= d is valid, if all vertices pts[l] satisfy
        // cᵀ * pts[l] >= d.
        bool is_valid_halfspace = true;
        // Now check if the other vertices pts[l] satisfies cᵀ * pts[l] >= d.
        for (int l = 0; l < static_cast<int>(pts.size()); ++l) {
          if ((l != i) && (l != j) && (l != k)) {
            if (c.dot(pts[l]) < d - 1E-10) {
              is_valid_halfspace = false;
              break;
            }
          }
        }
        // If all vertices pts[l] satisfy cᵀ * pts[l] >= d, then add this
        // constraint to A * x <= b
        if (is_valid_halfspace) {
          A->conservativeResize(A->rows() + 1, Eigen::NoChange);
          b->conservativeResize(b->rows() + 1, Eigen::NoChange);
          A->row(A->rows() - 1) = -c.transpose();
          (*b)(b->rows() - 1) = -d;
        }
      }
    }
  }
}
}  // namespace internal

namespace {

void AddMcCormickVectorConstraints(
    MathematicalProgram* prog, const VectorDecisionVariable<3>& v,
    const std::array<VectorXDecisionVariable, 3>& B_i,
    const VectorDecisionVariable<3>& v1, const VectorDecisionVariable<3>& v2,
    int num_intervals_per_half_axis, const Eigen::Ref<const Eigen::MatrixXi>& gray_codes) {
  const int& N{num_intervals_per_half_axis};

  // Iterate through regions.
  Eigen::Vector3d box_min, box_max;
  for (int xi = 0; xi < N; xi++) {
    box_min(0) = EnvelopeMinValue(xi, N);
    box_max(0) = EnvelopeMinValue(xi + 1, N);
    for (int yi = 0; yi < N; yi++) {
      box_min(1) = EnvelopeMinValue(yi, N);
      box_max(1) = EnvelopeMinValue(yi + 1, N);
      for (int zi = 0; zi < N; zi++) {
        box_min(2) = EnvelopeMinValue(zi, N);
        box_max(2) = EnvelopeMinValue(zi + 1, N);

        const double box_min_norm = box_min.lpNorm<2>();
        const double box_max_norm = box_max.lpNorm<2>();
        if (box_min_norm <= 1.0 + 2 * numeric_limits<double>::epsilon() &&
            box_max_norm >= 1.0 - 2 * numeric_limits<double>::epsilon()) {
          // The box intersects with the surface of the unit sphere.
          // Two possible cases
          // 1. If the box bmin <= x <= bmax intersects with the surface of the
          // unit sphere at a unique point (either bmin or bmax),
          // 2. Otherwise, there is a region of intersection.

          // We choose the error as 2 * eps here. The reason is that
          // if x.norm() == 1, then another vector y which is different from
          // x by eps (y(i) = x(i) + eps), the norm of y is at most 1 + 2 * eps.
          if (std::abs(box_min_norm - 1.0) <
                  2 * numeric_limits<double>::epsilon() ||
              std::abs(box_max_norm - 1.0) <
                  2 * numeric_limits<double>::epsilon()) {
            // If box_min or box_max is on the sphere, then denote the point on
            // the sphere as u. We have the following constraint when the box is
            // active
            //     v = u
            //     vᵀ * v1 = 0
            //     vᵀ * v2 = 0
            //     v.cross(v1) = v2
            // We introduce an integer valued vector c ∈ N³, such that
            // c(0) = c(1) = c(2) = 0 indicates that the box is active,
            // ∃ i, s.t c(i) >= 1 indicates that the box is inactive. We have
            // the following constraints
            // -2 * (c(0) + c(1) + c(2)) <= v - u <= 2 * (c(0) + c(1) + c(2))
            // -(c(0) + c(1) + c(2)) <= vᵀ * v1 <= (c(0) + c(1) + c(2))
            // -(c(0) + c(1) + c(2)) <= vᵀ * v2 <= (c(0) + c(1) + c(2))
            // -2 * (c(0) + c(1) + c(2)) <= v.cross(v1) - v2 <= 2 * (c(0) + c(1) + c(2))
            Eigen::Vector3d unique_intersection;  // `u` in the documentation
                                                  // above
            if (std::abs(box_min_norm - 1.0) <
                2 * numeric_limits<double>::epsilon()) {
              unique_intersection = box_min / box_min_norm;
            } else {
              unique_intersection = box_max / box_max_norm;
            }
            Eigen::Vector3d orthant_u;
            for (int o = 0; o < 8; o++) {  // iterate over orthants
              orthant_u = internal::FlipVector(unique_intersection, o);
              Vector3<symbolic::Expression> orthant_c = internal::CalcBoxBinaryExpressionInOrthant<symbolic::Expression, symbolic::Variable>(xi, yi, zi, o, gray_codes, B_i, num_intervals_per_half_axis);

              // TODO(hongkai.dai): remove this for loop when we can handle
              // Eigen::Array of symbolic formulae.
              const symbolic::Expression orthant_c_sum = orthant_c.sum();
              for (int i = 0; i < 3; ++i) {
                prog->AddLinearConstraint(v(i) - orthant_u(i) <= 2 * orthant_c_sum);
                prog->AddLinearConstraint(v(i) - orthant_u(i) >= -2 * orthant_c_sum);
              }
              const Expression v_dot_v1 = orthant_u.dot(v1);
              const Expression v_dot_v2 = orthant_u.dot(v2);
              prog->AddLinearConstraint(v_dot_v1 <= orthant_c_sum);
              prog->AddLinearConstraint(-orthant_c_sum <= v_dot_v1);
              prog->AddLinearConstraint(v_dot_v2 <= orthant_c_sum);
              prog->AddLinearConstraint(-orthant_c_sum <= v_dot_v2);
              const Vector3<Expression> v_cross_v1 = orthant_u.cross(v1);
              for (int i = 0; i < 3; ++i) {
                prog->AddLinearConstraint(v_cross_v1(i) - v2(i) <= 2 * orthant_c_sum);
                prog->AddLinearConstraint(v_cross_v1(i) - v2(i) >= -2 * orthant_c_sum);
              }
            }
          } else {
            // Find the intercepts of the unit sphere with the box, then find
            // the tightest linear constraint of the form:
            //    d <= n'*v
            // that puts v inside (but as close as possible to) the unit circle.
            auto pts = internal::ComputeBoxEdgesAndSphereIntersection(box_min,
                                                                      box_max);
            DRAKE_DEMAND(pts.size() >= 3);

            double d(0);
            Eigen::Vector3d normal{};
            internal::ComputeHalfSpaceRelaxationForBoxSphereIntersection(
                pts, &normal, &d);

            Eigen::VectorXd b(0);
            Eigen::Matrix<double, Eigen::Dynamic, 3> A(0, 3);

            internal::ComputeInnerFacetsForBoxSphereIntersection(pts, &A, &b);

            // theta is the maximal angle between v and normal, where v is an
            // intersecting point between the box and the sphere.
            double cos_theta = d;
            const double theta = std::acos(cos_theta);

            Eigen::Matrix<double, 1, 6> a;
            Eigen::Matrix<double, 3, 9> A_cross;

            Eigen::RowVector3d orthant_normal;
            for (int o = 0; o < 8; o++) {  // iterate over orthants
              orthant_normal = internal::FlipVector(normal, o).transpose();
              Vector3<symbolic::Expression> orthant_c = internal::CalcBoxBinaryExpressionInOrthant<symbolic::Expression, symbolic::Variable>(xi, yi, zi, o, gray_codes, B_i, num_intervals_per_half_axis);
              const symbolic::Expression orthant_c_sum = orthant_c.sum();
              for (int i = 0; i < A.rows(); ++i) {
                // Add the constraint that A * v <= b, representing the inner
                // facets f the convex hull, obtained from the vertices of the
                // intersection region.
                // This constraint is only active if the box is active.
                // We impose the constraint
                // A.row(i) * v - b(i) <= (1 - b) * (c(0) + c(1) + c(2))
                // Or in words
                // If c(0) = 0 and c(1) = 0 and c(2) = 0
                //   A.row(i) * v <= b(i)
                // Otherwise
                //   A.row(i) * v -b(i) is not constrained
                Eigen::Vector3d orthant_a =
                    -internal::FlipVector(-A.row(i).transpose(), o);
                prog->AddLinearConstraint(
                    orthant_a.dot(v) - b(i) <= (1 - b(i)) * orthant_c_sum);
              }

              // Max vector norm constraint: -1 <= normal'*x <= 1.
              if (o % 2 == 0)
                prog->AddLinearConstraint(orthant_normal, -1, 1, v);

              // Dot-product constraint: ideally v.dot(v1) = v.dot(v2) = 0.
              // The cone of (unit) vectors within theta of the normal vector
              // defines a band of admissible vectors v1 and v2 which are
              // orthogonal to v.  They must satisfy the constraint:
              //    -sin(theta) <= normal.dot(vi) <= sin(theta)
              // Proof sketch:
              //   v is within theta of normal.
              //   => vi must be within theta of a vector orthogonal to
              //      the normal.
              //   => vi must be pi/2 +/- theta from the normal.
              //   => |normal||vi| cos(pi/2 + theta) <= normal.dot(vi) <=
              //                |normal||vi| cos(pi/2 - theta).
              // Since normal and vi are both unit length,
              //     -sin(theta) <= normal.dot(vi) <= sin(theta).
              // To activate this only when this box is active, we use
              //   -sin(theta) - (c(0) + c(1) + c(2)) <=
              //     normal.dot(vi) <=
              //     sin(theta) + (c(0) + c(1) + c(2))
              // Note: (An alternative tighter, but SOCP constraint)
              //   v, v1, v2 forms an orthornormal basis. So n'*v is the
              //   projection of n in the v direction, same for n'*v1, n'*v2.
              //   Thus
              //     (n'*v)^2 + (n'*v1)^2 + (n'*v2)^2 = n'*n
              //   which translates to "The norm of a vector is equal to the
              //   sum of squares of the vector projected onto each axes of an
              //   orthornormal basis".
              //   This equation is the same as
              //     (n'*v1)^2 + (n'*v2)^2 <= sin(theta)^2
              //   So actually instead of imposing
              //     -sin(theta)<=n'*vi <=sin(theta),
              //   we can impose a tighter Lorentz cone constraint
              //     [|sin(theta)|, n'*v1, n'*v2] is in the Lorentz cone.
              const double sin_theta = sin(theta);
              prog->AddLinearConstraint(orthant_normal.dot(v1) <= sin_theta + orthant_c_sum);
              prog->AddLinearConstraint(orthant_normal.dot(v1) >= -sin_theta - orthant_c_sum);
              prog->AddLinearConstraint(orthant_normal.dot(v2) <= sin_theta + orthant_c_sum);
              prog->AddLinearConstraint(orthant_normal.dot(v2) >= -sin_theta - orthant_c_sum);

              // Cross-product constraint: ideally v2 = v.cross(v1).
              // Since v is within theta of normal, we will prove that
              // |v2 - normal.cross(v1)| <= 2 * sin(theta / 2)
              // Notice that (v2 - normal.cross(v1))ᵀ * (v2 - normal.cross(v1))
              // = v2ᵀ * v2 + (normal.cross(v1))ᵀ * (normal.cross(v1)) -
              //      2 * v2ᵀ * (normal.cross(v1))
              // <= 1 + 1 - 2 * cos(theta)
              // = (2 * sin(theta / 2))²
              // Thus we get |v2 - normal.cross(v1)| <= 2 * sin(theta / 2)
              // Here we consider to use an elementwise linear constraint
              // -2*sin(theta / 2) <=  v2 - normal.cross(v1) <= 2*sin(theta / 2)
              // Since 0<=theta<=pi/2, this should be enough to rule out the
              // det(R)=-1 case (the shortest projection of a line across the
              // circle onto a single axis has length 2sqrt(3)/3 > 1.15), and
              // can be significantly tighter.

              // To activate this only when the box is active, the complete
              // constraints are
              //  -2*sin(theta/2)-2 * (c(0) + c(1) + c(2)) <= v2-normal.cross(v1)
              //     <= 2*sin(theta/2) + 2 * (c(0) + c(1) * c(2))
              // Note: Again this constraint could be tighter as a Lorenz cone
              // constraint of the form:
              //   |v2 - normal.cross(v1)| <= 2*sin(theta/2).
              Vector3<symbolic::Expression> v2_minus_normal_cross_v1 = v2.cast<symbolic::Expression>();
              v2_minus_normal_cross_v1 -= orthant_normal.cross(v1);
              prog->AddLinearConstraint(v2_minus_normal_cross_v1 <= 2 * Eigen::Vector3d::Constant(sin(theta / 2)) + Vector3<symbolic::Expression>::Constant(2 * orthant_c_sum));
              prog->AddLinearConstraint(v2_minus_normal_cross_v1 >= -2 * Eigen::Vector3d::Constant(sin(theta / 2)) - Vector3<symbolic::Expression>::Constant(2 * orthant_c_sum));
            }
          }
        } else {
          // This box does not intersect with the surface of the sphere.
          for (int o = 0; o < 8; ++o) {  // iterate over orthants
            Vector3<symbolic::Expression> orthant_c = internal::CalcBoxBinaryExpressionInOrthant<symbolic::Expression, symbolic::Variable>(xi, yi, zi, o, gray_codes, B_i, num_intervals_per_half_axis);
            prog->AddLinearConstraint(orthant_c.sum() >= 1);
          }
        }
      }
    }
  }
}

/**
 * Add the constraint that vector R.col(i) and R.col(j) are not in the
 * same or opposite orthants. This constraint should be satisfied since
 * R.col(i) should be perpendicular to R.col(j). For example, if both
 * R.col(i) and R.col(j) are in the first orthant (+++), their inner product
 * has to be non-negative. If the inner product of two first orthant vectors
 * is exactly zero, then both vectors has to be on the boundaries of the first
 * orthant. But we can then assign the vector to a different orthant. The same
 * proof applies to the opposite orthant case.
 * To impose the constraint that R.col(0) and R.col(1) are not both in same
 * orthant, we use the fact that if `num_intervals_per_half_axis` is a power of
 * 2, and R(i, 0) and R(i, 1) have the same sign, then
 * B(i, 0) and B(i, 1) has to have the same value, namely
 * abs(B(i, 0) + B(i, 1) - 1) = 1, otherwise
 * abs(B(i, 0) + B(i, 1) - 1) = 0 if R(i, 0) and R(i, 1) have different
 * signs. We then consider the following constraint
 * sum_{i = 0, 1, 2} t(i) <= 2
 * t(i) >= B(i, 0) + B(i, 1) - 1 >= -t(i)
 * where t(i) is the auxiliary variable as an upper bound of
 * abs(B(i, 0) + B(i, 1))
 * To impose the constraint that R.col(0) and R.col(1) are not in the opposite
 * orthant, we use the fact that if R(i, 0) and R(i, 1) have the opposite sign,
 * then B(i, 0) and B(i, 1) have different values, namely
 * abs(B(i, 0) - B(i, 1)) = 1
 * We then consider the following constraint
 * sum_{i = 0, 1, 2} s(i) <= 2
 * s(i) >= B(i, 0) - B(i, 1) >= s(i)
 * where s(i) is the auxiliary variable as an upper bound of
 * abs(B(i, 0) - B(i, 1))
 * @param prog Add the constraint to this mathematical program.
 * @param B. The binary variables. B(i, j) = 0
 * means R(i, j) <= 0, and B(i, j) = 1 means R(i, j) >= 0
 * @num_intervals_per_half_axis Number of intervals per half axis.
 */
void AddNotInSameOrOppositeOrthantConstraint(
    MathematicalProgram* prog,
    const Eigen::Ref<const MatrixDecisionVariable<3, 3>>& B,
    int num_intervals_per_half_axis) {
  if (num_intervals_per_half_axis == (1 << CeilLog2(num_intervals_per_half_axis))) {
    const std::array<std::pair<int, int>, 3> column_idx = {
        {{0, 1}, {0, 2}, {1, 2}}};
    for (const auto &column_pair : column_idx) {
      const int col_idx0 = column_pair.first;
      const int col_idx1 = column_pair.second;
      auto t = prog->NewContinuousVariables<3>("t");
      auto s = prog->NewContinuousVariables<3>("s");
      prog->AddLinearConstraint(t.cast<symbolic::Expression>().sum() <= 2);
      prog->AddLinearConstraint(s.cast<symbolic::Expression>().sum() <= 2);
      for (int i = 0; i < 3; ++i) {
        prog->AddLinearConstraint(t(i) >= B(i, col_idx0) + B(i, col_idx1) - 1);
        prog->AddLinearConstraint(B(i, col_idx0) + B(i, col_idx1) - 1 >= -t(i));
        prog->AddLinearConstraint(s(i) >= B(i, col_idx0) - B(i, col_idx1));
        prog->AddLinearConstraint(B(i, col_idx0) - B(i, col_idx1) >= -s(i));
      }
    }
  }
}

/**
 * Add a constraint that x(0)² + x(1)² + x(2)² = 1. For this non-convex
 * constraint, we impose convex constraints as a relaxation. To do so, we cut
 * the range of x(i) into intervals at (φ(0), φ(1)), (φ(1), φ(2)), ..., (φ(N-1),
 * φ(N)). With sos2 constraint λ[i](k) >= 0, sum_k λ[i](k) = 1, at most two
 * entries in φ can be strictly positive, and these two entries are adjacent, we
 * can constrain that x(i) lies within one of the range. Moreover, we know that
 * x(i)² <= λ[i](j)*φ²(j) + λ[i](j+1)*φ²(j+1) if x(i) ∈ [φ(j), φ(j+1)]. We can
 * then impose the constraint that sum_{i = 0, 1, 2}sum_k λ[k](i) φ²(k) >= 1
 */
void AddUnitLengthConstraintWithLogarithmicSOS2(
    MathematicalProgram* prog,
    const Eigen::Ref<const Eigen::VectorXd>& phi_vec,
    const Eigen::Ref<const VectorXDecisionVariable>& lambda0,
const Eigen::Ref<const VectorXDecisionVariable>& lambda1,
const Eigen::Ref<const VectorXDecisionVariable>& lambda2) {
  symbolic::Expression x_sum_of_squares_ub{0};
  DRAKE_ASSERT(phi_vec.rows() == lambda0.rows());
  DRAKE_ASSERT(phi_vec.rows() == lambda1.rows());
  DRAKE_ASSERT(phi_vec.rows() == lambda2.rows());
  for (int i = 0; i < phi_vec.rows(); ++i) {
    double phi_square = phi_vec(i) * phi_vec(i);
    x_sum_of_squares_ub += (lambda0(i) + lambda1(i) + lambda2(i)) * phi_square;
  }
  prog->AddLinearConstraint(x_sum_of_squares_ub >= 1);
}

/**
 * Returns a variable `w` to approximate the bilinear product x * y. We know
 * that x is in one of the intervals [φx(i), φx(i+1)], [φy(j), φy(j+1)]. The
 * variable `w` is constrained to be in the convex hull of x * y for x in
 * [φx(i), φx(i+1)], y in [φy(j), φy(j+1)]
 * @param x
 * @param y
 * @param phi_x
 * @param phi_y
 * @param Bx
 * @param By
 * @return
 */
symbolic::Variable AddBilinearProductMcCormickEnvelopeSOS2(
    const symbolic::Variable x,
    const symbolic::Variable y,
    const Eigen::Ref<const Eigen::VectorXd>& phi_x,
    const Eigen::Ref<const Eigen::VectorXd>& phi_y,
    const Eigen::Ref<const VectorXDecisionVariable>& Bx,
    const Eigen::Ref<const VectorXDecisionVariable>& By) {
  const int num_phi_x = phi_x.rows();
  const int num_phi_y = phi_y.rows();
  auto lambda
}
}  // namespace

std::vector<MatrixDecisionVariable<3, 3>>
AddRotationMatrixMcCormickEnvelopeMilpConstraints(
    MathematicalProgram* prog,
    const Eigen::Ref<const MatrixDecisionVariable<3, 3>>& R,
    int num_intervals_per_half_axis, RollPitchYawLimits limits) {
  DRAKE_DEMAND(num_intervals_per_half_axis >= 1);

  // Use a simple lambda to make the constraints more readable below.
  // Note that
  //  forall k>=0, 0<=phi(k), and
  //  forall k<=num_intervals_per_half_axis, phi(k)<=1.
  auto phi = [&](int k) -> double {
    return EnvelopeMinValue(k, num_intervals_per_half_axis) - 1;
  };
  // We add auxiliary variables λ, such that λ[i][j](k),
  // k = 0, ..., 2 * num_intervals_per_half_axis satisfy the special ordered set
  // 2 constraint. Namely sum_k λ[i][j](k) = 1, λ[i][j](k) >= 0, and at most
  // two λ[i][j](k) can be strictly positive among all k, and these two entries
  // have to be adjacent, namely they should be λ[i][j](m) and λ[i][j](m+1).
  int num_lambda = 2 * num_intervals_per_half_axis + 1;
  Eigen::VectorXd phi_vec(num_lambda);
  for (int k = 0; k < num_lambda; ++k) {
    phi_vec(k) = phi(k);
  }
  int num_digits = CeilLog2(num_lambda - 1);
  const auto gray_codes = math::CalculateReflectedGrayCodes(num_digits);
  std::vector<MatrixDecisionVariable<3, 3>> B(num_digits);
  std::array<std::array<VectorXDecisionVariable, 3>, 3> lambda;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      lambda[i][j] = prog->NewContinuousVariables(num_lambda, "lambda[" + std::to_string(i) +"][" + std::to_string(j) + "]");
      auto B_ij = AddLogarithmicSOS2Constraint(prog, lambda[i][j].cast<symbolic::Expression>());
      for (int k = 0; k < num_digits; ++k) {
        B[k](i, j) = B_ij(k);
      }
      // R(i, j) = sum_k phi_vec(k) * lambda[k](i, j)
      prog->AddLinearConstraint(R(i, j) - phi_vec.dot(lambda[i][j].cast<symbolic::Expression>()) == 0);
    }
  }

  // Add constraint that no two rows (or two columns) can lie in the same
  // orthant (or opposite orthant).
  // Due to the property of Gray code, B[0](i, j) = 0 means R(i, j) <= 0,
  // B[0](i, j) = 1 means R(i, j) >= 0
  AddNotInSameOrOppositeOrthantConstraint(prog, B[0], num_intervals_per_half_axis);
  AddNotInSameOrOppositeOrthantConstraint(prog, B[0].transpose(), num_intervals_per_half_axis);

  // Add angle limit constraints.
  // Bounding box will turn on/off an orthant.  It's sufficient to add the
  // constraints only to the positive orthant.
  AddBoundingBoxConstraintsImpliedByRollPitchYawLimitsToBinary(prog, B[0],
                                                               limits);

  for (int i = 0; i < 3; ++i) {
    AddUnitLengthConstraintWithLogarithmicSOS2(prog, phi_vec, lambda[0][i], lambda[1][i], lambda[2][i]);
    AddUnitLengthConstraintWithLogarithmicSOS2(prog, phi_vec, lambda[i][0], lambda[i][1], lambda[i][2]);
  }

  // Add constraints to the column and row vectors.
  for (int i = 0; i < 3; i++) {
    std::array<VectorXDecisionVariable, 3> B_vec;
    for (int j = 0; j < 3; ++j) {
      B_vec[j].resize(num_digits);
    }
    // Make lists of the decision variables in terms of column vectors and row
    // vectors to facilitate the calls below.
    for (int k = 0; k < num_digits; k++) {
      for (int j = 0; j < 3; ++j) {
        B_vec[j](k) = B[k](j, i);
      }
    }
    AddMcCormickVectorConstraints(prog, R.col(i), B_vec,
                                  R.col((i + 1) % 3), R.col((i + 2) % 3), num_intervals_per_half_axis, gray_codes);

    for (int k = 0; k < num_digits; k++) {
      for (int j = 0; j < 3; ++j) {
        B_vec[j](k) = B[k](i, j);
      }
    }
    AddMcCormickVectorConstraints(prog, R.row(i).transpose(), B_vec,
                                  R.row((i + 1) % 3).transpose(),
                                  R.row((i + 2) % 3).transpose(), num_intervals_per_half_axis, gray_codes);
  }
  return B;
}

// Explicit instantiation
template Vector3<symbolic::Expression> internal::CalcBoxBinaryExpressionInOrthant<symbolic::Expression, symbolic::Variable>(
    int xi, int yi, int zi, int orthant,
    const Eigen::Ref<const Eigen::MatrixXi>& gray_codes,
    const std::array<VectorXDecisionVariable , 3>& B_vec,
    int num_intervals_per_half_axis);

template Eigen::Vector3d internal::CalcBoxBinaryExpressionInOrthant<double, double>(
    int xi, int yi, int zi, int orthant,
    const Eigen::Ref<const Eigen::MatrixXi>& gray_codes,
    const std::array<Eigen::VectorXd , 3>& B_vec,
    int num_intervals_per_half_axis);
}  // namespace solvers
}  // namespace drake
