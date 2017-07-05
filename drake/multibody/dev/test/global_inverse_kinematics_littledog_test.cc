#include "drake/multibody/global_inverse_kinematics.h"

#include <memory>

#include <gtest/gtest.h>

#include "drake/common/drake_path.h"
#include "drake/common/eigen_matrix_compare.h"
#include "drake/multibody/parsers/urdf_parser.h"
#include "drake/multibody/rigid_body_tree_construction.h"
#include "drake/multibody/rigid_body_plant/create_load_robot_message.h"
#include "drake/multibody/rigid_body_plant/viewer_draw_translator.h"
#include "drake/solvers/gurobi_solver.h"
#include "drake/lcm/drake_lcm.h"
#include "drake/multibody/shapes/geometry.h"
#include "drake/multibody/joints/fixed_joint.h"
#include "drake/systems/framework/basic_vector.h"
#include "drake/lcmtypes/drake/lcmt_viewer_load_robot.hpp"

using Eigen::Vector3d;
using Eigen::Isometry3d;
using drake::solvers::SolutionResult;
using DrakeShapes::Sphere;

namespace drake {
namespace multibody {
namespace {

std::unique_ptr<RigidBodyTreed> ConstructLittleDog() {
  std::unique_ptr<RigidBodyTree<double>> rigid_body_tree =
      std::make_unique<RigidBodyTree<double>>();
  const std::string model_path = drake::GetDrakePath() +
      "/examples/LittleDog/LittleDog.urdf";

  parsers::urdf::AddModelInstanceFromUrdfFile(
      model_path,
      drake::multibody::joints::kQuaternion,
      nullptr,
      rigid_body_tree.get());

  //AddFlatTerrainToWorld(rigid_body_tree.get());

  return rigid_body_tree;
}

void VisualizePosture(const RigidBodyTreed& tree, const Eigen::Ref<const Eigen::VectorXd>& q) {
  lcm::DrakeLcm lcm;
  Eigen::VectorXd x(tree.get_num_positions() + tree.get_num_velocities());
  x.head(q.size()) = q;
  systems::BasicVector<double> x_draw(x);
  std::vector<uint8_t> message_bytes;

  lcmt_viewer_load_robot load_msg = multibody::CreateLoadRobotMessage<double>(tree);
  const int length = load_msg.getEncodedSize();
  message_bytes.resize(length);
  load_msg.encode(message_bytes.data(), 0, length);
  lcm.Publish("DRAKE_VIEWER_LOAD_ROBOT", message_bytes.data(),
              message_bytes.size());

  systems::ViewerDrawTranslator posture_drawer(tree);
  posture_drawer.Serialize(0, x_draw, &message_bytes);
  lcm.Publish("DRAKE_VIEWER_DRAW", message_bytes.data(),
              message_bytes.size());
}

void AddPointToBody(RigidBodyTreed* tree, int link_idx, const Eigen::Vector3d& pt, const std::string& name) {
  auto body = std::make_unique<RigidBody<double>>();
  body->set_name(name);
  const Sphere shape(0.003);
  const Eigen::Vector4d material(0.9, 0.0, 0.7, 1.0);
  const DrakeShapes::VisualElement visual_element(shape, Eigen::Isometry3d::Identity(), material);
  body->AddVisualElement(visual_element);

  Eigen::Isometry3d transform;
  transform.linear().setIdentity();
  transform.translation() = pt;
  auto joint = std::make_unique<FixedJoint>(name + "_joint", transform);
  body->add_joint(tree->get_mutable_body(link_idx), std::move(joint));
  tree->bodies.push_back(std::move(body));
}

void AddBoxToBody(RigidBodyTreed* tree, int link_idx, const Eigen::Isometry3d& X_box_to_parent, const Eigen::Vector3d& box_size, const std::string& name, const Eigen::RowVector3d& color = Eigen::RowVector3d(0.4, 0.2, 0.6)) {
  auto body = std::make_unique<RigidBody<double>>();
  body->set_name(name);
  const DrakeShapes::Box box(box_size);
  const Eigen::Vector4d material(color(0), color(1), color(2), 1.0);
  const DrakeShapes::VisualElement visual_element(box, Eigen::Isometry3d::Identity(), material);
  body->AddVisualElement(visual_element);

  auto joint = std::make_unique<FixedJoint>(name + "_joint", X_box_to_parent);
  body->add_joint(tree->get_mutable_body(link_idx), std::move(joint));
  tree->bodies.push_back(std::move(body));
}

Eigen::Matrix<double, 3, 4> AddBoxSteppingStone(RigidBodyTreed* tree, const Eigen::Vector2d xy_pos, double yaw, const Eigen::Vector3d& box_size, const std::string& box_name, const Eigen::RowVector3d& color) {
  Eigen::Isometry3d X_box_to_world;
  X_box_to_world.linear() = Eigen::AngleAxisd(yaw, Eigen::Vector3d(0, 0, 1)).toRotationMatrix();
  X_box_to_world.translation() << xy_pos, box_size(2) / 2;
  AddBoxToBody(tree, 0, X_box_to_world, box_size, box_name, color);
  Eigen::Matrix<double, 3, 4> box_top_corners;
  double top_scale_factor = 0.6;
  box_top_corners.row(0) << box_size(0) / 2, box_size(0) / 2, -box_size(0) / 2, -box_size(0) / 2;
  box_top_corners.row(1) << box_size(1) / 2, -box_size(1) / 2, box_size(1) / 2, -box_size(1) / 2;
  box_top_corners.block<2, 4>(0, 0) *= top_scale_factor;
  box_top_corners.row(2) = Eigen::RowVector4d::Constant(box_size(2) / 2);
  for (int i = 0; i < 4; ++i) {
    box_top_corners.col(i) = X_box_to_world.linear() * box_top_corners.col(i) + X_box_to_world.translation();
  }
  return box_top_corners;
}

std::vector<Eigen::Matrix3Xd> AddSteppingStones(RigidBodyTreed* tree) {
  std::vector<Eigen::Matrix3Xd> stepping_regions;
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.1, 0.1), 0, Eigen::Vector3d(0.04, 0.04, 0.01), "stepping_stone1", Eigen::RowVector3d(0.1, 0.4, 0.3)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.1, 0.1), M_PI / 6, Eigen::Vector3d(0.08, 0.06, 0.03), "stepping_stone2", Eigen::RowVector3d(0.2, 0.1, 0.6)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.15, 0.12), -M_PI / 6, Eigen::Vector3d(0.04, 0.04, 0.04), "stepping_stone3", Eigen::RowVector3d(0.5, 0.2, 0.4)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.02, -0.08), M_PI / 4, Eigen::Vector3d(0.03, 0.03, 0.02), "stepping_stone4", Eigen::RowVector3d(0.8, 0.1, 0.3)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.12, -0.05), M_PI / 10, Eigen::Vector3d(0.04, 0.05, 0.03), "stepping_stone5", Eigen::RowVector3d(0.6, 0.1, 0.7)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.08, -0.03), -M_PI / 10, Eigen::Vector3d(0.04, 0.05, 0.06), "stepping_stone6", Eigen::RowVector3d(0.4, 0.2, 0.5)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.02, -0.05), M_PI / 10, Eigen::Vector3d(0.05, 0.05, 0.06), "stepping_stone7", Eigen::RowVector3d(0.2, 0.8, 0.1)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.02, 0.04), M_PI / 3, Eigen::Vector3d(0.04, 0.03, 0.04), "stepping_stone8", Eigen::RowVector3d(0.1, 0.2, 0.9)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.06, 0.04), M_PI / 10, Eigen::Vector3d(0.04, 0.05, 0.04), "stepping_stone9", Eigen::RowVector3d(0.9, 0.1, 0.2)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.02, 0.05), M_PI / 4, Eigen::Vector3d(0.03, 0.05, 0.03), "stepping_stone10", Eigen::RowVector3d(0.3, 0.7, 0.1)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.12, -0.05), M_PI / 10, Eigen::Vector3d(0.04, 0.05, 0.03), "stepping_stone11", Eigen::RowVector3d(0.1, 0.2, 0.3)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.07, -0.06), M_PI / 10, Eigen::Vector3d(0.04, 0.04, 0.04), "stepping_stone12", Eigen::RowVector3d(0.4, 0.2, 0.7)));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.08, 0.01), -M_PI / 10, Eigen::Vector3d(0.04, 0.06, 0.05), "stepping_stone13", Eigen::RowVector3d(0.3, 0.4, 0.1)));

  return stepping_regions;
}

