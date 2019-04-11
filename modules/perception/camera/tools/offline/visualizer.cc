/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the License);
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "modules/perception/camera/tools/offline/visualizer.h"

#include <fstream>
#include <iostream>
#include <limits>
#include "cyber/common/log.h"

namespace apollo {
namespace perception {
namespace camera {

std::vector<cv::Scalar> colorlistobj = {
    cv::Scalar(0, 0, 255),     cv::Scalar(0, 100, 255),
    cv::Scalar(0, 200, 255),   cv::Scalar(100, 255, 255),
    cv::Scalar(127, 255, 255), cv::Scalar(255, 100, 255),
    cv::Scalar(255, 0, 255),   cv::Scalar(255, 255, 100),
    cv::Scalar(255, 255, 0),   cv::Scalar(255, 0, 100),
    cv::Scalar(255, 0, 0),     cv::Scalar(0, 255, 0),
    cv::Scalar(100, 255, 100)};

std::map<base::LaneLinePositionType, cv::Scalar> colormapline = {
    {base::LaneLinePositionType::UNKNOWN, cv::Scalar(0, 0, 255)},
    {base::LaneLinePositionType::FOURTH_LEFT, cv::Scalar(0, 100, 255)},
    {base::LaneLinePositionType::THIRD_LEFT, cv::Scalar(0, 200, 255)},
    {base::LaneLinePositionType::ADJACENT_LEFT, cv::Scalar(100, 255, 255)},
    {base::LaneLinePositionType::EGO_LEFT, cv::Scalar(200, 255, 255)},
    {base::LaneLinePositionType::EGO_CENTER, cv::Scalar(255, 100, 255)},
    {base::LaneLinePositionType::EGO_RIGHT, cv::Scalar(255, 0, 255)},
    {base::LaneLinePositionType::ADJACENT_RIGHT, cv::Scalar(255, 255, 100)},
    {base::LaneLinePositionType::THIRD_RIGHT, cv::Scalar(255, 255, 0)},
    {base::LaneLinePositionType::FOURTH_RIGHT, cv::Scalar(255, 0, 100)},
    {base::LaneLinePositionType::OTHER, cv::Scalar(255, 0, 0)},
    {base::LaneLinePositionType::CURB_LEFT, cv::Scalar(0, 255, 0)},
    {base::LaneLinePositionType::CURB_RIGHT, cv::Scalar(100, 255, 100)}};

Eigen::Matrix3d Camera2CarHomograph(Eigen::Matrix3d intrinsic,
                                    Eigen::Matrix4d extrinsic_camera2lidar,
                                    Eigen::Matrix4d extrinsic_lidar2imu,
                                    double pitch_adj) {
  AINFO << "intrinsic parameter of camera: " << intrinsic;
  AINFO << "extrinsic parameter of camera to lidar: " << extrinsic_camera2lidar;
  AINFO << "extrinsic parameter of lidar to imu: " << extrinsic_lidar2imu;
  // rotate 90 degree around z axis to make x point forward
  Eigen::Matrix4d Rz;
  Rz << 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1;
  Eigen::Matrix4d extrinsic_camera2car;
  extrinsic_camera2car = extrinsic_camera2lidar * extrinsic_lidar2imu * Rz;
  // adjust pitch in camera coords
  Eigen::Matrix4d Rx;
  Rx << 1, 0, 0, 0, 0, cos(pitch_adj), -sin(pitch_adj), 0, 0, sin(pitch_adj),
      cos(pitch_adj), 0, 0, 0, 0, 1;
  extrinsic_camera2car = extrinsic_camera2car * Rx;
  AINFO << "extrinsic parameter from camera to car: " << extrinsic_camera2car;

  // compute the homography matrix, such that H [u, v, 1]' ~ [X_l, Y_l, 1]
  Eigen::Matrix3d K = intrinsic;
  Eigen::Matrix3d R = extrinsic_camera2car.block(0, 0, 3, 3);
  Eigen::Vector3d T = extrinsic_camera2car.block(0, 3, 3, 1);
  Eigen::Matrix3d H;

  H.block(0, 0, 3, 2) = (K * R.transpose()).block(0, 0, 3, 2);
  H.block(0, 2, 3, 1) = -K * R.transpose() * T;
  return H.inverse();
}

bool Visualizer::Init(const std::vector<std::string> &camera_names,
                      TransformServer *tf_server) {
  tf_server_ = tf_server;
  CHECK(tf_server_ != nullptr);
  last_timestamp_ = 0;
  small_h_ = static_cast<int>(image_height_ * scale_ratio_);
  small_w_ = static_cast<int>(image_width_ * scale_ratio_);
  world_h_ = 2 * small_h_;

  for (size_t i = 0; i < camera_names.size(); ++i) {
    camera_image_[camera_names[i]] =
        cv::Mat(small_h_, small_w_, CV_8UC3, cv::Scalar(0, 0, 0));
  }
  world_image_ = cv::Mat(world_h_, wide_pixel_, CV_8UC3, cv::Scalar(0, 0, 0));
  color_cipv_ = cv::Scalar(255, 255, 255);

  return true;
}

bool Visualizer::Init_all_info_single_camera(
    const std::string &camera_name,
    const std::map<std::string, Eigen::Matrix3f> &intrinsic_map,
    const std::map<std::string, Eigen::Matrix4d> &extrinsic_map,
    const Eigen::Matrix4d &ex_lidar2imu,
    const double pitch_adj_degree,
    const double yaw_adj_degree,
    const double roll_adj_degree,
    const int image_height,
    const int image_width) {
  image_height_ = image_height;
  image_width_ = image_width;
  intrinsic_map_ = intrinsic_map;
  extrinsic_map_ = extrinsic_map;
  ex_lidar2imu_ = ex_lidar2imu;

  last_timestamp_ = 0;
  small_h_ = static_cast<int>(image_height_ * scale_ratio_);
  small_w_ = static_cast<int>(image_width_ * scale_ratio_);
  world_h_ = 2 * small_h_;

  AINFO << "world_h_: " << world_h_;
  AINFO << "wide_pixel_: " << wide_pixel_;
  AINFO << "small_h_: " << small_h_;
  AINFO << "small_w_: " << small_w_;
  camera_image_[camera_name + "_2D"] =
      cv::Mat(small_h_, small_w_, CV_8UC3, cv::Scalar(0, 0, 0));
  camera_image_[camera_name + "_3D"] =
      cv::Mat(small_h_, small_w_, CV_8UC3, cv::Scalar(0, 0, 0));
  world_image_ = cv::Mat(world_h_, wide_pixel_, CV_8UC3, cv::Scalar(0, 0, 0));
  draw_range_circle();

  // 1. transform camera_>lidar
  ex_camera2lidar_ = extrinsic_map_.at(camera_name);
  AINFO << "ex_camera2lidar_ = " << extrinsic_map_.at(camera_name);

  AINFO << "ex_lidar2imu_ =" << ex_lidar2imu_;

  // 2. transform camera->lidar->imu
  ex_camera2imu_ = ex_lidar2imu_ * ex_camera2lidar_;
  AINFO << "ex_camera2imu_ =" << ex_camera2imu_;

  // intrinsic camera parameter
  K_ = intrinsic_map_.at(camera_name).cast<double>();
  AINFO << "intrinsic K_ =" << K_;
  // homography_ground2image_.setIdentity();
  // homography_image2ground_.setIdentity();

  // rotate 90 degree around z axis to make x point forward
  // double imu_height = 0;  // imu height should be considred later
  ex_imu2car_ << 0, 1, 0, 0,  // cos(90), sin(90), 0,
                -1, 0, 0, 0,  // -sin(90),  cos(90), 0,
                0, 0, 1, 0,  // 0,              0, 1
                0, 0, 0, 1;

  // 3. transform camera->lidar->imu->car
  ex_camera2car_ = ex_imu2car_ * ex_camera2imu_;

  AINFO << "ex_camera2car_ =" << ex_camera2car_;

  // Adjust angle
  adjust_angles(camera_name, pitch_adj_degree, yaw_adj_degree, roll_adj_degree);

  AINFO << "homography_image2ground_ =" << homography_image2ground_;

  AINFO << "homography_ground2image_ =" << homography_ground2image_;

  // compute FOV points
  p_fov_1_.x = 0;
  p_fov_1_.y = static_cast<int>(image_height_ * fov_cut_ratio_);

  p_fov_2_.x = image_width_ - 1;
  p_fov_2_.y = static_cast<int>(image_height_ * fov_cut_ratio_);

  p_fov_3_.x = 0;
  p_fov_3_.y = image_height_ - 1;

  p_fov_4_.x = image_width_ - 1;
  p_fov_4_.y = image_height_ - 1;

  AINFO << "p_fov_1_ =" << p_fov_1_;
  AINFO << "p_fov_2_ =" << p_fov_2_;
  AINFO << "p_fov_3_ =" << p_fov_3_;
  AINFO << "p_fov_4_ =" << p_fov_4_;

  vp1_[0] = 1024.0;
  if (K_(0, 0) >= 1.0) {
    vp1_[1] = (image_width_ >> 1) * vp1_[0] / K_(0, 0);
  } else {
    AWARN
        << "Focal length (" << K_(0, 0)
        << " in pixel) is incorrect. Please check camera intrinsic parameters.";
    vp1_[1] = vp1_[0] * 0.25;
  }

  vp2_[0] = vp1_[0];
  vp2_[1] = -vp1_[1];

  AINFO << "vanishing point 1:" << vp1_;
  AINFO << "vanishing point 2:" << vp2_;

  pitch_adj_degree_ = pitch_adj_degree;
  yaw_adj_degree_ = yaw_adj_degree;
  roll_adj_degree_ = roll_adj_degree;

  reset_key();

  return true;
}

bool Visualizer::adjust_angles(const std::string &camera_name,
                               const double pitch_adj_degree,
                               const double yaw_adj_degree,
                               const double roll_adj_degree) {
  // Convert degree angles to radian angles
  double pitch_adj_radian = pitch_adj_degree * degree_to_radian_factor_;
  double yaw_adj_radian = yaw_adj_degree * degree_to_radian_factor_;
  double roll_adj_radian = roll_adj_degree * degree_to_radian_factor_;

  // We use "right handed ZYX" coordinate system for euler angles
  // adjust pitch yaw roll in camera coords
  // Remember that camera coordinate
  // (Z)----> X
  //  |
  //  |
  //  V
  //  Y
  Eigen::Matrix4d Rx;  // pitch
  Rx << 1, 0, 0, 0,
        0, cos(pitch_adj_radian), -sin(pitch_adj_radian), 0,
        0, sin(pitch_adj_radian), cos(pitch_adj_radian), 0,
        0, 0, 0, 1;
  Eigen::Matrix4d Ry;  // yaw
  Ry << cos(yaw_adj_radian), 0, sin(yaw_adj_radian), 0,
        0, 1, 0, 0,
        -sin(yaw_adj_radian), 0, cos(yaw_adj_radian), 0,
        0, 0, 0, 1;
  Eigen::Matrix4d Rz;  // roll
  Rz << cos(roll_adj_radian), -sin(roll_adj_radian), 0, 0,
        sin(roll_adj_radian), cos(roll_adj_radian), 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1;

  adjusted_camera2car_ = ex_camera2car_ * Rz * Ry * Rx;
  AWARN << "adjusted_camera2car_: " << adjusted_camera2car_;

  // Get homography from projection matrix
  // ====
  // Version 1. Direct

  // compute the homography matrix, such that H [u, v, 1]' ~ [X_l, Y_l, 1]
  Eigen::Matrix3d R = adjusted_camera2car_.block(0, 0, 3, 3);
  Eigen::Vector3d T = adjusted_camera2car_.block(0, 3, 3, 1);
  Eigen::Matrix3d H;
  Eigen::Matrix3d H_inv;

  H.block(0, 0, 3, 2) = (K_ * R.transpose()).block(0, 0, 3, 2);
  H.block(0, 2, 3, 1) = -K_ * R.transpose() * T;
  H_inv = H.inverse();
  homography_ground2image_ = H;
  homography_image2ground_ = H_inv;

  // Version 2. Conceptual
  // ex_car2camera_ = adjusted_camera2car_.inverse();

  // // compute the homography matrix, such that H [u, v, 1]' ~ [X_l, Y_l, 1]
  // Eigen::Matrix3d R = ex_car2camera_.block(0, 0, 3, 3);
  // Eigen::Vector3d T = ex_car2camera_.block(0, 3, 3, 1);

  // projection_matrix_.setIdentity();
  // projection_matrix_.block(0, 0, 3, 3) = K_ * R;
  // projection_matrix_.block(0, 3, 3, 1) = K_ * T;

  // homography_ground2image_.block(0, 0, 3, 2)
  //   = projection_matrix_.block(0, 0, 3, 2);
  // homography_ground2image_.block(0, 2, 3, 1)
  //   = projection_matrix_.block(0, 3, 3, 1);

  // AINFO << "homography_ground2image_: ";
  // AINFO << homography_ground2image_;

  // homography_image2ground_ = homography_ground2image_.inverse();

  return true;
}

bool Visualizer::SetDirectory(const std::string &path) {
  int is_success = 1;
  std::string command;
  command = "mkdir -p " + path;
  is_success = system(command.c_str());
  command = "rm " + path + "/*.jpg";
  is_success = system(command.c_str());
  path_ = path;
  if (is_success > 0) {
    return true;
  } else {
    return false;
  }
}

std::string Visualizer::type_to_string(
    const apollo::perception::base::ObjectType type) {
  switch (type) {
    case apollo::perception::base::ObjectType::UNKNOWN:
      return "UNKN";
    case apollo::perception::base::ObjectType::UNKNOWN_MOVABLE:
      return "U_MO";
    case apollo::perception::base::ObjectType::UNKNOWN_UNMOVABLE:
      return "UNMO";
    case apollo::perception::base::ObjectType::PEDESTRIAN:
      return "PED";
    case apollo::perception::base::ObjectType::BICYCLE:
      return "CYC";
    case apollo::perception::base::ObjectType::VEHICLE:
      return "VEH";
    default:
      break;
  }
  return "WRNG";
}

std::string Visualizer::sub_type_to_string(
    const apollo::perception::base::ObjectSubType type) {
  switch (type) {
    case apollo::perception::base::ObjectSubType::UNKNOWN:
      return "UNKN";
    case apollo::perception::base::ObjectSubType::UNKNOWN_MOVABLE:
      return "U_MO";
    case apollo::perception::base::ObjectSubType::UNKNOWN_UNMOVABLE:
      return "UNMO";
    case apollo::perception::base::ObjectSubType::CAR:
      return "CAR";
    case apollo::perception::base::ObjectSubType::VAN:
      return "VAN";
    case apollo::perception::base::ObjectSubType::TRUCK:
      return "TRUC";
    case apollo::perception::base::ObjectSubType::BUS:
      return "BUS";
    case apollo::perception::base::ObjectSubType::CYCLIST:
      return "CYC";
    case apollo::perception::base::ObjectSubType::MOTORCYCLIST:
      return "MCYC";
    case apollo::perception::base::ObjectSubType::TRICYCLIST:
      return "TCYC";
    case apollo::perception::base::ObjectSubType::PEDESTRIAN:
      return "PED";
    case apollo::perception::base::ObjectSubType::TRAFFICCONE:
      return "CONE";
    default:
      break;
  }
  return "WRNG";
}

bool Visualizer::reset_key() {
  use_class_color_ = true;
  capture_screen_ = false;
  capture_video_ = false;
  show_camera_box2d_ = true;
  show_camera_box3d_ = true;
  show_camera_bdv_ = true;
  show_radar_pc_ = true;
  show_fusion_ = false;
  show_associate_color_ = false;
  show_type_id_label_ = true;
  show_verbose_ = false;
  show_lane_ = true;
  show_trajectory_ = true;
  show_vp_grid_ = true;  // show vanishing point and ground plane grid
  draw_lane_objects_ = true;
  show_box_ = true;
  show_velocity_ = false;
  show_polygon_ = true;
  show_text_ = false;
  show_help_text_ = false;
  return true;
}

double Visualizer::regularize_angle(const double radian_angle) {
  if (radian_angle <= -M_PI) {
    return radian_angle + M_PI * 2.0;
  } else if (radian_angle > M_PI) {
    return radian_angle - M_PI * 2.0;
  }
  return radian_angle;
}

// ZYX Euler angles to quaternion
bool Visualizer::euler_to_quaternion(Eigen::Vector4d *quarternion,
                                     const double pitch_radian,
                                     const double yaw_radian,
                                     const double roll_radian) {
  // // Option 1. ZYX Euler to quortonian
  // double cy = cos(yaw_radian * 0.5);
  // double sy = sin(yaw_radian * 0.5);
  // double cp = cos(pitch_radian * 0.5);
  // double sp = sin(pitch_radian * 0.5);
  // double cr = cos(roll_radian * 0.5);
  // double sr = sin(roll_radian * 0.5);

  // quarternion[0] = sy * cp * cr - cy * sp * sr;  // Q.x
  // quarternion[1] = cy * sp * cr + sy * cp * sr;  // Q.y
  // quarternion[2] = cy * cp * sr - sy * sp * cr;  // Q.z
  // quarternion[3] = cy * cp * cr + sy * sp * sr;  // Q.w

  // AINFO << "fast quarternion(x, y, z, w): ("
  //       << quarternion[0] << ", "
  //       << quarternion[1] << ", "
  //       << quarternion[2] << ", "
  //       << quarternion[3] << ")";

  // Option 2. Rotation matrix to quaternion
  Eigen::Matrix3d Rx;  // pitch
  Rx << 1, 0, 0,
        0, cos(pitch_radian), -sin(pitch_radian),
        0, sin(pitch_radian), cos(pitch_radian);
  Eigen::Matrix3d Ry;  // yaw
  Ry << cos(yaw_radian), 0, sin(yaw_radian),
        0, 1, 0,
        -sin(yaw_radian), 0, cos(yaw_radian);
  Eigen::Matrix3d Rz;  // roll
  Rz << cos(roll_radian), -sin(roll_radian), 0,
        sin(roll_radian), cos(roll_radian), 0,
        0, 0, 1;
  Eigen::Matrix3d R;
  R = Rz * Ry * Rx;
  AINFO << "Rotation matrix R: " << R;
  double qw = 0.5 * sqrt(1.0 + R(0, 0) + R(1, 1) + R(2, 2));
  if (fabs(qw) > 1.0e-6) {
    (*quarternion)[0] = 0.25 * (R(2, 1) - R(1, 2)) / qw;  // Q.x
    (*quarternion)[1] = 0.25 * (R(0, 2) - R(2, 0)) / qw;  // Q.y
    (*quarternion)[2] = 0.25 * (R(1, 0) - R(0, 1)) / qw;  // Q.z
    (*quarternion)[3] = qw;  // Q.w
    AINFO << "quarternion(x, y, z, w): ("
          << (*quarternion)[0] << ", "
          << (*quarternion)[1] << ", "
          << (*quarternion)[2] << ", "
          << (*quarternion)[3] << ")";
  } else {
    double qx = 0.5 * sqrt(1.0 + R(0, 0) - R(1, 1) - R(2, 2));
    if (fabs(qx) < 1.0e-6) {
      AWARN << "quarternion is degenerate qw: " << qw << "qx: " << qx;
      return false;
    }
    (*quarternion)[0] = qx;                               // Q.x
    (*quarternion)[1] = 0.25 * (R(0, 1) + R(1, 0)) / qx;  // Q.y
    (*quarternion)[2] = 0.25 * (R(0, 2) + R(2, 0)) / qx;  // Q.z
    (*quarternion)[3] = 0.25 * (R(2, 1) - R(1, 2)) / qx;  // Q.w
    AINFO << "second quarternion(x, y, z, w): ("
          << (*quarternion)[0] << ", "
          << (*quarternion)[1] << ", "
          << (*quarternion)[2] << ", "
          << (*quarternion)[3] << ")";
  }
  return true;
}

bool Visualizer::copy_backup_file(const std::string &filename) {
  static int index = 0;
  // int last_index = 0;
  // std::string files = filename + "*";
  // for (const auto &file : std::filesysfs::directory_iterator(files)) {
  //     AINFO << file.path() << std::endl;
  //     // Extract index
  //     last_index = get_index(file.path());
  // }
  // index = last_index;

  ++index;
  std::string yaml_bak_file = filename + "__" + std::to_string(index);
  AINFO << "yaml_backup_file: " << yaml_bak_file;

  std::string command = "cp " + filename + " " + yaml_bak_file;
  int ret = system(command.c_str());
  if (ret != 0) {
    AINFO << "Cannot backup the file, " << filename;
  } else {
    AINFO << "Backup file, " << filename << " saved.";
  }

  return true;
}

bool Visualizer::save_extrinsic_in_yaml(const std::string &camera_name,
                                        const Eigen::Matrix4d &extrinsic,
                                        const Eigen::Vector4d &quarternion,
                                        const double pitch_radian,
                                        const double yaw_radian,
                                        const double roll_radian) {
  std::string yaml_file =
      FLAGS_obs_sensor_intrinsic_path + "/" + camera_name + "_extrinsics.yaml";

  copy_backup_file(yaml_file);

  AINFO << "extrinsic: " << extrinsic;

  // Save data
  // Option 1. Save using streaming
  std::ofstream y_file(yaml_file);

  y_file << "header:\n";
  y_file << "  seq: 0\n";
  y_file << "  stamp:\n";
  y_file << "    secs: 0\n";
  y_file << "    nsecs: 0\n";
  y_file << "  frame_id: velodyne128\n";
  y_file << "child_frame_id: front_6mm\n";
  y_file << "transform:\n";
  y_file << "  translation:\n";
  y_file << "    x: " << extrinsic(0, 3) << "\n";
  y_file << "    y: " << extrinsic(1, 3) << "\n";
  y_file << "    z: " << extrinsic(2, 3) << "\n";
  y_file << "  rotation:\n";
  y_file << "     x: " << quarternion[0] << "\n";
  y_file << "     y: " << quarternion[1] << "\n";
  y_file << "     z: " << quarternion[2] << "\n";
  y_file << "     w: " << quarternion[3] << "\n";
  y_file << "  euler_angles_degree:\n";
  y_file << "     pitch: " << pitch_radian * radian_to_degree_factor_ << "\n";
  y_file << "     yaw: " << yaw_radian * radian_to_degree_factor_ << "\n";
  y_file << "     roll: " << roll_radian * radian_to_degree_factor_ << "\n";
  // Option 2. Use YAML write function.
  // Alert! Couldn't find a library to save YAML node.
  // YAML::Node node = YAML::LoadFile(yaml_file);

  // try{
  //   if (node.IsNull()) {
  //     AINFO << "Load " << yaml_file << " failed! please check!";
  //     return false;
  //   }
  //   // Replace rotation only
  //   node["transform"]["rotation"]["x"].as<double>() = quarternion[0];
  //   node["transform"]["rotation"]["y"].as<double>() = quarternion[1];
  //   node["transform"]["rotation"]["z"].as<double>() = quarternion[2];
  //   node["transform"]["rotation"]["w"].as<double>() = quarternion[3];
  //
  //   node.SaveFile(yaml_file);
  //   if (node.IsNull()) {
  //     AINFO << "Save " << yaml_file << " failed! please check!";
  //     return false;
  //   }
  // } catch (YAML::InvalidNode &in) {
  //   AERROR << "load/save camera extrisic file " << yaml_file
  //          << " with error, YAML::InvalidNode exception";
  //   return false;
  // } catch (YAML::TypedBadConversion<double> &bc) {
  //   AERROR << "load camera extrisic file " << yaml_file
  //          << " with error, YAML::TypedBadConversion exception";
  //   return false;
  // } catch (YAML::Exception &e) {
  //   AERROR << "load camera extrisic file " << yaml_file
  //          << " with error, YAML exception:" << e.what();
  //   return false;
  // }

  return true;
}

bool Visualizer::save_manual_calibration_parameter(
    const std::string &camera_name, const double pitch_adj_degree,
    const double yaw_adj_degree, const double roll_adj_degree) {
  // Convert degree angles to radian angles
  double pitch_adj_radian = pitch_adj_degree * degree_to_radian_factor_;
  double yaw_adj_radian = yaw_adj_degree * degree_to_radian_factor_;
  double roll_adj_radian = roll_adj_degree * degree_to_radian_factor_;

  // Get current angle from extrinsics
  // ex_camera2lidar_ = extrinsic_map_.at(camera_name);
  Eigen::Matrix3d R = ex_camera2lidar_.block(0, 0, 3, 3);

  double old_pitch_radian = regularize_angle(atan2(R(2, 1), R(2, 2)));
  double old_roll_radian = regularize_angle(
      atan2(-R(2, 0), sqrt(R(2, 1) * R(2, 1) + R(2, 2) * R(2, 2))));
  double old_yaw_radian = regularize_angle(atan2(R(1, 0), R(0, 0)));
  AINFO << "Old pitch: " << old_pitch_radian * radian_to_degree_factor_;
  AINFO << "Old yaw: " << old_yaw_radian * radian_to_degree_factor_;
  AINFO << "Old roll: " << old_roll_radian * radian_to_degree_factor_;
  AINFO << "Adjusted pitch: " << pitch_adj_degree;
  AINFO << "Adjusted yaw: " << yaw_adj_degree;
  AINFO << "Adjusted roll: " << roll_adj_degree;

  // Convert value here since the coordinate system is different

  // Apply changed angles to each angle
  double new_pitch_radian =
      regularize_angle(old_pitch_radian + pitch_adj_radian);
  double new_yaw_radian = regularize_angle(old_yaw_radian - yaw_adj_radian);
  double new_roll_radian = regularize_angle(old_roll_radian + roll_adj_radian);

  AINFO << "New pitch: " << new_pitch_radian * radian_to_degree_factor_;
  AINFO << "New yaw: " << new_yaw_radian * radian_to_degree_factor_;
  AINFO << "New roll: " << new_roll_radian * radian_to_degree_factor_;

  Eigen::Vector4d quarternion;
  euler_to_quaternion(&quarternion, new_pitch_radian, new_roll_radian,
                      new_yaw_radian);
  AINFO << "Quarternion X: " << quarternion[0] << ", Y: " << quarternion[1]
        << ", Z: " << quarternion[2] << ", W: " << quarternion[3];
  // Save the file
  // Yaw and Roll are swapped.
  save_extrinsic_in_yaml(camera_name, ex_camera2lidar_, quarternion,
                         new_pitch_radian, new_yaw_radian, new_roll_radian);

  return true;
}

bool Visualizer::key_handler(const std::string &camera_name, const int key) {
  AINFO << "Pressed Key: " << key;
  if (key <= 0) {
    return false;
  }
  switch (key) {
    case 48:  // 0
      show_associate_color_ = !show_associate_color_;
      break;
    case 50:  // 2
      show_camera_box2d_ = !show_camera_box2d_;
      break;
    case 51:  // 3
      show_camera_box3d_ = !show_camera_box3d_;
      break;
    case 65: case 97:  // 'A' 'a'
      capture_video_ = !capture_video_;
      break;
    case 66: case 98:  // 'B' 'b'
      show_box_ = (show_box_ + 1) % 2;
      break;
    case 67: case 99:  // 'C' 'd'
      use_class_color_ = !use_class_color_;
      break;
    case 68: case 100:  // 'D' 'd'
      show_radar_pc_ = !show_radar_pc_;
      break;
    case 69: case 101:  // 'E' 'e'
      draw_lane_objects_ = !draw_lane_objects_;
      break;
    case 70: case 102:  // 'F' 'f'
      show_fusion_ = !show_fusion_;
      break;
    case 71: case 103:  // 'G' 'g'
      show_vp_grid_ = !show_vp_grid_;
      break;
    case 72: case 104:  // 'H' 'h'
      show_help_text_ = !show_help_text_;
      break;
    case 73: case 105:  // 'I' 'i'
      show_type_id_label_ = !show_type_id_label_;
      break;
    case 76: case 108:  // 'L' 'l'
      show_verbose_ = !show_verbose_;
      break;
    case 79: case 111:  // 'O' 'o'
      show_camera_bdv_ = !show_camera_bdv_;
      break;
    case 81: case 113:  // 'Q' 'q'
      show_lane_ = !show_lane_;
      break;
    case 82: case 114:  // 'R' 'r'
      reset_key();
      break;
    case 83: case 115:  // 'S' 's'
      capture_screen_ = true;
      break;
    case 84: case 116:  // 'T' 't'
      show_trajectory_ = !show_trajectory_;
      break;
    case 86: case 118:  // 'V' 'v'
      show_velocity_ = (show_velocity_ + 1) % 2;
      break;
    case 65362:  // Up_Arrow
      if (pitch_adj_degree_ + 0.05 <= max_pitch_degree_) {
        pitch_adj_degree_ -= 0.05;
      }
      AINFO << "Current pitch: " << pitch_adj_degree_;
      break;
    case 65364:  // Down_Arrow
      if (pitch_adj_degree_ - 0.05 >= min_pitch_degree_) {
        pitch_adj_degree_ += 0.05;
      }
      AINFO << "Current pitch: " << pitch_adj_degree_;
      break;
    case 65363:  // Right_Arrow
      if (yaw_adj_degree_ + 0.05 <= max_yaw_degree_) {
        yaw_adj_degree_ -= 0.05;
      }
      AINFO << "Current yaw: " << yaw_adj_degree_;
      break;
    case 65361:  // Left_Arrow
      if (yaw_adj_degree_ - 0.05 >= min_yaw_degree_) {
        yaw_adj_degree_ += 0.05;
      }
      AINFO << "Current yaw: " << yaw_adj_degree_;
      break;
    case 130899:  // SHIFT + Right_Arrow
      if (roll_adj_degree_ + 0.05 <= max_roll_degree_) {
        roll_adj_degree_ -= 0.05;
      }
      AINFO << "Current roll: " << roll_adj_degree_;
      break;
    case 130897:  // SHIFT + Left_Arrow
      if (roll_adj_degree_ - 0.05 >= min_roll_degree_) {
        roll_adj_degree_ += 0.05;
      }
      AINFO << "Current roll: " << roll_adj_degree_;
      break;
    case 262259:  // CTRL + S
      save_manual_calibration_parameter(camera_name, pitch_adj_degree_,
                                        yaw_adj_degree_, roll_adj_degree_);
      AINFO << "Saved calibration parameters(pyr): (" << pitch_adj_degree_
            << ", " << yaw_adj_degree_ << ", " << roll_adj_degree_ << ")";
      break;
    default:
      break;
  }

  help_str_ = "H: show help";
  if (show_help_text_) {
    help_str_ += " (ON)";
    help_str_ += "\nR: reset matrxi\nB: show box";
    if (show_box_) help_str_ += "(ON)";
    help_str_ += "\nV: show velocity";
    if (show_velocity_) help_str_ += " (ON)";
    help_str_ += "\nC: use class color";
    if (use_class_color_) help_str_ += " (ON)";
    help_str_ += "\nS: capture screen";
    help_str_ += "\nA: capture video";
    help_str_ += "\nI: show type id label";
    if (show_type_id_label_) help_str_ += " (ON)";
    help_str_ += "\nQ: show lane";
    if (show_lane_) help_str_ += " (ON)";
    help_str_ += "\nE: draw lane objects";
    if (draw_lane_objects_) help_str_ += " (ON)";
    help_str_ += "\nF: show fusion";
    if (show_fusion_) help_str_ += " (ON)";
    help_str_ += "\nD: show radar pc";
    if (show_radar_pc_) help_str_ += " (ON)";
    help_str_ += "\nT: show trajectory";
    if (show_trajectory_) help_str_ += " (ON)";
    help_str_ += "\nO: show camera bdv";
    if (show_camera_bdv_) help_str_ += " (ON)";
    help_str_ += "\n2: show camera box2d";
    if (show_camera_box2d_) help_str_ += " (ON)";
    help_str_ += "\n3: show camera box3d";
    if (show_camera_box3d_) help_str_ += " (ON)";
    help_str_ += "\n0: show associate color";
    if (show_associate_color_) help_str_ += " (ON)";
    help_str_ += "\nG: show vanishing point and ground plane grid";
    if (show_vp_grid_) help_str_ += " (ON)";
    help_str_ += "\nT: show verbose";
    if (show_verbose_) help_str_ += " (ON)";
  }
  switch (key) {
    case 65362:   // Up_Arrow
    case 65361:   // Left_Arrow
    case 65363:   // Right_Arrow
    case 65364:   // Down_Arrow
    case 130897:  // ALT + Left_Arrow
    case 130899:  // ALT + Right_Arrow
      adjust_angles(camera_name, pitch_adj_degree_, yaw_adj_degree_,
                    roll_adj_degree_);
      if (show_help_text_) {
        help_str_ += "\nAdjusted Pitch: " + std::to_string(pitch_adj_degree_);
        help_str_ += "\nAdjusted Yaw: " + std::to_string(yaw_adj_degree_);
        help_str_ += "\nAdjusted Roll: " + std::to_string(roll_adj_degree_);
      }
  }
  return true;
}

void Visualizer::Draw2Dand3D(const cv::Mat &img, const CameraFrame &frame) {
  cv::Mat image = img.clone();
  Eigen::Affine3d pose;
  if (!tf_server_->QueryPos(frame.timestamp, &pose)) {
    pose.setIdentity();
  }
  Eigen::Affine3d lidar2novatel;
  tf_server_->QueryTransform("velodyne128", "novatel", &lidar2novatel);
  Eigen::Affine3d lidar2world = pose * lidar2novatel;
  Eigen::Affine3d world2lidar = lidar2world.inverse();
  for (const auto &object : frame.tracked_objects) {
    base::RectF rect(object->camera_supplement.box);
    cv::Rect r(static_cast<int>(rect.x), static_cast<int>(rect.y),
               static_cast<int>(rect.width), static_cast<int>(rect.height));
    cv::Scalar color;
    if (object->b_cipv) {
      color = color_cipv_;
    } else {
      color = colorlistobj[object->track_id % colorlistobj.size()];
    }
    cv::rectangle(image, r, color, 2);
    cv::putText(image, std::to_string(object->track_id),
                cv::Point(static_cast<int>(rect.x), static_cast<int>(rect.y)),
                cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);
    Eigen::Vector3d theta;
    theta << cos(object->theta), sin(object->theta), 0;
    theta = world2lidar.linear() * theta;
    float yaw = static_cast<float>(atan2(theta[1], theta[0]));
    Eigen::Matrix2d rotate;
    rotate << cos(yaw), -sin(yaw), sin(yaw), cos(yaw);

    Eigen::Vector3d pos;
    pos << object->center[0], object->center[1], object->center[2];
    pos = world2lidar * pos;
    Eigen::Vector2d pos_2d;
    pos_2d << pos[0], pos[1];
    Eigen::Vector3d v;
    v << object->velocity[0], object->velocity[1], object->velocity[2];
    v = world2lidar.linear() * v;
    Eigen::Vector2d v_2d;
    v_2d << v[0] + pos_2d[0], v[1] + pos_2d[1];
    Eigen::Vector2d p1;
    p1 << object->size[0] * 0.5, object->size[1] * 0.5;
    p1 = rotate * p1 + pos_2d;
    Eigen::Vector2d p2;
    p2 << -object->size[0] * 0.5, object->size[1] * 0.5;
    p2 = rotate * p2 + pos_2d;
    Eigen::Vector2d p3;
    p3 << -object->size[0] * 0.5, -object->size[1] * 0.5;
    p3 = rotate * p3 + pos_2d;
    Eigen::Vector2d p4;
    p4 << object->size[0] * 0.5, -object->size[1] * 0.5;
    p4 = rotate * p4 + pos_2d;

    cv::line(world_image_, world_point_to_bigimg(p1), world_point_to_bigimg(p2),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(p2), world_point_to_bigimg(p3),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(p3), world_point_to_bigimg(p4),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(p4), world_point_to_bigimg(p1),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(pos_2d),
             world_point_to_bigimg(v_2d),
             color, 2);
  }
  last_timestamp_ = frame.timestamp;
  camera_image_[frame.data_provider->sensor_name()] = image;
  cv::resize(image, camera_image_[frame.data_provider->sensor_name()],
             cv::Size(small_w_, small_h_));
}

void Visualizer::ShowResult(const cv::Mat &img, const CameraFrame &frame) {
  cv::Mat image = img.clone();
  std::string camera_name = frame.data_provider->sensor_name();

  if (frame.timestamp - last_timestamp_ > 0.02) {
    cv::Mat bigimg(world_h_, small_w_ + wide_pixel_, CV_8UC3);
    camera_image_["front_6mm"].copyTo(
        bigimg(cv::Rect(0, 0, small_w_, small_h_)));
    camera_image_["front_12mm"].copyTo(
        bigimg(cv::Rect(0, small_h_, small_w_, small_h_)));
    world_image_.copyTo(bigimg(cv::Rect(small_w_, 0, wide_pixel_, world_h_)));
    if (write_out_img_) {
      char path[1000];
      snprintf(path, sizeof(path), "%s/%06d.jpg", path_.c_str(),
               frame.frame_id);
      AINFO << "A snapshot of visualizer saved at " << path;
      cv::imwrite(path, bigimg);
    }

    if (cv_imshow_img_) {
      cv::imshow("", bigimg);
      int key = cvWaitKey(30);
      key_handler(camera_name, key);
    }
    world_image_ = cv::Mat(world_h_, wide_pixel_, CV_8UC3, cv::Scalar(0, 0, 0));
    draw_range_circle();
  }

  cv::putText(image, camera_name, cv::Point(10, 50), cv::FONT_HERSHEY_DUPLEX,
              1.3, cv::Scalar(0, 0, 255), 3);
  cv::putText(image, "frame #: " + std::to_string(frame.frame_id),
              cv::Point(10, 100), cv::FONT_HERSHEY_DUPLEX, 1.3,
              cv::Scalar(0, 0, 255), 3);
  Draw2Dand3D(image, frame);
}

void Visualizer::Draw2Dand3D_all_info_single_camera(const cv::Mat &img,
                                                    const CameraFrame &frame,
                                                    Eigen::Matrix3d intrinsic,
                                                    Eigen::Matrix4d extrinsic) {
  cv::Mat image_2D = img.clone();  // All clone should be replaced with global
  cv::Mat image_3D = img.clone();  // variable and allocated at Init..

  // plot FOV

  // cv::line(img2, p_fov_1_, p_fov_2_, cv::Scalar(255, 255, 255), 2);
  // cv::line(img2, p_fov_1_, p_fov_3_, cv::Scalar(255, 255, 255), 2);
  // cv::line(img2, p_fov_2_, p_fov_4_, cv::Scalar(255, 255, 255), 2);
  // cv::line(world_image_, world_point_to_bigimg(image2ground(p_fov_1_)),
  //          world_point_to_bigimg(image2ground(p_fov_2_)),
  //          cv::Scalar(255, 255, 255), 2);
  // cv::line(world_image_, world_point_to_bigimg(image2ground(p_fov_1_)),
  //          world_point_to_bigimg(image2ground(p_fov_3_)),
  //          cv::Scalar(255, 255, 255), 2);
  // cv::line(world_image_, world_point_to_bigimg(image2ground(p_fov_2_)),
  //          world_point_to_bigimg(image2ground(p_fov_4_)),
  //          cv::Scalar(255, 255, 255), 2);

  // cv::line(img2, p_fov_2_, p_fov_4_, cv::Scalar(255, 255, 255), 2);
  // cv::line(world_image_, world_point_to_bigimg(image2ground(p_fov_1_)),
  //          world_point_to_bigimg(image2ground(p_fov_2_)),
  //          cv::Scalar(255, 255, 255), 2);
  // cv::line(world_image_, world_point_to_bigimg(image2ground(p_fov_1_)),

  if (show_vp_grid_) {
    cv::line(image_2D, ground2image(vp1_), ground2image(vp2_),
             cv::Scalar(255, 255, 255), 2);
  }
  AINFO << "vp1_: " << vp1_ << ", vp1_image: " << ground2image(vp1_);
  AINFO << "vp2_: " << vp2_ << ", vp2_image: " << ground2image(vp2_);

  // AINFO << "FOV point 1: " << image2ground(p_fov_1_);
  // AINFO << "FOV point 2: " << image2ground(p_fov_2_);
  // AINFO << "FOV point 3: " << image2ground(p_fov_3_);
  // AINFO << "FOV point 4: " << image2ground(p_fov_4_);

  // plot laneline on image and ground plane
  for (const auto &object : frame.lane_objects) {
    cv::Scalar lane_color = colormapline[object.pos_type];
    cv::Point p_prev;
    p_prev.x = static_cast<int>(object.curve_image_point_set[0].x);
    p_prev.y = static_cast<int>(object.curve_image_point_set[0].y);
    Eigen::Vector2d p_prev_ground = image2ground(p_prev);

    for (unsigned i = 1; i < object.curve_image_point_set.size(); i++) {
      cv::Point p_cur;
      p_cur.x = static_cast<int>(object.curve_image_point_set[i].x);
      p_cur.y = static_cast<int>(object.curve_image_point_set[i].y);
      Eigen::Vector2d p_cur_ground = image2ground(p_cur);

      cv::line(image_3D, p_prev, p_cur, lane_color, 2);
      cv::line(world_image_, world_point_to_bigimg(p_prev_ground),
               world_point_to_bigimg(p_cur_ground), lane_color, 2);
      p_prev = p_cur;
      p_prev_ground = p_cur_ground;
    }
  }

  AINFO << "Drew lane line";

  for (const auto &object : frame.tracked_objects) {
    // plot 2D box on image_2D
    base::RectF rect(object->camera_supplement.box);
    cv::Rect r(static_cast<int>(rect.x), static_cast<int>(rect.y),
               static_cast<int>(rect.width), static_cast<int>(rect.height));
    cv::Scalar color;
    if (object->b_cipv) {
      color = color_cipv_;
    } else {
      color = colorlistobj[object->track_id % colorlistobj.size()];
    }
    cv::rectangle(image_2D, r, color, 2);
    cv::putText(image_2D, std::to_string(object->track_id),
                cv::Point(static_cast<int>(rect.x), static_cast<int>(rect.y)),
                cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);

    cv::putText(
        image_2D,
        // type_to_string(object->type) + "->" +
        sub_type_to_string(object->sub_type),
        cv::Point(static_cast<int>(rect.x), static_cast<int>(rect.y) + 30),
        cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(255, 0, 0), 1);

    // compute 8 vetices in camera coodinates
    Eigen::Vector3d pos;
    pos << object->camera_supplement.local_center[0],
        object->camera_supplement.local_center[1],
        object->camera_supplement.local_center[2];
    double theta_ray = atan2(pos[0], pos[2]);
    double theta = object->camera_supplement.alpha + theta_ray;

    Eigen::Matrix3d rotate_ry;
    rotate_ry << cos(theta), 0, sin(theta), 0, 1, 0, -sin(theta), 0, cos(theta);
    std::vector<Eigen::Vector3d> p(8);
    p[0] << object->size[0] * 0.5, object->size[2] * 0.5, object->size[1] * 0.5;
    p[1] << -object->size[0] * 0.5, object->size[2] * 0.5,
        object->size[1] * 0.5;
    p[2] << -object->size[0] * 0.5, object->size[2] * 0.5,
        -object->size[1] * 0.5;
    p[3] << object->size[0] * 0.5, object->size[2] * 0.5,
        -object->size[1] * 0.5;
    p[4] << object->size[0] * 0.5, -object->size[2] * 0.5,
        object->size[1] * 0.5;
    p[5] << -object->size[0] * 0.5, -object->size[2] * 0.5,
        object->size[1] * 0.5;
    p[6] << -object->size[0] * 0.5, -object->size[2] * 0.5,
        -object->size[1] * 0.5;
    p[7] << object->size[0] * 0.5, -object->size[2] * 0.5,
        -object->size[1] * 0.5;
    for (uint i = 0; i < p.size(); i++) p[i] = rotate_ry * p[i] + pos;

    // compute 4 bottom vetices in lidar coordinate
    // Eigen::Vector3d p1_l_3d = extrinsic.block(0,0,3,3) * p1 +
    //                           extrinsic.block(0,3,3,1);
    // Eigen::Vector3d p2_l_3d = extrinsic.block(0,0,3,3) * p2 +
    //                           extrinsic.block(0,3,3,1);
    // Eigen::Vector3d p3_l_3d = extrinsic.block(0,0,3,3) * p3 +
    //                           extrinsic.block(0,3,3,1);
    // Eigen::Vector3d p4_l_3d = extrinsic.block(0,0,3,3) * p4 +
    //                           extrinsic.block(0,3,3,1);

    // Eigen::Vector2d p1_l = p1_l_3d.block(0, 0, 2, 1);
    // Eigen::Vector2d p2_l = p2_l_3d.block(0, 0, 2, 1);
    // Eigen::Vector2d p3_l = p3_l_3d.block(0, 0, 2, 1);
    // Eigen::Vector2d p4_l = p4_l_3d.block(0, 0, 2, 1);

    // compute obstacle center in lidar ground
    cv::Point c_2D;
    c_2D.x = static_cast<int>(rect.x + rect.width / 2);
    c_2D.y = static_cast<int>(rect.y + rect.height);
    Eigen::Vector2d c_2D_l = image2ground(c_2D);
    Eigen::Matrix2d rotate_rz;
    theta = theta - M_PI_2;
    rotate_rz << cos(theta), sin(theta), -sin(theta), cos(theta);
    // plot obstacles on ground plane in lidar coordinates
    Eigen::Vector2d p1_l;
    p1_l << object->size[0] * 0.5, object->size[1] * 0.5;
    p1_l = rotate_rz * p1_l + c_2D_l;
    Eigen::Vector2d p2_l;
    p2_l << -object->size[0] * 0.5, object->size[1] * 0.5;
    p2_l = rotate_rz * p2_l + c_2D_l;
    Eigen::Vector2d p3_l;
    p3_l << -object->size[0] * 0.5, -object->size[1] * 0.5;
    p3_l = rotate_rz * p3_l + c_2D_l;
    Eigen::Vector2d p4_l;
    p4_l << object->size[0] * 0.5, -object->size[1] * 0.5;
    p4_l = rotate_rz * p4_l + c_2D_l;
    cv::line(world_image_, world_point_to_bigimg(p1_l),
             world_point_to_bigimg(p2_l),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(p2_l),
             world_point_to_bigimg(p3_l),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(p3_l),
             world_point_to_bigimg(p4_l),
             color, 2);
    cv::line(world_image_, world_point_to_bigimg(p4_l),
             world_point_to_bigimg(p1_l),
             color, 2);

    // plot projected 3D box on image_3D
    for (uint i = 0; i < p.size(); i++) p[i] = intrinsic * p[i];

    std::vector<cv::Point> p_proj(8);
    for (uint i = 0; i < p_proj.size(); i++) {
      p_proj[i].x = static_cast<int>(p[i][0] / p[i][2]);
      p_proj[i].y = static_cast<int>(p[i][1] / p[i][2]);
    }

    cv::line(image_3D, p_proj[0], p_proj[1],
             color, 2);
    cv::line(image_3D, p_proj[1], p_proj[2],
             color, 2);
    cv::line(image_3D, p_proj[2], p_proj[3],
             color, 2);
    cv::line(image_3D, p_proj[3], p_proj[0],
             color, 2);
    cv::line(image_3D, p_proj[4], p_proj[5],
             color, 2);
    cv::line(image_3D, p_proj[5], p_proj[6],
             color, 2);
    cv::line(image_3D, p_proj[6], p_proj[7],
             color, 2);
    cv::line(image_3D, p_proj[7], p_proj[4],
             color, 2);
    cv::line(image_3D, p_proj[0], p_proj[4],
             color, 2);
    cv::line(image_3D, p_proj[1], p_proj[5],
             color, 2);
    cv::line(image_3D, p_proj[2], p_proj[6],
             color, 2);
    cv::line(image_3D, p_proj[3], p_proj[7],
             color, 2);
  }

  AINFO << "Drew object";

  last_timestamp_ = frame.timestamp;
  camera_image_[frame.data_provider->sensor_name() + "_2D"] = image_2D;
  cv::resize(image_2D,
             camera_image_[frame.data_provider->sensor_name() + "_2D"],
             cv::Size(small_w_, small_h_));
  camera_image_[frame.data_provider->sensor_name() + "_3D"] = image_3D;
  cv::resize(image_3D,
             camera_image_[frame.data_provider->sensor_name() + "_3D"],
             cv::Size(small_w_, small_h_));

  AINFO << "Finished copy";
}

void Visualizer::ShowResult_all_info_single_camera(const cv::Mat &img,
    const CameraFrame &frame,
    const base::MotionBufferPtr motion_buffer) {
  if (frame.timestamp - last_timestamp_ < 0.02) return;

  // draw results on visulization panel
  int line_pos = 0;
  cv::Mat image = img.clone();
  std::string camera_name = frame.data_provider->sensor_name();
  line_pos += 50;
  cv::putText(image, camera_name, cv::Point(10, line_pos),
              cv::FONT_HERSHEY_DUPLEX, 1.3, cv::Scalar(0, 0, 255), 3);
  line_pos += 50;
  cv::putText(image, "frame id: " + std::to_string(frame.frame_id),
              cv::Point(10, line_pos), cv::FONT_HERSHEY_DUPLEX, 1.3,
              cv::Scalar(0, 0, 255), 3);
  line_pos += 50;
  cv::putText(image,
              "yaw rate: " + std::to_string(motion_buffer->back().yaw_rate),
              cv::Point(10, line_pos), cv::FONT_HERSHEY_DUPLEX, 1.3,
              cv::Scalar(0, 0, 255), 3);
  line_pos += 50;
  cv::putText(image,
             "pitch rate: " + std::to_string(motion_buffer->back().pitch_rate),
              cv::Point(10, line_pos), cv::FONT_HERSHEY_DUPLEX, 1.3,
              cv::Scalar(0, 0, 255), 3);
  line_pos += 50;
  cv::putText(image,
              "roll rate: " + std::to_string(motion_buffer->back().roll_rate),
              cv::Point(10, line_pos), cv::FONT_HERSHEY_DUPLEX, 1.3,
              cv::Scalar(0, 0, 255), 3);
  line_pos += 50;
  cv::putText(image,
              "velocity: " + std::to_string(motion_buffer->back().velocity),
              cv::Point(10, line_pos), cv::FONT_HERSHEY_DUPLEX, 1.3,
              cv::Scalar(0, 0, 255), 3);

  for (const auto &object : frame.tracked_objects) {
    if (object->b_cipv) {
      line_pos += 50;
      cv::putText(image,
                  "CIPV: " + std::to_string(object->track_id),
                  cv::Point(10, line_pos), cv::FONT_HERSHEY_DUPLEX, 1.3,
                  cv::Scalar(0, 0, 255), 3);
    }
  }

  if (intrinsic_map_.find(camera_name) != intrinsic_map_.end() &&
      extrinsic_map_.find(camera_name) != extrinsic_map_.end()) {
    Draw2Dand3D_all_info_single_camera(
        image, frame, intrinsic_map_.at(camera_name).cast<double>(),
        extrinsic_map_.at(camera_name));
  } else {
    AERROR << "Failed to find necessuary intrinsic or extrinsic params.";
  }

  // copy visual results into visualization panel
  cv::Mat bigimg(world_h_, small_w_ + wide_pixel_, CV_8UC3);
  camera_image_[camera_name + "_2D"].copyTo(
      bigimg(cv::Rect(0, 0, small_w_, small_h_)));
  camera_image_[camera_name + "_3D"].copyTo(
      bigimg(cv::Rect(0, small_h_, small_w_, small_h_)));
  world_image_.copyTo(bigimg(cv::Rect(small_w_, 0, wide_pixel_, world_h_)));

  // output visualization panel
  if (write_out_img_) {
    char path[1000];
    snprintf(path, sizeof(path), "%s/%06d.jpg", path_.c_str(), frame.frame_id);
    AINFO << path;
    cv::imwrite(path, bigimg);
  }

  if (cv_imshow_img_) {
    cv::imshow("", bigimg);
    int key = cvWaitKey(30);
    key_handler(camera_name, key);
  }

  // re-initialize empty world_image_
  world_image_ = cv::Mat(world_h_, wide_pixel_, CV_8UC3, cv::Scalar(0, 0, 0));
  draw_range_circle();
  AINFO << "Finished ShowResult_all_info_single_camera";
}

void Visualizer::draw_range_circle() {
  cv::Scalar color(255, 100, 0);
  cv::circle(world_image_, cv::Point(wide_pixel_ / 2, world_h_), 1 * m2pixel_,
             color, 1);
  for (int i = 20; i < 300; i += 20) {
    cv::circle(world_image_, cv::Point(wide_pixel_ / 2, world_h_), i * m2pixel_,
               color, 2);
  }
  for (int i = 50; i < 300; i += 50) {
    cv::putText(world_image_, std::to_string(i),
                cv::Point(wide_pixel_ / 2, world_h_ - i * m2pixel_),
                cv::FONT_HERSHEY_DUPLEX, 1, cv::Scalar(0, 0, 255), 2);
  }
}

cv::Point Visualizer::world_point_to_bigimg(const Eigen::Vector2d &p) {
  cv::Point point;
  point.x = static_cast<int>(-p[1] * m2pixel_ + wide_pixel_ * 0.5);
  point.y = static_cast<int>(world_h_ - p[0] * m2pixel_);
  return point;
}

Eigen::Vector2d Visualizer::image2ground(cv::Point p_img) {
  Eigen::Vector3d p_homo;

  p_homo << p_img.x, p_img.y, 1;
  Eigen::Vector3d p_ground;
  p_ground = homography_image2ground_ * p_homo;
  if (fabs(p_ground[2]) > std::numeric_limits<double>::min()) {
    p_ground[0] = p_ground[0] / p_ground[2];
    p_ground[1] = p_ground[1] / p_ground[2];
  } else {
    AINFO << "p_ground[2] too small :" << p_ground[2];
  }
  return p_ground.block(0, 0, 2, 1);
}
cv::Point Visualizer::ground2image(Eigen::Vector2d p_ground) {
  Eigen::Vector3d p_homo;

  p_homo << p_ground[0], p_ground[1], 1;
  Eigen::Vector3d p_img;
  p_img = homography_ground2image_ * p_homo;
  if (fabs(p_img[2]) > std::numeric_limits<double>::min()) {
    p_img[0] = p_img[0] / p_img[2];
    p_img[1] = p_img[1] / p_img[2];
  }
  return cv::Point(static_cast<int>(p_img[0]), static_cast<int>(p_img[1]));
}

}  // namespace camera
}  // namespace perception
}  // namespace apollo
