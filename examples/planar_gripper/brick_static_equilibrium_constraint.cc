#include "drake/examples/planar_gripper/brick_static_equilibrium_constraint.h"

#include "drake/math/autodiff.h"
#include "drake/math/autodiff_gradient.h"
#include "drake/multibody/inverse_kinematics/kinematic_constraint_utilities.h"

namespace drake {
namespace examples {

BrickStaticEquilibriumNonlinearConstraint::
    BrickStaticEquilibriumNonlinearConstraint(
        const GripperBrickSystem<double>& gripper_brick_system,
        std::vector<std::pair<Finger, BrickFace>> finger_face_contacts,
        systems::Context<double>* plant_mutable_context)
    : solvers::Constraint(3,
                          gripper_brick_system.plant().num_positions() +
                              finger_face_contacts.size() * 2,
                          Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero()),
      gripper_brick_system_{gripper_brick_system},
      finger_face_contacts_(std::move(finger_face_contacts)),
      plant_mutable_context_(plant_mutable_context) {
  brick_mass_ = gripper_brick_system_.plant()
                    .GetBodyByName("brick_link")
                    .get_default_mass();
}

Eigen::Vector3d
BrickStaticEquilibriumNonlinearConstraint::ComputeFingerTipInBrickFrame(
    const multibody::MultibodyPlant<double>& plant, const Finger finger,
    const systems::Context<double>& plant_context,
    const Eigen::Ref<const Eigen::VectorXd>&) const {
  Eigen::Vector3d p_BTip;
  plant.CalcPointsPositions(plant_context,
                            gripper_brick_system_.finger_link2_frame(finger),
                            gripper_brick_system_.p_F2Tip(),
                            gripper_brick_system_.brick_frame(), &p_BTip);
  return p_BTip;
}

Vector3<AutoDiffXd>
BrickStaticEquilibriumNonlinearConstraint::ComputeFingerTipInBrickFrame(
    const multibody::MultibodyPlant<double>& plant, const Finger finger,
    const systems::Context<double>& plant_context,
    const Eigen::Ref<const AutoDiffVecXd>& q) const {
  Eigen::Vector3d p_BTip;
  plant.CalcPointsPositions(plant_context,
                            gripper_brick_system_.finger_link2_frame(finger),
                            gripper_brick_system_.p_F2Tip(),
                            gripper_brick_system_.brick_frame(), &p_BTip);
  Eigen::Matrix3Xd Js_v_BF2_B(3, plant.num_positions());
  plant.CalcJacobianTranslationalVelocity(
      plant_context, multibody::JacobianWrtVariable::kQDot,
      gripper_brick_system_.finger_link2_frame(finger),
      gripper_brick_system_.p_F2Tip(), gripper_brick_system_.brick_frame(),
      gripper_brick_system_.brick_frame(), &Js_v_BF2_B);
  return math::initializeAutoDiffGivenGradientMatrix(
      p_BTip, Js_v_BF2_B * math::autoDiffToGradientMatrix(q));
}

template <typename T>
void BrickStaticEquilibriumNonlinearConstraint::DoEvalGeneric(
    const Eigen::Ref<const VectorX<T>>& x, VectorX<T>* y) const {
  y->resize(3);
  const auto& plant = gripper_brick_system_.plant();
  multibody::internal::UpdateContextConfiguration(
      plant_mutable_context_, plant, x.head(plant.num_positions()));
  const T theta = x(gripper_brick_system_.brick_revolute_x_position_index());
  using std::cos;
  using std::sin;
  const T sin_theta = sin(theta);
  const T cos_theta = cos(theta);
  Matrix2<T> R_WB;
  R_WB << cos_theta, -sin_theta, sin_theta, cos_theta;
  const Vector2<T> f_B =
      R_WB.transpose() * Eigen::Vector2d(0, -brick_mass_ * 9.81);
  y->template head<2>() = f_B.template tail<2>();
  (*y)(2) = T(0);
  for (int i = 0; i < static_cast<int>(finger_face_contacts_.size()); ++i) {
    y->template head<2>() +=
        x.template segment<2>(plant.num_positions() + i * 2);
    const Vector3<T> p_BTip = ComputeFingerTipInBrickFrame(
        plant, finger_face_contacts_[i].first, *plant_mutable_context_,
        x.head(plant.num_positions()));
    // C is the point of contact between the finger and the brick.
    Vector2<T> p_BC = p_BTip.template tail<2>();
    switch (finger_face_contacts_[i].second) {
      case BrickFace::kPosY: {
        p_BC(0) -= T(gripper_brick_system_.finger_tip_radius());
        break;
      }
      case BrickFace::kNegY: {
        p_BC(0) += T(gripper_brick_system_.finger_tip_radius());
        break;
      }
      case BrickFace::kPosZ: {
        p_BC(1) -= T(gripper_brick_system_.finger_tip_radius());
        break;
      }
      case BrickFace::kNegZ: {
        p_BC(1) += T(gripper_brick_system_.finger_tip_radius());
        break;
      }
    }
    // Now compute the torque about the COM
    (*y)(2) += p_BC(0) * x(plant.num_positions() + 2 * i + 1) -
               p_BC(1) * x(plant.num_positions() + 2 * i);
  }
}  // namespace examples

void BrickStaticEquilibriumNonlinearConstraint::DoEval(
    const Eigen::Ref<const Eigen::VectorXd>& x, Eigen::VectorXd* y) const {
  DoEvalGeneric<double>(x, y);
}

void BrickStaticEquilibriumNonlinearConstraint::DoEval(
    const Eigen::Ref<const AutoDiffVecXd>& x, AutoDiffVecXd* y) const {
  DoEvalGeneric<AutoDiffXd>(x, y);
}

void BrickStaticEquilibriumNonlinearConstraint::DoEval(
    const Eigen::Ref<const VectorX<symbolic::Variable>>&,
    VectorX<symbolic::Expression>*) const {
  throw std::runtime_error(
      "BrickStaticEquilibriumNonlinearConstraint::DoEval does not support "
      "symbolic computation.");
}

Eigen::Matrix<symbolic::Variable, 2, Eigen::Dynamic>
AddBrickStaticEquilibriumConstraint(
    const GripperBrickSystem<double>& gripper_brick_system,
    const std::vector<std::pair<Finger, BrickFace>>& finger_face_contacts,
    const Eigen::Ref<const VectorX<symbolic::Variable>>& q_vars,
    systems::Context<double>* plant_mutable_context,
    solvers::MathematicalProgram* prog) {
  const int num_contacts = static_cast<int>(finger_face_contacts.size());
  const auto f_Cb_B =
      prog->NewContinuousVariables<2, Eigen::Dynamic>(2, num_contacts);
  const auto& plant = gripper_brick_system.plant();
  // Now add the nonlinear constraint that the total wrench is 0.
  VectorX<symbolic::Variable> nonlinear_constraint_bound_vars(
      plant.num_positions() + 2 * num_contacts);
  nonlinear_constraint_bound_vars.head(plant.num_positions()) = q_vars;
  for (int i = 0; i < num_contacts; ++i) {
    nonlinear_constraint_bound_vars.segment<2>(plant.num_positions() + 2 * i) =
        f_Cb_B.col(i);
  }
  prog->AddConstraint(
      std::make_shared<BrickStaticEquilibriumNonlinearConstraint>(
          gripper_brick_system, finger_face_contacts, plant_mutable_context),
      nonlinear_constraint_bound_vars);

  // Add the linear constraint that the contact force is within the friction
  // cone.
  const multibody::CoulombFriction<double>& brick_friction =
      plant.default_coulomb_friction(plant.GetCollisionGeometriesForBody(
          gripper_brick_system.brick_frame().body())[0]);
  for (int i = 0; i < num_contacts; ++i) {
    const multibody::CoulombFriction<double>& finger_tip_friction =
        plant.default_coulomb_friction(plant.GetCollisionGeometriesForBody(
            gripper_brick_system
                .finger_link2_frame(finger_face_contacts[i].first)
                .body())[0]);
    const multibody::CoulombFriction<double> combined_friction =
        multibody::CalcContactFrictionFromSurfaceProperties(
            brick_friction, finger_tip_friction);
    const double mu = combined_friction.static_friction();
    switch (finger_face_contacts[i].second) {
      case BrickFace::kNegY: {
        prog->AddLinearConstraint(f_Cb_B(0, i) >= 0);
        prog->AddLinearConstraint(f_Cb_B(1, i) <= mu * f_Cb_B(0, i));
        prog->AddLinearConstraint(f_Cb_B(1, i) >= -mu * f_Cb_B(0, i));
        break;
      }
      case BrickFace::kNegZ: {
        prog->AddLinearConstraint(f_Cb_B(1, i) >= 0);
        prog->AddLinearConstraint(f_Cb_B(0, i) <= mu * f_Cb_B(1, i));
        prog->AddLinearConstraint(f_Cb_B(0, i) >= -mu * f_Cb_B(1, i));
        break;
      }
      case BrickFace::kPosY: {
        prog->AddLinearConstraint(f_Cb_B(0, i) <= 0);
        prog->AddLinearConstraint(f_Cb_B(1, i) <= -mu * f_Cb_B(0, i));
        prog->AddLinearConstraint(f_Cb_B(1, i) >= mu * f_Cb_B(0, i));
        break;
      }
      case BrickFace::kPosZ: {
        prog->AddLinearConstraint(f_Cb_B(1, i) <= 0);
        prog->AddLinearConstraint(f_Cb_B(0, i) <= -mu * f_Cb_B(1, i));
        prog->AddLinearConstraint(f_Cb_B(0, i) >= mu * f_Cb_B(1, i));
        break;
      }
    }
  }

  return f_Cb_B;
}
}  // namespace examples
}  // namespace drake