GTEST_TEST(GlobalIKTest, LittleDogTest) {
  auto tree = ConstructLittleDog();
  const auto stepping_regions = AddSteppingStones(tree.get());

  int back_right_lower_leg_idx = tree->FindBodyIndex("back_right_lower_leg");
  int back_left_lower_leg_idx = tree->FindBodyIndex("back_left_lower_leg");
  int front_left_lower_leg_idx = tree->FindBodyIndex("front_left_lower_leg");
  int front_right_lower_leg_idx = tree->FindBodyIndex("front_right_lower_leg");

  Eigen::Vector3d back_r_toe(0.02, 0, -0.102);
  Eigen::Vector3d back_l_toe(0.02, 0, -0.102);
  Eigen::Vector3d front_r_toe(-0.02, 0, -0.102);
  Eigen::Vector3d front_l_toe(-0.02, 0, -0.102);
  AddPointToBody(tree.get(), back_right_lower_leg_idx, back_r_toe, "back_r_toe");
  AddPointToBody(tree.get(), back_left_lower_leg_idx, back_l_toe, "back_l_toe");
  AddPointToBody(tree.get(), front_right_lower_leg_idx, front_r_toe, "front_r_toe");
  AddPointToBody(tree.get(), front_left_lower_leg_idx, front_l_toe, "front_l_toe");

  GlobalInverseKinematics global_ik(*tree, 2);

  auto front_left_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(front_left_lower_leg_idx, front_l_toe, stepping_regions);
  auto front_right_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(front_right_lower_leg_idx, front_r_toe, stepping_regions);
  auto back_left_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(back_left_lower_leg_idx, back_l_toe, stepping_regions);
  auto back_right_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(back_right_lower_leg_idx, back_r_toe, stepping_regions);
  // No two toes on the same stepping stone.
  for (int i = 0; i < static_cast<int>(stepping_regions.size()); ++i) {
    global_ik.AddLinearConstraint(front_left_toe_stepping_stone(i) + front_right_toe_stepping_stone(i) + back_left_toe_stepping_stone(i) + back_right_toe_stepping_stone(i) <= 1);
  }

  auto base_rotmat = global_ik.body_rotation_matrix(1);
  auto base_pos = global_ik.body_position(1);
  global_ik.AddBoundingBoxConstraint(0.14, 0.2, base_pos(2));
  // The torso can yaw
  global_ik.AddBoundingBoxConstraint(Eigen::Vector3d(0, 0, 1), Eigen::Vector3d(0, 0, 1), base_rotmat.col(2));
  global_ik.AddBoundingBoxConstraint(Eigen::RowVector2d(0, 0), Eigen::RowVector2d(0, 0), base_rotmat.block<1, 2>(2, 0));
  global_ik.AddBoundingBoxConstraint(0.8, 1, base_rotmat(0, 0));
  global_ik.AddLinearConstraint(base_rotmat(0, 0) == base_rotmat(1, 1));
  global_ik.AddLinearConstraint(base_rotmat(1, 0) + base_rotmat(0, 1) == 0);
  // I do not want the leg to be too stretched
  const auto R_back_ll_leg = global_ik.body_rotation_matrix(back_left_lower_leg_idx);
  global_ik.AddBoundingBoxConstraint(0.9, 1, R_back_ll_leg(2, 2));
  const auto R_back_rl_leg = global_ik.body_rotation_matrix(back_right_lower_leg_idx);
  global_ik.AddBoundingBoxConstraint(0.9, 1, R_back_rl_leg(2, 2));
  const auto R_front_ll_leg = global_ik.body_rotation_matrix(front_left_lower_leg_idx);
  global_ik.AddBoundingBoxConstraint(0.9, 1, R_front_ll_leg(2, 2));
  const auto R_front_rl_leg = global_ik.body_rotation_matrix(front_right_lower_leg_idx);
  global_ik.AddBoundingBoxConstraint(0.9, 1, R_front_rl_leg(2, 2));

  // Front legs are in front of the rear legs
  const Vector3<symbolic::Expression> p_back_left_toe = global_ik.body_position(back_left_lower_leg_idx) + R_back_ll_leg * back_l_toe;
  const Vector3<symbolic::Expression> p_back_right_toe = global_ik.body_position(back_right_lower_leg_idx) + R_back_rl_leg * back_r_toe;
  const Vector3<symbolic::Expression> p_front_left_toe = global_ik.body_position(front_left_lower_leg_idx) + R_front_ll_leg * front_l_toe;
  const Vector3<symbolic::Expression> p_front_right_toe = global_ik.body_position(front_right_lower_leg_idx) + R_front_rl_leg * front_r_toe;

  global_ik.AddLinearConstraint(p_back_left_toe(0) <= p_front_left_toe(0) - 0.1);
  global_ik.AddLinearConstraint(p_back_right_toe(0) <= p_front_right_toe(0) - 0.1);

  solvers::GurobiSolver gurobi_solver;
  if (gurobi_solver.available()) {
    global_ik.SetSolverOption(solvers::SolverType::kGurobi, "OutputFlag", 1);
    auto sol_result = gurobi_solver.Solve(global_ik);
    EXPECT_EQ(sol_result, solvers::SolutionResult::kSolutionFound);
    const auto q_ik = global_ik.ReconstructGeneralizedPositionSolution();
    VisualizePosture(*tree, q_ik);
  }
}
/*
std::vector<Eigen::Matrix3Xd> AddUnreachableSteppingStones(RigidBodyTreed* tree) {
  std::vector<Eigen::Matrix3Xd> stepping_regions;
  Eigen::RowVector3d color(0.4, 0.2, 0.6);
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.3, 0.3), 0, Eigen::Vector3d(0.04, 0.04, 0.01), "stepping_stone1", color));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.3, 0.3), 0, Eigen::Vector3d(0.04, 0.04, -0.01), "stepping_stone2", color));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(-0.2, -0.3), 0, Eigen::Vector3d(0.04, 0.04, 0.02), "stepping_stone3", color));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.2, -0.3), 0, Eigen::Vector3d(0.04, 0.04, 0.03), "stepping_stone4", color));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.05, 0.3), 0, Eigen::Vector3d(0.04, 0.04, 0.03), "stepping_stone5", color));
  stepping_regions.push_back(AddBoxSteppingStone(tree, Eigen::Vector2d(0.3, 0.02), 0, Eigen::Vector3d(0.04, 0.06, 0.01), "stepping_stone6", color));
  return stepping_regions;
}

GTEST_TEST(GlobalIKTest, LittleDogInfeasibleTest) {
  auto tree = ConstructLittleDog();
  auto stepping_regions = AddUnreachableSteppingStones(tree.get());

  Eigen::RowVector3d obstacle_color(1, 0, 0);
  AddBoxSteppingStone(tree.get(), Eigen::Vector2d(0, 0), 0, Eigen::Vector3d(0.06, 0.06, 0.15), "obstacle1", obstacle_color);
  AddBoxSteppingStone(tree.get(), Eigen::Vector2d(0.15, 0.15), 0, Eigen::Vector3d(0.06, 0.06, 0.15), "obstacle2", obstacle_color);

  int back_right_lower_leg_idx = tree->FindBodyIndex("back_right_lower_leg");
  int back_left_lower_leg_idx = tree->FindBodyIndex("back_left_lower_leg");
  int front_left_lower_leg_idx = tree->FindBodyIndex("front_left_lower_leg");
  int front_right_lower_leg_idx = tree->FindBodyIndex("front_right_lower_leg");

  Eigen::Vector3d back_r_toe(0.02, 0, -0.1);
  Eigen::Vector3d back_l_toe(0.02, 0, -0.1);
  Eigen::Vector3d front_r_toe(-0.02, 0, -0.1);
  Eigen::Vector3d front_l_toe(-0.02, 0, -0.1);

  GlobalInverseKinematics global_ik(*tree, 2);

  auto front_left_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(front_left_lower_leg_idx, front_l_toe, stepping_regions);
  auto front_right_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(front_right_lower_leg_idx, front_r_toe, stepping_regions);
  auto back_left_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(back_left_lower_leg_idx, back_l_toe, stepping_regions);
  auto back_right_toe_stepping_stone = global_ik.BodyPointInOneOfRegions(back_right_lower_leg_idx, back_r_toe, stepping_regions);
  // No two toes on the same stepping stone.
  for (int i = 0; i < static_cast<int>(stepping_regions.size()); ++i) {
    global_ik.AddLinearConstraint(front_left_toe_stepping_stone(i) + front_right_toe_stepping_stone(i) + back_left_toe_stepping_stone(i) + back_right_toe_stepping_stone(i) <= 1);
  }

  auto base_pos = global_ik.body_position(1);
  global_ik.AddBoundingBoxConstraint(0.23, 0.3, base_pos(2));

  solvers::GurobiSolver gurobi_solver;
  if (gurobi_solver.available()) {
    global_ik.SetSolverOption(solvers::SolverType::kGurobi, "OutputFlag", 1);
    auto sol_result = gurobi_solver.Solve(global_ik);
    EXPECT_TRUE(sol_result == solvers::SolutionResult::kInfeasible_Or_Unbounded
    || sol_result == solvers::SolutionResult::kInfeasibleConstraints);
    if (sol_result == solvers::SolutionResult::kSolutionFound) {
      const auto q_ik = global_ik.ReconstructGeneralizedPositionSolution();
      VisualizePosture(*tree, q_ik);
    } else {
      Eigen::VectorXd q_infeasible(tree->get_num_positions());
      q_infeasible.setZero();
      q_infeasible(2) = 0.3;
      q_infeasible(3) = 1.0;
      VisualizePosture(*tree, q_infeasible);
    }
  }
}*/
}  // namespace
}  // namespace multibody
}  // namespace drake