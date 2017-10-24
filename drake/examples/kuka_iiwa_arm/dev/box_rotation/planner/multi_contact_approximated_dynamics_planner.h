#pragma once

#include "drake/solvers/mathematical_program.h"

namespace drake {
namespace examples {
namespace kuka_iiwa_arm {
namespace box_rotation {

class ContactFacet {
 public:
  ContactFacet(const Eigen::Ref<const Eigen::Matrix3Xd>& vertices,
               const Eigen::Ref<const Eigen::Matrix3Xd>& friction_cone_edges);

  /**
   * @retval wrenches wrenches is a vector of size NumVertices. wrenches[i] is
   * a 6 x NumFrictionConeEdges() matrix, containing the wrenches (force/torque)
   * generated by an edge of a friction cone at one vertex.
   */
  std::vector<Eigen::Matrix<double, 6, Eigen::Dynamic>> CalcWrenchConeEdges()
      const;

  int NumVertices() const { return vertices_.cols(); }

  int NumFrictionConeEdges() const { return friction_cone_edges_.cols(); }

  const Eigen::Matrix3Xd& vertices() const { return vertices_; }

  const Eigen::Matrix3Xd& friction_cone_edges() const {
    return friction_cone_edges_;
  }

 private:
  Eigen::Matrix3Xd vertices_;
  Eigen::Matrix3Xd friction_cone_edges_;
};

class MultiContactApproximatedDynamicsPlanner
    : public solvers::MathematicalProgram {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(MultiContactApproximatedDynamicsPlanner)

  MultiContactApproximatedDynamicsPlanner(
      double mass, const Eigen::Ref<const Eigen::Matrix3d>& inertia,
      const std::vector<ContactFacet>& contact_facets, int nT,
      int num_arm_patches);

 private:
  /**
   * Add the constraint on the linear dynamics
   * m * com_accel = R_WB * force + m * gravity;
   * Notice that there is the bilinear product between R_WB and the contact
   * force. We will need to relax this bilinear product to convex constraints.
   */
  void AddLinearDynamicConstraint();

  double m_;             // mass of the box.
  Eigen::Matrix3d I_B_;  // Inertia of the box, in the body frame.
  Eigen::Vector3d gravity_;
  std::vector<ContactFacet>
      contact_facets_;    // All the contact facets on the box.
  int nT_;                // Number of time samples.
  int num_arms_patches_;  // Number of total contact patches on all arms.
  // com_pos_ is a 3 x nT_ matrix.
  solvers::MatrixDecisionVariable<3, Eigen::Dynamic> com_pos_;
  // com_vel_ is a 3 x nT_ matrix. The velocity of the CoM in the world frame.
  solvers::MatrixDecisionVariable<3, Eigen::Dynamic> com_vel_;
  // com_accel_ is a 3 x nT_ matrix. The acceleration of the CoM in the world
  // frame.
  solvers::MatrixDecisionVariable<3, Eigen::Dynamic> com_accel_;
  // R_WB_ is vector of size nT_, R_WB_[i] is a 3 x 3 matrix, the orientation
  // of body frame B measured and expressed in the world frame W.
  std::vector<solvers::MatrixDecisionVariable<3, 3>> R_WB_;
  // omega_BpB_ is a 3 x nT_ matrix. omega_BpB_.col(i) is the angular velocity
  // of the body frame B, measured and expressed in a frame Bp, that is fixed in
  // the world frame, and instantaneously coincides with the body frame B.
  solvers::MatrixDecisionVariable<3, Eigen::Dynamic> omega_BpB_;
  // omega_dot_BpB_ is a 3 x nT_ matrix. omega_dot_BpB_.col(i) is the angular
  // acceleration of the body frame B, measured and expressed in the frame Bp.
  solvers::MatrixDecisionVariable<3, Eigen::Dynamic> omega_dot_BpB_;
  // B_active_facet_ is a num_facets x nT_ matrix containing binary variables.
  // B_active_facet_(i, j) = 1 if the i'th facet is active at time j, 0
  // otherwise.
  solvers::MatrixXDecisionVariable B_active_facet_;
  // contact_wrench_weight_ has size num_facets. contact_wrench_weight[i] is a
  // matrix with contact_facets_[i].NumVertices() *
  // contact_facets_[i].NumFrictionConeEdges() rows,
  // and nT_ columns.
  std::vector<solvers::MatrixXDecisionVariable> contact_wrench_weight_;
  // total_contact_wrench_ is a 6 x nT_ matrix. The first three rows for contact
  // force, the bottom three rows for contact torque.
  solvers::MatrixDecisionVariable<6, Eigen::Dynamic> total_contact_wrench_;
};
}  // namespace box_rotation
}  // namespace kuka_iiwa_arm
}  // namespace examples
}  // namespace drake