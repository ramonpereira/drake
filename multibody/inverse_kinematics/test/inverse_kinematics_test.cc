#include "drake/multibody/inverse_kinematics/inverse_kinematics.h"

#include <gtest/gtest.h>

#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/multibody/inverse_kinematics/test/inverse_kinematics_test_utilities.h"
#include "drake/solvers/create_constraint.h"

namespace drake {
namespace multibody {
Eigen::Quaterniond Vector4ToQuaternion(
    const Eigen::Ref<const Eigen::Vector4d>& q) {
  return Eigen::Quaterniond(q(0), q(1), q(2), q(3));
}

class TwoFreeBodiesTest : public ::testing::Test {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(TwoFreeBodiesTest)

  TwoFreeBodiesTest()
      : two_bodies_plant_(ConstructTwoFreeBodiesPlant<double>()),
        // TODO(hongkai.dai) call GetFrameByName()
        body1_frame_(two_bodies_plant_->GetFrameByName("body1")),
        body2_frame_(two_bodies_plant_->GetFrameByName("body2")),
        ik_(*two_bodies_plant_) {
    // TODO(hongkai.dai): The unit quaternion constraint should be added by
    // InverseKinematics automatically.
    ik_.get_mutable_prog()->AddConstraint(
        solvers::internal::ParseQuadraticConstraint(
            ik_.q().head<4>().cast<symbolic::Expression>().squaredNorm(), 1,
            1));
    ik_.get_mutable_prog()->AddConstraint(
        solvers::internal::ParseQuadraticConstraint(
            ik_.q().segment<4>(7).cast<symbolic::Expression>().squaredNorm(), 1,
            1));
  }

  ~TwoFreeBodiesTest() override {}

  void RetrieveSolution() {
    const auto q_sol = ik_.prog().GetSolution(ik_.q());
    body1_quaternion_sol_ = Vector4ToQuaternion(q_sol.head<4>());
    body1_position_sol_ = q_sol.segment<3>(4);
    body2_quaternion_sol_ = Vector4ToQuaternion(q_sol.segment<4>(7));
    body2_position_sol_ = q_sol.tail<3>();
  }

 protected:
  std::unique_ptr<multibody_plant::MultibodyPlant<double>> two_bodies_plant_;
  const Frame<double>& body1_frame_;
  const Frame<double>& body2_frame_;
  InverseKinematics ik_;

  Eigen::Quaterniond body1_quaternion_sol_;
  Eigen::Quaterniond body2_quaternion_sol_;
  Eigen::Vector3d body1_position_sol_;
  Eigen::Vector3d body2_position_sol_;
};

GTEST_TEST(InverseKinematicsTest, ConstructorWithJointLimits) {
  // Constructs an inverse kinematics problem for IIWA robot, make sure that
  // the joint limits are imposed.
  auto plant = ConstructIiwaPlant("iiwa14_no_collision.sdf", 0.01);

  InverseKinematics ik(*plant);
  // Now check the joint limits.
  VectorX<double> lower_limits(7);
  // The lower and upper joint limits are copied from the SDF file. Please make
  // sure the values are in sync with the SDF file.
  lower_limits << -2.96706, -2.0944, -2.96706, -2.0944, -2.96706, -2.0944,
      -3.05433;
  VectorX<double> upper_limits(7);
  upper_limits = -lower_limits;
  // Check if q_test will satisfy the joint limit constraint imposed from the
  // IK constructor.
  auto q_test_bound = ik.get_mutable_prog()->AddBoundingBoxConstraint(
      Eigen::VectorXd::Zero(7), Eigen::VectorXd::Zero(7), ik.q());
  auto check_q_test = [&ik, &q_test_bound](const Eigen::VectorXd& q_test) {
    q_test_bound.evaluator()->UpdateLowerBound(q_test);
    q_test_bound.evaluator()->UpdateUpperBound(q_test);
    const auto result = ik.get_mutable_prog()->Solve();
    return result == solvers::SolutionResult::kSolutionFound;
  };
  for (int i = 0; i < 7; ++i) {
    Eigen::VectorXd q_good = Eigen::VectorXd::Zero(7);
    q_good(i) = lower_limits(i) * 0.01 + upper_limits(i) * 0.99;
    EXPECT_TRUE(check_q_test(q_good));
    q_good(i) = lower_limits(i) * 0.99 + upper_limits(i) * 0.01;
    EXPECT_TRUE(check_q_test(q_good));
    Eigen::VectorXd q_bad = q_good;
    q_bad(i) = -0.01 * lower_limits(i) + 1.01 * upper_limits(i);
    EXPECT_FALSE(check_q_test(q_bad));
    q_bad(i) = 1.01 * lower_limits(i) - 0.01 * upper_limits(i);
    EXPECT_FALSE(check_q_test(q_bad));
  }
}

TEST_F(TwoFreeBodiesTest, PositionConstraint) {
  const Eigen::Vector3d p_BQ(0.2, 0.3, 0.5);
  const Eigen::Vector3d p_AQ_lower(-0.1, -0.2, -0.3);
  const Eigen::Vector3d p_AQ_upper(-0.05, -0.12, -0.28);
  ik_.AddPositionConstraint(body1_frame_, p_BQ, body2_frame_, p_AQ_lower,
                            p_AQ_upper);

  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().head<4>(),
                                          Eigen::Vector4d(1, 0, 0, 0));
  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().segment<4>(7),
                                          Eigen::Vector4d(1, 0, 0, 0));
  const auto result = ik_.get_mutable_prog()->Solve();
  EXPECT_EQ(result, solvers::SolutionResult::kSolutionFound);
  RetrieveSolution();
  const Eigen::Vector3d p_AQ = body2_quaternion_sol_.inverse() *
                               (body1_quaternion_sol_ * p_BQ +
                                body1_position_sol_ - body2_position_sol_);
  const double tol = 1E-6;
  EXPECT_TRUE(
      (p_AQ.array() <= p_AQ_upper.array() + Eigen::Array3d::Constant(tol))
          .all());
  EXPECT_TRUE(
      (p_AQ.array() >= p_AQ_lower.array() - Eigen::Array3d::Constant(tol))
          .all());
}

