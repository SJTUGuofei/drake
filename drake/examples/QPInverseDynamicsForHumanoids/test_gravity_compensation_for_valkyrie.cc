#include "drake/common/drake_path.h"
#include "drake/examples/QPInverseDynamicsForHumanoids/qp_controller.h"
#include "drake/systems/plants/joints/floating_base_types.h"

#include "drake/common/eigen_matrix_compare.h"
#include "gtest/gtest.h"

QPInput GenerateQPInput(const HumanoidStatus& robot_status, const Vector3d& desired_com, const Vector3d& Kp_com, const Vector3d& Kd_com, const VectorXd& desired_joints, const VectorXd& Kp_joints, const VectorXd& Kd_joints, const CartesianSetPoint& desired_pelvis, const CartesianSetPoint& desired_torso) {
  // Make input.
  QPInput input(robot_status.robot());

  // These represent the desired motions for the robot, and are typically
  // outputs of motion planner or hand-crafted behavior state machines.

  // Setup a PD tracking law for center of mass
  input.mutable_desired_comdd() = (Kp_com.array() * (desired_com - robot_status.com()).array() - Kd_com.array() * robot_status.comd().array()).matrix();
  input.mutable_w_com() = 1e3;

  // Minimize acceleration in the generalized coordinates.
  input.mutable_desired_vd() = (Kp_joints.array() * (desired_joints - robot_status.position()).array() - Kd_joints.array() * robot_status.velocity().array()).matrix();
  input.mutable_w_vd() = 1;

  // Setup tracking for various body parts.
  DesiredBodyAcceleration pelvdd_d(*robot_status.robot().FindBody("pelvis"));
  pelvdd_d.mutable_weight() = 1e1;
  pelvdd_d.mutable_acceleration() = desired_pelvis.ComputeTargetAcceleration(robot_status.pelvis().pose(), robot_status.pelvis().velocity());
  input.mutable_desired_body_accelerations().push_back(pelvdd_d);

  DesiredBodyAcceleration torsodd_d(*robot_status.robot().FindBody("torso"));
  torsodd_d.mutable_weight() = 1e1;
  torsodd_d.mutable_acceleration() = desired_torso.ComputeTargetAcceleration(robot_status.torso().pose(), robot_status.torso().velocity());
  input.mutable_desired_body_accelerations().push_back(torsodd_d);

  // Weights are set arbitrarily by the control designer, these typically
  // require tuning.
  input.mutable_w_basis_reg() = 1e-6;

  // Make contact points.
  ContactInformation left_foot_contact(
      *robot_status.robot().FindBody("leftFoot"), 4);
  left_foot_contact.mutable_contact_points().push_back(
      Vector3d(0.2, 0.05, -0.09));
  left_foot_contact.mutable_contact_points().push_back(
      Vector3d(0.2, -0.05, -0.09));
  left_foot_contact.mutable_contact_points().push_back(
      Vector3d(-0.05, -0.05, -0.09));
  left_foot_contact.mutable_contact_points().push_back(
      Vector3d(-0.05, 0.05, -0.09));

  ContactInformation right_foot_contact(
      *robot_status.robot().FindBody("rightFoot"), 4);
  right_foot_contact.mutable_contact_points() =
      left_foot_contact.contact_points();

  input.mutable_contact_info().push_back(left_foot_contact);
  input.mutable_contact_info().push_back(right_foot_contact);

  return input;
}

GTEST_TEST(testQPInverseDynamicsController, testStanding) {
  // Loads model.
  std::string urdf =
      drake::GetDrakePath() +
      std::string(
          "/examples/QPInverseDynamicsForHumanoids/valkyrie_sim_drake.urdf");
  RigidBodyTree robot(urdf, drake::systems::plants::joints::kRollPitchYaw);
  HumanoidStatus robot_status(robot);

  QPController con;
  QPInput input(robot_status.robot());
  QPOutput output(robot_status.robot());

  // Setup initial condition.
  VectorXd q(robot_status.robot().get_num_positions());
  VectorXd v(robot_status.robot().get_num_velocities());

  q = robot_status.GetNominalPosition();
  v.setZero();
  VectorXd q_ini = q;

  robot_status.Update(0, q, v,
                      VectorXd::Zero(robot_status.robot().actuators.size()),
                      Vector6d::Zero(), Vector6d::Zero());

  // Setup a tracking problem.
  Vector3d Kp_com = Vector3d::Constant(40);
  Vector3d Kd_com = Vector3d::Constant(12);
  VectorXd Kp_joints = VectorXd::Constant(robot_status.robot().get_num_positions(), 20);
  VectorXd Kd_joints = VectorXd::Constant(robot_status.robot().get_num_velocities(), 2);
  Vector6d Kp_pelvis = Vector6d::Constant(20);
  Vector6d Kd_pelvis = Vector6d::Constant(2);
  Vector6d Kp_torso = Vector6d::Constant(20);
  Vector6d Kd_torso = Vector6d::Constant(2);

  Vector3d desired_com = robot_status.com();
  VectorXd desired_q = robot_status.position();
  CartesianSetPoint desired_pelvis(robot_status.pelvis().pose(), Vector6d::Zero(), Vector6d::Zero(), Kp_pelvis, Kd_pelvis);
  CartesianSetPoint desired_torso(robot_status.torso().pose(), Vector6d::Zero(), Vector6d::Zero(), Kp_torso, Kd_torso);

  // Perturb initial condition
  v[robot_status.joint_name_to_position_index().at("torsoPitch")] += 0.2;
  robot_status.Update(0, q, v,
                      VectorXd::Zero(robot_status.robot().actuators.size()),
                      Vector6d::Zero(), Vector6d::Zero());

  double dt = 2e-3;
  double time = 0;

  // Feet should be stationary
  EXPECT_TRUE(robot_status.foot(Side::LEFT).velocity().norm() < 1e-10);
  EXPECT_TRUE(robot_status.foot(Side::RIGHT).velocity().norm() < 1e-10);

  while(time < 5) {
    input = GenerateQPInput(robot_status, desired_com, Kp_com, Kd_com, desired_q, Kp_joints, Kd_joints, desired_pelvis, desired_torso);
    int status = con.Control(robot_status, input, &output);
    if (status)
      break;

    // Dummy integration.
    q += v * dt;
    v += output.vd() * dt;
    time += dt;

    robot_status.Update(time, q, v,
        output.joint_torque(),
        Vector6d::Zero(), Vector6d::Zero());
  }

  // Robot should be stabilized.
  EXPECT_TRUE(robot_status.foot(Side::LEFT).velocity().norm() < 1e-6);
  EXPECT_TRUE(robot_status.foot(Side::RIGHT).velocity().norm() < 1e-6);

  EXPECT_TRUE(drake::CompareMatrices(q, q_ini, 1e-4, drake::MatrixCompareType::absolute));
  EXPECT_TRUE(drake::CompareMatrices(v, VectorXd::Zero(robot_status.robot().get_num_velocities()), 1e-3, drake::MatrixCompareType::absolute));
}
