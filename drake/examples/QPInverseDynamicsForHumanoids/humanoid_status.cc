#include "humanoid_status.h"
#include <iostream>

namespace drake {
namespace example {
namespace qp_inverse_dynamics {

// TODO(siyuan.feng@tri.global): These are hard coded for Valkyrie, and they
// should be included in the model file or loaded from a separate config file.
const Eigen::Vector3d HumanoidStatus::kFootToContactOffset = Eigen::Vector3d(0, 0, -0.09);
const Eigen::Vector3d HumanoidStatus::kFootToSensorPositionOffset =
    Eigen::Vector3d(0.0215646, 0.0, -0.051054);
const Matrix3d HumanoidStatus::kFootToSensorRotationOffset =
    Matrix3d(AngleAxisd(-M_PI, Eigen::Vector3d::UnitX()));

void HumanoidStatus::Update(double t, const Ref<const Eigen::VectorXd>& q,
                            const Ref<const Eigen::VectorXd>& v,
                            const Ref<const Eigen::VectorXd>& joint_torque,
                            const Ref<const Eigen::Vector6d>& l_wrench,
                            const Ref<const Eigen::Vector6d>& r_wrench) {
  if (q.size() != position_.size() || v.size() != velocity_.size() ||
      joint_torque.size() != joint_torque_.size()) {
    throw std::runtime_error("robot state update dimension mismatch.");
  }

  time_ = t;
  position_ = q;
  velocity_ = v;
  joint_torque_ = joint_torque;

  cache_.initialize(position_, velocity_);
  robot_.doKinematics(cache_, true);

  M_ = robot_.massMatrix(cache_);
  drake::eigen_aligned_std_unordered_map<RigidBody const*,
                                         drake::TwistVector<double>> f_ext;
  bias_term_ = robot_.dynamicsBiasTerm(cache_, f_ext);

  // com
  com_ = robot_.centerOfMass(cache_);
  J_com_ = robot_.centerOfMassJacobian(cache_);
  Jdot_times_v_com_ = robot_.centerOfMassJacobianDotTimesV(cache_);
  comd_ = J_com_ * velocity_;
  centroidal_momentum_matrix_ = robot_.centroidalMomentumMatrix(cache_);
  centroidal_momentum_matrix_dot_times_v_ =
      robot_.centroidalMomentumMatrixDotTimesV(cache_);
  centroidal_momentum_ = centroidal_momentum_matrix_ * velocity_;

  // body parts
  for (size_t i = 0; i < bodies_of_interest_.size(); i++)
    bodies_of_interest_[i].Update(robot_, cache_);

  // ft sensor
  foot_wrench_in_sensor_frame_[Side::LEFT] = l_wrench;
  foot_wrench_in_sensor_frame_[Side::RIGHT] = r_wrench;
  for (int i = 0; i < 2; i++) {
    // Rotate the sensor measurement to body frame first.
    foot_wrench_in_sensor_frame_[i].head(3) =
        kFootToSensorRotationOffset.transpose() *
        foot_wrench_in_sensor_frame_[i].head(3);
    foot_wrench_in_sensor_frame_[i].tail(3) =
        kFootToSensorRotationOffset.transpose() *
        foot_wrench_in_sensor_frame_[i].tail(3);

    // H^w_s = sensor frame = rs.foot_sensor(i).pose()
    // H^w_ak = world frame aligned, but located at ankle joint = [I,
    // rs.foot(i).pose().translation()]
    // To transform wrench from s frame to ak frame, we need H^ak_s.
    Eigen::Isometry3d H_s_to_w = foot_sensor(i).pose();
    Eigen::Isometry3d H_ak_to_w(Eigen::Isometry3d::Identity());
    H_ak_to_w.translation() = foot(i).pose().translation();
    foot_wrench_in_world_frame_[i] = transformSpatialForce(
        H_ak_to_w.inverse() * H_s_to_w, foot_wrench_in_sensor_frame_[i]);
  }

  // Compute center of pressure (CoP)
  Eigen::Vector2d cop_w[2];
  double Fz[2];
  for (int i = 0; i < 2; i++) {
    Fz[i] = foot_wrench_in_world_frame_[i][5];
    if (fabs(Fz[i]) < 1e-3) {
      cop_in_sensor_frame_[i][0] = 0;
      cop_in_sensor_frame_[i][1] = 0;
      cop_w[i][0] = foot(i).pose().translation()[0];
      cop_w[i][1] = foot(i).pose().translation()[1];
    } else {
      // CoP relative to the ft sensor
      cop_in_sensor_frame_[i][0] = -foot_wrench_in_sensor_frame_[i][1] /
                                   foot_wrench_in_sensor_frame_[i][5];
      cop_in_sensor_frame_[i][1] = foot_wrench_in_sensor_frame_[i][0] /
                                   foot_wrench_in_sensor_frame_[i][5];

      // CoP in the world frame
      cop_w[i][0] = -foot_wrench_in_world_frame_[i][1] / Fz[i] +
                    foot(i).pose().translation()[0];
      cop_w[i][1] = foot_wrench_in_world_frame_[i][0] / Fz[i] +
                    foot(i).pose().translation()[1];
    }
  }

  // This is assuming that both feet are on the same horizontal surface.
  cop_ = (cop_w[Side::LEFT] * Fz[Side::LEFT] +
          cop_w[Side::RIGHT] * Fz[Side::RIGHT]) /
         (Fz[Side::LEFT] + Fz[Side::RIGHT]);
}

} // end namespace qp_inverse_dynamics
} // end namespace example
} // end namespace drake