TEST_F(TwoFreeBodiesTest, OrientationConstraint) {
  const double angle_bound = 0.05 * M_PI;

  const math::RotationMatrix<double> R_AbarA(Eigen::AngleAxisd(
      0.1 * M_PI, Eigen::Vector3d(0.2, 0.4, 1.2).normalized()));
  const math::RotationMatrix<double> R_BbarB(Eigen::AngleAxisd(
      0.2 * M_PI, Eigen::Vector3d(1.2, 2.1, -0.2).normalized()));

  ik_.AddOrientationConstraint(body1_frame_, R_AbarA, body2_frame_, R_BbarB,
                               angle_bound);

  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().head<4>(),
                                          Eigen::Vector4d(1, 0, 0, 0));
  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().segment<4>(7),
                                          Eigen::Vector4d(1, 0, 0, 0));
  const auto result = ik_.get_mutable_prog()->Solve();
  EXPECT_EQ(result, solvers::SolutionResult::kSolutionFound);
  const auto q_sol = ik_.prog().GetSolution(ik_.q());
  RetrieveSolution();
  const Eigen::Matrix3d R_AbarBbar =
      (body1_quaternion_sol_.inverse() * body2_quaternion_sol_)
          .toRotationMatrix();
  const Eigen::Matrix3d R_AB =
      R_AbarA.matrix().transpose() * R_AbarBbar * R_BbarB.matrix();
  const double angle = Eigen::AngleAxisd(R_AB).angle();
  EXPECT_LE(angle, angle_bound + 1E-6);
}

TEST_F(TwoFreeBodiesTest, GazeTargetConstraint) {
  const Eigen::Vector3d p_AS(0.01, 0.2, 0.4);
  const Eigen::Vector3d n_A(0.2, 0.4, -0.1);
  const Eigen::Vector3d p_BT(0.4, -0.2, 1.5);
  const double cone_half_angle{0.2 * M_PI};

  ik_.AddGazeTargetConstraint(body1_frame_, p_AS, n_A, body2_frame_, p_BT,
                              cone_half_angle);

  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().head<4>(),
                                          Eigen::Vector4d(1, 0, 0, 0));
  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().segment<4>(7),
                                          Eigen::Vector4d(1, 0, 0, 0));
  const auto result = ik_.get_mutable_prog()->Solve();
  EXPECT_EQ(result, solvers::SolutionResult::kSolutionFound);

  RetrieveSolution();
  const Eigen::Vector3d p_WS =
      body1_quaternion_sol_ * p_AS + body1_position_sol_;
  const Eigen::Vector3d p_WT =
      body2_quaternion_sol_ * p_BT + body2_position_sol_;
  const Eigen::Vector3d p_ST_A =
      body1_quaternion_sol_.inverse() * (p_WT - p_WS);
  EXPECT_GE(p_ST_A.dot(n_A),
            std::cos(cone_half_angle) * p_ST_A.norm() * n_A.norm() - 1E-3);
}

TEST_F(TwoFreeBodiesTest, AngleBetweenVectorsConstraint) {
  const Eigen::Vector3d n_A(0.2, -0.4, 0.9);
  const Eigen::Vector3d n_B(1.4, -0.1, 1.8);

  const double angle_lower{0.2 * M_PI};
  const double angle_upper{0.2 * M_PI};

  ik_.AddAngleBetweenVectorsConstraint(body1_frame_, n_A, body2_frame_, n_B,
                                       angle_lower, angle_upper);
  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().head<4>(),
                                          Eigen::Vector4d(1, 0, 0, 0));
  ik_.get_mutable_prog()->SetInitialGuess(ik_.q().segment<4>(7),
                                          Eigen::Vector4d(1, 0, 0, 0));
  const auto result = ik_.get_mutable_prog()->Solve();
  EXPECT_EQ(result, solvers::SolutionResult::kSolutionFound);

  RetrieveSolution();

  const Eigen::Vector3d n_A_W = body1_quaternion_sol_ * n_A;
  const Eigen::Vector3d n_B_W = body2_quaternion_sol_ * n_B;

  const double angle =
      std::acos(n_A_W.dot(n_B_W) / (n_A_W.norm() * n_B_W.norm()));
  EXPECT_NEAR(angle, angle_lower, 1E-6);
}
}  // namespace multibody
}  // namespace drake
