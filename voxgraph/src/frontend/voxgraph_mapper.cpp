#include "voxgraph/frontend/voxgraph_mapper.h"
#include <geometry_msgs/PoseArray.h>
#include <minkindr_conversions/kindr_msg.h>
#include <minkindr_conversions/kindr_xml.h>
#include <nav_msgs/Path.h>
#include <sensor_msgs/Imu.h>
#include <visualization_msgs/MarkerArray.h>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include "voxgraph/frontend/submap_collection/submap_timeline.h"
#include "voxgraph/tools/io.h"
#include "voxgraph/tools/ros_params.h"
#include "voxgraph/tools/submap_registration_helper.h"
#include "voxgraph/tools/tf_helper.h"

namespace voxgraph {

VoxgraphMapper::VoxgraphMapper(const ros::NodeHandle &nh,
                               const ros::NodeHandle &nh_private)
    : VoxgraphMapper(nh, nh_private,
                     getVoxgraphSubmapConfigFromRosParams(nh_private)) {}

VoxgraphMapper::VoxgraphMapper(const ros::NodeHandle &nh,
                               const ros::NodeHandle &nh_private,
                               VoxgraphSubmap::Config submap_config)
    : nh_(nh),
      nh_private_(nh_private),
      verbose_(false),
      auto_pause_rosbag_(false),
      rosbag_helper_(nh),
      pointcloud_topic_("pointcloud"),
      subscriber_queue_length_(100),
      loop_closure_topic_("loop_closure_input"),
      loop_closure_subscriber_queue_length_(1000),
      registration_constraints_enabled_(false),
      odometry_constraints_enabled_(false),
      height_constraints_enabled_(false),
      submap_config_(submap_config),
      submap_collection_ptr_(
          std::make_shared<VoxgraphSubmapCollection>(submap_config_)),
      submap_vis_(submap_config_),
      pose_graph_interface_(nh_private, submap_collection_ptr_),
      projected_map_server_(nh_private),
      submap_server_(nh_private),
      loop_closure_edge_server_(nh_private),
      use_icp_refinement_(false),
      map_tracker_(submap_collection_ptr_,
                   FrameNames::fromRosParams(nh_private), verbose_) {
  // Setup interaction with ROS
  getParametersFromRos();
  subscribeToTopics();
  advertiseTopics();
  advertiseServices();
}

void VoxgraphMapper::getParametersFromRos() {
  nh_private_.param("verbose", verbose_, verbose_);
  pose_graph_interface_.setVerbosity(verbose_);
  map_tracker_.setVerbosity(verbose_);

  nh_private_.param("subscriber_queue_length", subscriber_queue_length_,
                    subscriber_queue_length_);
  nh_private_.param("pointcloud_topic", pointcloud_topic_, pointcloud_topic_);
  nh_private_.param("loop_closure_topic", loop_closure_topic_,
                    loop_closure_topic_);

  // Get the submap creation interval as a ros::Duration
  double interval_temp;
  if (nh_private_.getParam("submap_creation_interval", interval_temp)) {
    submap_collection_ptr_->setSubmapCreationInterval(
        ros::Duration(interval_temp));
  }

  // Get the mesh visualization interval
  double update_mesh_every_n_sec = 0.0;
  nh_private_.param("update_mesh_every_n_sec", update_mesh_every_n_sec,
      update_mesh_every_n_sec);
  if (update_mesh_every_n_sec > 0.0) {
    update_mesh_timer_ =
        nh_private_.createTimer(ros::Duration(update_mesh_every_n_sec),
            &VoxgraphMapper::publishActiveMeshCallback, this);
  }

  // Read whether or not to auto pause the rosbag during graph optimization
  nh_private_.param("auto_pause_rosbag", auto_pause_rosbag_,
                    auto_pause_rosbag_);

  //  // Load the transform from the base frame to the sensor frame, if
  //  available
  //  // NOTE: If the transform is not specified through ROS params, voxgraph
  //  will
  //  //       attempt to get if from TFs
  //  XmlRpc::XmlRpcValue T_base_sensor_xml;
  //  get_sensor_calibration_from_tfs_ =
  //      !nh_private_.getParam("T_base_sensor", T_base_sensor_xml);
  //  if (!get_sensor_calibration_from_tfs_) {
  //    kindr::minimal::xmlRpcToKindr(T_base_sensor_xml, &T_B_C_);
  //  }
  //  ROS_INFO_STREAM(
  //      "Using transform from pointcloud sensor to robot base "
  //      "link from "
  //      << (get_sensor_calibration_from_tfs_ ? "TFs" : "ROS params"));

  // Read the measurement params from their sub-namespace
  ros::NodeHandle nh_measurement_params(nh_private_, "measurements");
  nh_measurement_params.param("submap_registration/enabled",
                              registration_constraints_enabled_,
                              registration_constraints_enabled_);
  ROS_INFO_STREAM_COND(
      verbose_,
      "Submap registration constraints: "
          << (registration_constraints_enabled_ ? "enabled" : "disabled"));
  nh_measurement_params.param("odometry/enabled", odometry_constraints_enabled_,
                              odometry_constraints_enabled_);
  ROS_INFO_STREAM_COND(
      verbose_,
      "Odometry constraints: " << (odometry_constraints_enabled_ ? "enabled"
                                                                 : "disabled"));
  nh_measurement_params.param("height/enabled", height_constraints_enabled_,
                              height_constraints_enabled_);
  ROS_INFO_STREAM_COND(
      verbose_, "Height constraints: "
                    << (height_constraints_enabled_ ? "enabled" : "disabled"));
  pose_graph_interface_.setMeasurementConfigFromRosParams(
      nh_measurement_params);

  // Enable or disable voxgraph ICP
  nh_private_.param("use_icp_refinement", use_icp_refinement_,
                    use_icp_refinement_);
  ROS_INFO_STREAM_COND(verbose_, "ICP refinement: "
                       << (use_icp_refinement_ ? "enabled" : "disabled"));
  // TODO(victorr): Remove this once ICP refinement is compatible with the new
  //                frame convention
  CHECK(!use_icp_refinement_) << "ICP refinement is temporarely unavailable as "
                                 "it is not yet compatible "
                                 "with the new coordinate frame convention.";

  // Read TSDF integrator params from their sub-namespace
  ros::NodeHandle nh_tsdf_params(nh_private_, "tsdf_integrator");
  pointcloud_integrator_.setTsdfIntegratorConfigFromRosParam(nh_tsdf_params);
}

void VoxgraphMapper::subscribeToTopics() {
  pointcloud_subscriber_ =
      nh_.subscribe(pointcloud_topic_, subscriber_queue_length_,
                    &VoxgraphMapper::pointcloudCallback, this);
  loop_closure_subscriber_ =
      nh_.subscribe(loop_closure_topic_, loop_closure_subscriber_queue_length_,
                    &VoxgraphMapper::loopClosureCallback, this);
  map_tracker_.subscribeToTopics(
      nh_, nh_private_.param<std::string>("odometry_input_topic", ""),
      nh_private_.param<std::string>("imu_biases_topic", ""));
}

void VoxgraphMapper::advertiseTopics() {
  separated_mesh_pub_ = nh_private_.advertise<visualization_msgs::Marker>(
      "separated_mesh", subscriber_queue_length_, true);
  active_mesh_pub_ = nh_private_.advertise<visualization_msgs::Marker>(
      "active_mesh", subscriber_queue_length_, true);
  combined_mesh_pub_ = nh_private_.advertise<visualization_msgs::Marker>(
      "combined_mesh", subscriber_queue_length_, true);
  pose_history_pub_ =
      nh_private_.advertise<nav_msgs::Path>("pose_history", 1, true);
  loop_closure_links_pub_ =
      nh_private_.advertise<visualization_msgs::MarkerArray>(
          "loop_closure_links_vis", subscriber_queue_length_, true);
  loop_closure_axes_pub_ = nh_private_.advertise<geometry_msgs::PoseArray>(
      "loop_closure_axes_vis", subscriber_queue_length_, true);
  map_tracker_.advertiseTopics(
      nh_private_,
      nh_private_.param<std::string>("odometry_output_topic", "odometry"));
}

void VoxgraphMapper::advertiseServices() {
  publish_separated_mesh_srv_ = nh_private_.advertiseService(
      "publish_separated_mesh", &VoxgraphMapper::publishSeparatedMeshCallback,
      this);
  publish_combined_mesh_srv_ = nh_private_.advertiseService(
      "publish_combined_mesh", &VoxgraphMapper::publishCombinedMeshCallback,
      this);
  optimize_graph_srv_ = nh_private_.advertiseService(
      "optimize_pose_graph", &VoxgraphMapper::optimizeGraphCallback, this);
  finish_map_srv_ = nh_private_.advertiseService(
      "finish_map", &VoxgraphMapper::finishMapCallback, this);
  save_to_file_srv_ = nh_private_.advertiseService(
      "save_to_file", &VoxgraphMapper::saveToFileCallback, this);
  save_pose_history_to_file_srv_ = nh_private_.advertiseService(
      "save_pose_history_to_file",
      &VoxgraphMapper::savePoseHistoryToFileCallback, this);
  save_separated_mesh_srv_ = nh_private_.advertiseService(
      "save_separated_mesh", &VoxgraphMapper::saveSeparatedMeshCallback, this);
  save_combined_mesh_srv_ = nh_private_.advertiseService(
      "save_combined_mesh", &VoxgraphMapper::saveCombinedMeshCallback, this);

  // Service for submaps
  publish_active_submap_srv_ = nh_private_.advertiseService(
      "publish_active_submap",
      &VoxgraphMapper::publishActiveSubmapCallback, this);
}

void VoxgraphMapper::pointcloudCallback(
    const sensor_msgs::PointCloud2::Ptr &pointcloud_msg) {
  // Lookup the robot pose at the time of the pointcloud message
  ros::Time current_timestamp = pointcloud_msg->header.stamp;
  if (!map_tracker_.updateToTime(current_timestamp,
                                 pointcloud_msg->header.frame_id)) {
    // If the pose cannot be found, the pointcloud is skipped
    ROS_WARN_STREAM("Skipping pointcloud since the poses at time "
                    << current_timestamp << " could not be looked up.");
    return;
  }

  // Check if it's time to create a new submap
  if (submap_collection_ptr_->shouldCreateNewSubmap(current_timestamp)) {
    // Automatically pause the rosbag if requested
    if (auto_pause_rosbag_) rosbag_helper_.pauseRosbag();

    // Add the finished submap to the pose graph
    switchToNewSubmap(current_timestamp);

    // Optimize the pose graph in a separate thread
    if (optimization_async_handle_.valid()) {
      // Wait for the previous optimization to finish before starting a new one
      optimization_async_handle_.wait();
    }
    optimization_async_handle_ = std::async(
        std::launch::async, &VoxgraphMapper::optimizePoseGraph, this);

    // Publish the map in its different representations
    publishMaps(current_timestamp);

    // Resume playing the rosbag
    if (auto_pause_rosbag_) rosbag_helper_.playRosbag();
  }

  // Refine the camera pose using scan to submap matching
  if (use_icp_refinement_) {
    map_tracker_.registerPointcloud(pointcloud_msg);
  }

  // Integrate the pointcloud
  pointcloud_integrator_.integratePointcloud(
      pointcloud_msg, map_tracker_.get_T_S_C(),
      submap_collection_ptr_->getActiveSubmapPtr().get());

  // Add the current pose to the submap's pose history
  submap_collection_ptr_->getActiveSubmapPtr()->addPoseToHistory(
      current_timestamp, map_tracker_.get_T_S_B());

  // Publish the odometry
  map_tracker_.publishOdometry();

  // Publish the TF frames
  map_tracker_.publishTFs();

  // Publish the pose history
  if (pose_history_pub_.getNumSubscribers() > 0) {
    submap_vis_.publishPoseHistory(*submap_collection_ptr_,
                                   map_tracker_.getFrameNames().mission_frame,
                                   pose_history_pub_);
  }
}

void VoxgraphMapper::loopClosureCallback(
    const voxgraph_msgs::LoopClosure &loop_closure_msg) {
  // TODO(victorr): Introduce flag to switch between default or msg info. matrix
  // TODO(victorr): Move the code below to a measurement processor
  // Setup warning msg prefix
  const ros::Time &timestamp_A = loop_closure_msg.from_timestamp;
  const ros::Time &timestamp_B = loop_closure_msg.to_timestamp;
  std::ostringstream warning_msg_prefix;
  warning_msg_prefix << "Could not add loop closure from timestamp "
                     << timestamp_A << " to " << timestamp_B;

  // Find the submaps that were active at both timestamps
  SubmapID submap_id_A, submap_id_B;
  submap_collection_ptr_->lookupActiveSubmapByTime(
      loop_closure_msg.from_timestamp, &submap_id_A);
  submap_collection_ptr_->lookupActiveSubmapByTime(
      loop_closure_msg.to_timestamp, &submap_id_B);
  if (!submap_id_A || !submap_id_B) {
    ROS_WARN_STREAM(warning_msg_prefix.str() << ": timestamp A or B has no "
                                                "corresponding submap");
    return;
  }
  if (submap_id_A == submap_id_B) {
    ROS_WARN_STREAM(warning_msg_prefix.str() << ": timestamp A and B fall "
                                                "within the same submap");
    return;
  }
  const VoxgraphSubmap &submap_A =
      submap_collection_ptr_->getSubmap(submap_id_A);
  const VoxgraphSubmap &submap_B =
      submap_collection_ptr_->getSubmap(submap_id_B);

  // Find the robot pose at both timestamp in their active submap frame
  Transformation T_A_t1, T_B_t2;
  if (!submap_A.lookupPoseByTime(timestamp_A, &T_A_t1) ||
      !submap_B.lookupPoseByTime(timestamp_B, &T_B_t2)) {
    ROS_WARN_STREAM(warning_msg_prefix.str() << ": timestamp A or B has no "
                                                "corresponding robot pose");
    return;
  }

  // Convert the transform between two timestamps into a transform between
  // two submap origins
  // NOTE: The usual conversion method, tf::transformMsgToKindr(), is not used
  //       since it has a hard CHECK on invalid rotations which cannot be caught
  Eigen::Vector3d translation;
  tf::vectorMsgToEigen(loop_closure_msg.transform.translation, translation);
  Eigen::Quaterniond rotation;
  tf::quaternionMsgToEigen(loop_closure_msg.transform.rotation, rotation);
  if (std::abs(rotation.squaredNorm() - 1.0) > 1e-3) {
    ROS_WARN_STREAM(warning_msg_prefix.str() << ": supplied transform "
                                                "quaternion is invalid");
    return;
  }
  Transformation T_t1_t2(translation.cast<voxblox::FloatingPoint>(),
                         rotation.cast<voxblox::FloatingPoint>());
  Transformation T_AB = T_A_t1 * T_t1_t2 * T_B_t2.inverse();
  pose_graph_interface_.addLoopClosureMeasurement(submap_id_A, submap_id_B,
                                                  T_AB);

  // Visualize the loop closure link
  const Transformation &T_M_A = submap_A.getPose();
  const Transformation &T_M_B = submap_B.getPose();
  const Transformation T_M_t1 = T_M_A * T_A_t1;
  const Transformation T_M_t2 = T_M_B * T_B_t2;
  loop_closure_vis_.publishLoopClosure(
      T_M_t1, T_M_t2, T_t1_t2, map_tracker_.getFrameNames().mission_frame,
      loop_closure_links_pub_);
  loop_closure_vis_.publishAxes(T_M_t1, T_M_t2, T_t1_t2,
                                map_tracker_.getFrameNames().mission_frame,
                                loop_closure_axes_pub_);
}

void VoxgraphMapper::publishActiveMeshCallback(const ros::TimerEvent& /*event*/) {
  if (!submap_collection_ptr_->exists(submap_collection_ptr_->getActiveSubmapID())) {
    return;
  }
  // publish submap transform
  map_tracker_.publishTFs();

  // publish active mesh
  if (active_mesh_pub_.getNumSubscribers() > 0) {
    submap_vis_.publishMesh(*submap_collection_ptr_,
        submap_collection_ptr_->getActiveSubmapID(),
//        voxblox::rainbowColorMap(1.0),
        voxblox::Color::Gray(),
        map_tracker_.getFrameNames().active_submap_frame, active_mesh_pub_);
  }
}

bool VoxgraphMapper::publishSeparatedMeshCallback(
    std_srvs::Empty::Request &request, std_srvs::Empty::Response &response) {
  submap_vis_.publishSeparatedMesh(*submap_collection_ptr_,
                                   map_tracker_.getFrameNames().mission_frame,
                                   separated_mesh_pub_);
  return true;  // Tell ROS it succeeded
}

bool VoxgraphMapper::publishCombinedMeshCallback(
    std_srvs::Empty::Request &request, std_srvs::Empty::Response &response) {
  submap_vis_.publishCombinedMesh(*submap_collection_ptr_,
                                  map_tracker_.getFrameNames().mission_frame,
                                  combined_mesh_pub_);
  return true;  // Tell ROS it succeeded
}

bool VoxgraphMapper::finishMapCallback(std_srvs::Empty::Request &request,
                                       std_srvs::Empty::Response &response) {
  // Indicate that the previous submap is finished
  // s.t. its cached members are generated
  if (!submap_collection_ptr_->empty()) {
    ROS_INFO("Finishing the last submap");
    submap_collection_ptr_->getActiveSubmapPtr()->finishSubmap();
  }

  // Add registration constraints for all overlapping submaps
  if (registration_constraints_enabled_) {
    ROS_INFO(
        "Updating the registration constraints "
        "to include the last submap");
    pose_graph_interface_.updateRegistrationConstraints();
  }

  // Optimize the pose graph
  optimizePoseGraph();

  // Publishing the finished submap
  // todo: fix double publishing
  submap_server_.publishActiveSubmap(submap_collection_ptr_, ros::Time::now());
  publishMaps(ros::Time::now());

  ROS_INFO("The map is now finished and ready to be saved");
  return true;
}

bool VoxgraphMapper::optimizeGraphCallback(
    std_srvs::Empty::Request &request, std_srvs::Empty::Response &response) {
  optimizePoseGraph();
  return true;
}

bool VoxgraphMapper::saveToFileCallback(
    voxblox_msgs::FilePath::Request &request,
    voxblox_msgs::FilePath::Response &response) {
  submap_collection_ptr_->saveToFile(request.file_path);
  return true;
}

bool VoxgraphMapper::savePoseHistoryToFileCallback(
    voxblox_msgs::FilePath::Request &request,
    voxblox_msgs::FilePath::Response &response) {
  ROS_INFO_STREAM("Writing pose history to bag at: " << request.file_path);
  io::savePoseHistoryToFile(request.file_path,
                            submap_collection_ptr_->getPoseHistory());
  return true;
}

bool VoxgraphMapper::saveSeparatedMeshCallback(
    voxblox_msgs::FilePath::Request &request,
    voxblox_msgs::FilePath::Response &response) {
  submap_vis_.saveSeparatedMesh(request.file_path, *submap_collection_ptr_);
  return true;  // Tell ROS it succeeded
}

bool VoxgraphMapper::saveCombinedMeshCallback(
    voxblox_msgs::FilePath::Request &request,
    voxblox_msgs::FilePath::Response &response) {
  submap_vis_.saveCombinedMesh(request.file_path, *submap_collection_ptr_);
  return true;  // Tell ROS it succeeded
}

void VoxgraphMapper::switchToNewSubmap(const ros::Time &current_timestamp) {
  // Indicate that the previous submap is finished
  // s.t. its cached members are generated
  if (!submap_collection_ptr_->empty()) {
    submap_collection_ptr_->getActiveSubmapPtr()->finishSubmap();
  }

  // Add registration constraints for all overlapping submaps
  // NOTE: We do this before the new submap is created, such that its TSDF and
  //       ESDF are not yet used for registration. One advantage of this is that
  //       pointclouds can be integrated into the active (newest) submap without
  //       affecting the optimization (running in a separate) thread in any way.
  if (registration_constraints_enabled_) {
    pose_graph_interface_.updateRegistrationConstraints();
  }

  // Create the new submap
  submap_collection_ptr_->createNewSubmap(map_tracker_.get_T_M_B(),
                                          current_timestamp);

  // Add the new submap to the pose graph
  SubmapID active_submap_id = submap_collection_ptr_->getActiveSubmapID();
  pose_graph_interface_.addSubmap(active_submap_id);

  // Add an odometry constraint from the previous to the new submap
  if (odometry_constraints_enabled_ && submap_collection_ptr_->size() >= 2) {
    pose_graph_interface_.setVerbosity(true);
    std::cout << "T_S_B: " << map_tracker_.get_T_S_B() << std::endl;
    pose_graph_interface_.addOdometryMeasurement(
        submap_collection_ptr_->getPreviousSubmapId(),
        submap_collection_ptr_->getActiveSubmapID(), map_tracker_.get_T_S_B());
  }

  // Constrain the height
  // NOTE: No constraint is added for the 1st submap since its pose is fixed
  if (height_constraints_enabled_ && submap_collection_ptr_->size() > 1) {
    // This assumes that the height estimate from the odometry source is
    // really good (e.g. when it fuses an altimeter)
    double current_height = map_tracker_.get_T_O_B().getPosition().z();
    pose_graph_interface_.addHeightMeasurement(active_submap_id,
                                               current_height);
  }

  // Start tracking the odometry relative to the new submap
  map_tracker_.switchToNewSubmap();
}

int VoxgraphMapper::optimizePoseGraph() {
  // Optimize the pose graph
  ROS_INFO("Optimizing the pose graph");
  pose_graph_interface_.optimize();

  // Update the submap poses
  pose_graph_interface_.updateSubmapCollectionPoses();

  // Publish updated poses
  submap_server_.publishSubmapPoses(submap_collection_ptr_, ros::Time::now());

  // Report successful completion
  return 1;
}

void VoxgraphMapper::publishMaps(const ros::Time &current_timestamp) {
  // Publish the meshes if there are subscribers
  // NOTE: Users can request new meshes at any time through service calls
  //       so there's no point in publishing them just in case
  if (combined_mesh_pub_.getNumSubscribers() > 0) {
    std::thread combined_mesh_thread(&SubmapVisuals::publishCombinedMesh,
                                     &submap_vis_, *submap_collection_ptr_,
                                     map_tracker_.getFrameNames().mission_frame,
                                     combined_mesh_pub_);
    combined_mesh_thread.detach();
  }
  if (separated_mesh_pub_.getNumSubscribers() > 0) {
    std::thread separated_mesh_thread(
        &SubmapVisuals::publishSeparatedMesh, &submap_vis_,
        *submap_collection_ptr_, map_tracker_.getFrameNames().mission_frame,
        separated_mesh_pub_);
    separated_mesh_thread.detach();
  }

  // publish active mesh
  if (active_mesh_pub_.getNumSubscribers() > 0) {
    std::thread active_mesh_thread(
        static_cast<void (voxgraph::SubmapVisuals::*)(
            const cblox::SubmapCollection<VoxgraphSubmap>&,
            const cblox::SubmapID&, const voxblox::Color&, const std::string&,
            const ros::Publisher&)>(&SubmapVisuals::publishMesh),
        &submap_vis_, *submap_collection_ptr_,
        submap_collection_ptr_->getActiveSubmapID(),
//        voxblox::rainbowColorMap(1.0),
        voxblox::Color::Gray(),
        map_tracker_.getFrameNames().active_submap_frame, active_mesh_pub_);
    active_mesh_thread.detach();
  }

  // Publish the previous (finished) submap
  if (submap_collection_ptr_->size() > 1) {
    SubmapID previous_submap_id = submap_collection_ptr_->getPreviousSubmapId();
    submap_server_.publishSubmap(
        submap_collection_ptr_->getSubmap(previous_submap_id),
        current_timestamp);
  }

  // Publish the submap collection
  projected_map_server_.publishProjectedMap(*submap_collection_ptr_,
                                            current_timestamp);

  // Publish the submap poses
  submap_server_.publishSubmapPoses(submap_collection_ptr_,
      current_timestamp);

  // Publish the loop closure edges
  loop_closure_edge_server_.publishLoopClosureEdges(
      pose_graph_interface_, *submap_collection_ptr_, current_timestamp);
}

bool VoxgraphMapper::publishActiveSubmapCallback(
    cblox_msgs::SubmapSrv::Request &request,
    cblox_msgs::SubmapSrv::Response &response) {
  ROS_INFO("[VoxgraphMapper] Request for Active Submap Received, Processing.");
  if (!submap_collection_ptr_->empty()) {
    cblox_msgs::MapLayer msg = submap_server_.serializeActiveSubmap(
        submap_collection_ptr_, ros::Time::now());
    response.submap_msg = msg;
    return true;
  } else {
    return false;
  }
}
}  // namespace voxgraph
