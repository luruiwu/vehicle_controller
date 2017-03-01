#include <vehicle_controller/controller.h>
#include <vehicle_controller/quaternions.h>
#include <vehicle_controller/utility.h>

#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <limits>

#include <geometry_msgs/PointStamped.h>
#include <std_msgs/Float32.h>
#include <std_msgs/String.h>

#include <vehicle_controller/four_wheel_steer_controller.h>
#include <vehicle_controller/differential_drive_controller.h>

#include <algorithm>
#include <sstream>
#include <functional>

Controller::Controller(const std::string& ns)
    : nh(ns), state(INACTIVE)
    , reverse_allowed(true), stuck(new StuckDetector)
{
    mp_.carrot_distance = 1.0;
    mp_.min_speed       = 0.0;
    mp_.commanded_speed = 0.0;
    mp_.max_controller_speed_ = 0.25;
    mp_.max_unlimited_speed_ = 2.0;
    mp_.max_unlimited_angular_rate_ = 1.0;
    mp_.max_controller_angular_rate_ =  0.4;
    mp_.inclination_speed_reduction_factor = 0.5 / (30 * M_PI/180.0); // 0.5 per 30 degrees
    mp_.inclination_speed_reduction_time_constant = 0.3;
    mp_.flipper_name = "flipper_front";
    mp_.pd_params = "PdParams";

    default_map_frame_id = "nav";
    base_frame_id = "base_link";

    final_twist_trials = 0;

    camera_control = false;
    camera_lookat_distance = 1.0;
    camera_lookat_height = -0.2;

    check_stuck = true;

    mp_.current_inclination = 0.0;
    velocity_error = 0.0;

    goal_position_tolerance = 0.0;
    goal_angle_tolerance = 0.0;

    cameraDefaultOrientation.header.frame_id = "base_stabilized";
    tf::Quaternion cameraOrientationQuaternion;
    cameraOrientationQuaternion.setEuler(0.0, 25.0*M_PI/180.0, 0.0);
    tf::quaternionTFToMsg(cameraOrientationQuaternion, cameraDefaultOrientation.quaternion);

    follow_path_server_.reset(new actionlib::SimpleActionServer<move_base_lite_msgs::FollowPathAction>(nh, "/controller/follow_path", 0, false));

    follow_path_server_->registerGoalCallback(boost::bind(&Controller::followPathGoalCallback, this));
    follow_path_server_->registerPreemptCallback(boost::bind(&Controller::followPathPreemptCallback, this));

    follow_path_server_->start();
}

Controller::~Controller()
{
}

bool Controller::configure()
{
    ros::NodeHandle params("~");
    params.getParam("carrot_distance", mp_.carrot_distance);
    params.getParam("min_speed", mp_.min_speed);
    params.getParam("frame_id", default_map_frame_id);
    setMapFrameId(default_map_frame_id);
    params.getParam("base_frame_id", base_frame_id);
    params.getParam("camera_control", camera_control);
    params.getParam("camera_lookat_distance", camera_lookat_distance);
    params.getParam("camera_lookat_height", camera_lookat_height);
    params.getParam("check_stuck", check_stuck);
    params.getParam("inclination_speed_reduction_factor", mp_.inclination_speed_reduction_factor);
    params.getParam("inclination_speed_reduction_time_constant", mp_.inclination_speed_reduction_time_constant);
    params.getParam("goal_position_tolerance", goal_position_tolerance);
    params.getParam("goal_angle_tolerance", goal_angle_tolerance);
    params.getParam("speed",   mp_.commanded_speed);
    params.param("pd_params",  mp_.pd_params, std::string("PdParams"));
    params.param("y_symmetry", mp_.y_symmetry, false);
    vehicle_control_type = "differential_steering";
    params.getParam("vehicle_control_type", vehicle_control_type);
    double stuck_detection_window = StuckDetector::DEFAULT_DETECTION_WINDOW;
    params.param("stuck_detection_window", stuck_detection_window, StuckDetector::DEFAULT_DETECTION_WINDOW);
    stuck.reset(new StuckDetector(stuck_detection_window));

    if (vehicle_control_type == "differential_steering")
        vehicle_control_interface_.reset(new DifferentialDriveController());
    else
        vehicle_control_interface_.reset(new FourWheelSteerController());
    vehicle_control_interface_->configure(params, &mp_);

    ROS_INFO("[vehicle_controller] Low level vehicle motion controller is %s", this->vehicle_control_interface_->getName().c_str());

    stateSubscriber     = nh.subscribe("state", 10, &Controller::stateCallback, this);
    drivetoSubscriber   = nh.subscribe("driveto", 10, &Controller::drivetoCallback, this);
    drivepathSubscriber = nh.subscribe("drivepath", 10, &Controller::drivepathCallback, this);
    cmd_velSubscriber   = nh.subscribe("cmd_vel", 10, &Controller::cmd_velCallback, this);
    cmd_velTeleopSubscriber = nh.subscribe("cmd_vel_teleop", 10, &Controller::cmd_velTeleopCallback, this);
    speedSubscriber     = nh.subscribe("speed", 10, &Controller::speedCallback, this);

    carrotPosePublisher = nh.advertise<geometry_msgs::PoseStamped>("carrot", 1, true);
    endPosePoublisher   = nh.advertise<geometry_msgs::PoseStamped>("end_pose", 1, true);
    drivepathPublisher  = nh.advertise<nav_msgs::Path>("drivepath", 1, true);
    pathPosePublisher   = nh.advertise<nav_msgs::Path>("smooth_path", 1, true);

    //cmd_flipper_toggle_sub_ = nh.subscribe("cmd_flipper_toggle", 10, &Controller::cmd_flipper_toggleCallback, this);
    //joint_states_sub_   = nh.subscribe("joint_states", 1, &Controller::joint_statesCallback, this);

    diagnosticsPublisher = params.advertise<std_msgs::Float32>("velocity_error", 1, true);
    autonomy_level_pub_ = nh.advertise<std_msgs::String>("/autonomy_level", 30);
    //jointstate_cmd_pub_ = nh.advertise<sensor_msgs::JointState>("/jointstate_cmd", 1, true);

    if (camera_control)
    {
        cameraOrientationPublisher = nh.advertise<geometry_msgs::QuaternionStamped>("camera/command", 1);
        lookatPublisher = nh.advertise<geometry_msgs::PointStamped>("camera/look_at", 1);
        cameraOrientationPublisher.publish(cameraDefaultOrientation);
    }
    empty_path.header.frame_id = map_frame_id;

    return true;
}

/*
void Controller::joint_statesCallback(sensor_msgs::JointStateConstPtr msg)
{
    auto it = std::find(msg->name.begin(), msg->name.end(), mp_.flipper_name);
    if(it != msg->name.end())
    {
        flipper_state = msg->position[std::distance(msg->name.begin(), it)];
    }
}
*/

bool Controller::updateRobotState(const nav_msgs::Odometry& odom_state)
{
    dt = (odom_state.header.stamp - robot_state_header.stamp).toSec();

    if (dt < 0.0 || dt > 0.2){
      ROS_INFO("[vehicle_controller] dt between old robot state and new is %f seconds, setting to 0.0 for this iteration", dt);
      dt = 0.0;
    }

    geometry_msgs::PoseStamped pose;
    geometry_msgs::Vector3Stamped velocity_linear;
    geometry_msgs::Vector3Stamped velocity_angular;
    pose.header = odom_state.header;
    pose.pose = odom_state.pose.pose;
    velocity_linear.header = odom_state.header;
    velocity_linear.vector = odom_state.twist.twist.linear;
    velocity_angular.header = odom_state.header;
    velocity_angular.vector = odom_state.twist.twist.angular;

    try
    {
        listener.waitForTransform(map_frame_id, odom_state.header.frame_id, odom_state.header.stamp, ros::Duration(3.0));
        listener.waitForTransform(base_frame_id, odom_state.header.frame_id, odom_state.header.stamp, ros::Duration(3.0));
        listener.transformPose(map_frame_id, pose, pose);
        listener.transformVector(base_frame_id, velocity_linear, velocity_linear);
        listener.transformVector(base_frame_id, velocity_angular, velocity_angular);

        robot_state_header = odom_state.header;
        robot_control_state.setRobotState(velocity_linear.vector, velocity_angular.vector, pose.pose, dt);
        robot_control_state.clearControlState();
    }
    catch (tf::TransformException ex)
    {
        ROS_ERROR("%s", ex.what());
        return false;
    }

    double inclination = acos(pose.pose.orientation.w * pose.pose.orientation.w - pose.pose.orientation.x * pose.pose.orientation.x
                              - pose.pose.orientation.y * pose.pose.orientation.y + pose.pose.orientation.z * pose.pose.orientation.z);
    if (inclination >= mp_.current_inclination)
        mp_.current_inclination = inclination;
    else
        mp_.current_inclination = (mp_.inclination_speed_reduction_time_constant * mp_.current_inclination + dt * inclination)
                                  / (mp_.inclination_speed_reduction_time_constant + dt);

    return true;

}

void Controller::stateCallback(const nav_msgs::OdometryConstPtr& odom_state)
{
    latest_odom_ = odom_state;

    if (state < DRIVETO) return;

    this->updateRobotState(*latest_odom_);

    update();    
}

void Controller::drivetoCallback(const ros::MessageEvent<geometry_msgs::PoseStamped>& event)
{
    setMapFrameId(default_map_frame_id);
    geometry_msgs::PoseStampedConstPtr goal = event.getConstMessage();

    //publishActionResult(actionlib_msgs::GoalStatus::PREEMPTED, "Received a new goal.");
    if (follow_path_server_->isActive()){
      move_base_lite_msgs::FollowPathResult result;
      result.result.val = move_base_lite_msgs::ErrorCodes::PREEMPTED;
      follow_path_server_->setPreempted(result, "drive to callback");
    }
    driveto(*goal, 0.0);
}

bool Controller::driveto(const geometry_msgs::PoseStamped& goal, double speed)
{
    reset();

    geometry_msgs::PoseStamped goal_transformed;
    try
    {
        listener.waitForTransform(map_frame_id, goal.header.frame_id, goal.header.stamp, ros::Duration(3.0));
        listener.transformPose(map_frame_id, goal, goal_transformed);
    }
    catch (tf::TransformException ex)
    {
        ROS_ERROR("[vehicle_controller] %s", ex.what());
        stop();
        //publishActionResult(actionlib_msgs::GoalStatus::REJECTED);
        if (follow_path_server_->isActive()){
          move_base_lite_msgs::FollowPathResult result;
          result.result.val = move_base_lite_msgs::ErrorCodes::TF_LOOKUP_FAILURE;
          follow_path_server_->setAborted(result, ex.what());
        }
        return false;
    }

    start = robot_control_state.pose;
    addLeg(goal_transformed.pose, speed);
    state = DRIVETO;

    ROS_INFO("[vehicle_controller] Received new goal point (x = %.2f, y = %.2f), backward = %d.",
             goal_transformed.pose.position.x, goal_transformed.pose.position.y, legs.back().backward);

    final_twist_trials = 0;
    //publishActionResult(actionlib_msgs::GoalStatus::ACTIVE);
    return true;
}

void Controller::drivepathCallback(const ros::MessageEvent<nav_msgs::Path>& event)
{
    setMapFrameId(default_map_frame_id);
    if (event.getPublisherName() == ros::this_node::getName()) return;
    nav_msgs::PathConstPtr path = event.getConstMessage();

    //publishActionResult(actionlib_msgs::GoalStatus::PREEMPTED, "received a new path");
    if (follow_path_server_->isActive()){
      ROS_INFO("Received new path while Action running, preempted.");
      move_base_lite_msgs::FollowPathResult result;
      result.result.val = move_base_lite_msgs::ErrorCodes::PREEMPTED;
      follow_path_server_->setPreempted(result, "drive path callback");
    }
    drivepath(*path, 0.0);
}

/*
void Controller::cmd_flipper_toggleCallback(std_msgs::Empty const &)
{
    sensor_msgs::JointState msg;
    msg.name = { mp_.flipper_name };
    msg.position = { flipper_state < mp_.flipper_switch_position ? mp_.flipper_high_position : mp_.flipper_low_position };
    jointstate_cmd_pub_.publish(msg);
}
*/

bool Controller::pathToBeSmoothed(std::deque<geometry_msgs::Pose> const & transformed_path, bool fixed_path)
{
    // Check if this path shall be smoothed
    // <=> path has length >= 2 and path is output of exploration planner
    //     and hence, the points lie in the centers of neighbouring grid cells.
    // Note: This function not robust to changes in the exploration planner

    if(transformed_path.size() < 2 || fixed_path)
        return false;

    bool path_to_be_smoothed = transformed_path.size() > 2;

    return path_to_be_smoothed;

//    double lu = 0.05 - 1e-5;
//    double lo = std::sqrt(2.0) * 0.05 + 1e-5;
//    std::stringstream sstr;
//    for(unsigned i = 1; path_to_be_smoothed && i < transformed_path.size() - 1; ++i)
//    {
//        double d = euclideanDistance(transformed_path[i].position,transformed_path[i - 1].position);
//        path_to_be_smoothed = path_to_be_smoothed && lu < d && d < lo;
//        sstr << " " << d << ":" << (lu < d && d < lo ? "T" : "F");
//    }
//    return path_to_be_smoothed;
}

bool Controller::drivepath(const nav_msgs::Path& path, double speed, bool fixed_path)
{
    reset();

    if (!latest_odom_.get()){
      ROS_ERROR("No latest odom message received, aborting path planning in drivepath!");
      stop();
      //publishActionResult(actionlib_msgs::GoalStatus::ABORTED);
      if (follow_path_server_->isActive()){
        move_base_lite_msgs::FollowPathResult result;
        result.result.val = move_base_lite_msgs::ErrorCodes::FAILURE;
        follow_path_server_->setAborted(result, "no odom message received");
      }
      return false;
    }

    this->updateRobotState(*latest_odom_);

    if (path.poses.size() == 0)
    {
        ROS_WARN("[vehicle_controller] Received empty path");
        stop();
        //publishActionResult(actionlib_msgs::GoalStatus::SUCCEEDED);
        if (follow_path_server_->isActive()){
          move_base_lite_msgs::FollowPathResult result;
          result.result.val = move_base_lite_msgs::ErrorCodes::SUCCESS;
          follow_path_server_->setSucceeded(result, "empty path received, automatic success.");
        }
        return false;
    }

    tf::StampedTransform transform;
    if(!createDrivepath2MapTransform(transform, path))
        return false;

    std::deque<geometry_msgs::Pose> map_path(path.poses.size());
    std::transform(path.poses.begin(), path.poses.end(), map_path.begin(),
                   [transform](geometry_msgs::PoseStamped const & ps)
                   {
                        tf::Pose tf_waypoint;
                        geometry_msgs::Pose transformed_waypoint;
                        tf::poseMsgToTF(ps.pose, tf_waypoint);
                        tf_waypoint = transform * tf_waypoint;
                        tf::poseTFToMsg(tf_waypoint, transformed_waypoint);
                        return transformed_waypoint;
                   });

    geometry_msgs::PoseStamped ptbp;
    ptbp.header.stamp = ros::Time::now();
    ptbp.header.frame_id = map_frame_id;
    ptbp.pose.position = map_path.back().position;
    ptbp.pose.orientation = map_path.back().orientation;
    endPosePoublisher.publish(ptbp);

    if(pathToBeSmoothed(map_path, fixed_path))
    {
        ROS_DEBUG("[vehicle_controller] Using PathSmoother.");
        start = map_path[0];
        start.orientation = robot_control_state.pose.orientation;

        Pathsmoother3D ps3d(vehicle_control_type == "differential_steering", &mp_);

        quat in_start_orientation;
        quat in_end_orientation;

        deque_vec3 in_path;
        std::transform(map_path.begin(), map_path.end(), std::back_inserter(in_path),
                       [](geometry_msgs::Pose const & pose_)
                       { return vec3(pose_.position.x, pose_.position.y, pose_.position.z); });

        in_start_orientation = geomQuat2EigenQuat(robot_control_state.pose.orientation);
        in_end_orientation   = geomQuat2EigenQuat(map_path.back().orientation);

        vector_vec3 out_smoothed_positions;
        vector_quat out_smoothed_orientations;
        ps3d.smooth(in_path, in_start_orientation, in_end_orientation,
                    out_smoothed_positions, out_smoothed_orientations, reverse_allowed);

        std::vector<geometry_msgs::Pose> smooth_path;
        std::transform(out_smoothed_positions.begin(), out_smoothed_positions.end(),
                       out_smoothed_orientations.begin(), std::back_inserter(smooth_path),
                       boost::bind(&Controller::createPoseFromQuatAndPosition, this, _1, _2));

        std::for_each(smooth_path.begin() + 1, smooth_path.end(), boost::bind(&Controller::addLeg, this, _1, speed));

        nav_msgs::Path path2publish;
        path2publish.header.frame_id = map_frame_id;
        path2publish.header.stamp = ros::Time::now();
        std::transform(smooth_path.begin(), smooth_path.end(), std::back_inserter(path2publish.poses),
                       [path2publish](geometry_msgs::Pose const & pose)
                       {
                            geometry_msgs::PoseStamped ps;
                            ps.header = path2publish.header;
                            ps.pose = pose;
                            return ps;
                       });
        pathPosePublisher.publish(path2publish);
    }
    else
    {
        for(nav_msgs::Path::_poses_type::const_iterator waypoint = path.poses.begin(); waypoint != path.poses.end(); ++waypoint)
        {
            tf::Pose tf_waypoint;
            geometry_msgs::Pose transformed_waypoint;
            tf::poseMsgToTF(waypoint->pose, tf_waypoint);
            tf_waypoint = transform * tf_waypoint;
            tf::poseTFToMsg(tf_waypoint, transformed_waypoint);
            if (waypoint == path.poses.begin())
            {
                start = transformed_waypoint;
                start.orientation = robot_control_state.pose.orientation;
            }
            else
            {
                addLeg(transformed_waypoint);
            }
            if (path.poses.size() == 1) // TODO: Consider calling driveto instead for small paths instead.
            {
                transformed_waypoint.position.x += 0.01;
                addLeg(transformed_waypoint, speed);
            }
        }
        nav_msgs::Path path;
        path.header.frame_id = map_frame_id;
        path.header.stamp = ros::Time::now();
        pathPosePublisher.publish(path);
    }

    state = DRIVEPATH;
    if(legs.size() >= 1)
        ROS_INFO("[vehicle_controller] Received new path to goal point (x = %.2f, y = %.2f)", legs.back().p2.x, legs.back().p2.y);
    else
        ROS_WARN("[vehicle_controller] Controller::drivepath produced empty legs array.");

    //publishActionResult(actionlib_msgs::GoalStatus::ACTIVE);
    return true;
}

geometry_msgs::Pose Controller::createPoseFromQuatAndPosition(vec3 const & position, quat const & orientation)
{
    geometry_msgs::Pose pose;
    pose.position.x = position(0);
    pose.position.y = position(1);
    pose.position.z = position(2);
    pose.orientation.w = orientation.w();
    pose.orientation.x = orientation.x();
    pose.orientation.y = orientation.y();
    pose.orientation.z = orientation.z();
    return pose;
}

bool Controller::createDrivepath2MapTransform(tf::StampedTransform & transform, const nav_msgs::Path& path)
{
    if (!path.header.frame_id.empty())
    {
        try
        {
            listener.waitForTransform(this->map_frame_id, path.header.frame_id, path.header.stamp, ros::Duration(3.0));
            listener.lookupTransform(this->map_frame_id, path.header.frame_id, path.header.stamp, transform);
        }
        catch (tf::TransformException ex)
        {
            ROS_ERROR("[vehicle_controller] Drivepath transformation to map frame failed: "
                      "%s", ex.what());
            stop();
            //publishActionResult(actionlib_msgs::GoalStatus::REJECTED);
            if (follow_path_server_->isActive()){
              move_base_lite_msgs::FollowPathResult result;
              result.result.val = move_base_lite_msgs::ErrorCodes::TF_LOOKUP_FAILURE;
              follow_path_server_->setAborted(result, ex.what());
            }
            return false;
        }
    }
    else
    {
        ROS_WARN("[vehicle_controller] Received a path with empty frame_id. Assuming frame = %s.", map_frame_id.c_str());
        transform.setIdentity();
    }
    return true;
}

void Controller::cmd_velCallback(const geometry_msgs::Twist& velocity)
{
    //publishActionResult(actionlib_msgs::GoalStatus::PREEMPTED, "received a velocity command");
    if (follow_path_server_->isActive()){
      ROS_INFO("Direct cmd_vel received, preempting running Action!");
      move_base_lite_msgs::FollowPathResult result;
      result.result.val = move_base_lite_msgs::ErrorCodes::PREEMPTED;
      follow_path_server_->setPreempted(result, "cmd_vel callback");
    }
    reset();
    state = ((velocity.linear.x == 0.0) && (velocity.angular.z == 0.0)) ? INACTIVE : VELOCITY;
    vehicle_control_interface_->executeTwist(velocity);
}

void Controller::cmd_velTeleopCallback(const geometry_msgs::Twist& velocity)
{
    if (follow_path_server_->isActive()){
      ROS_INFO("Direct teleop cmd_vel received, preempting running Action!");
      move_base_lite_msgs::FollowPathResult result;
      result.result.val = move_base_lite_msgs::ErrorCodes::PREEMPTED;
      follow_path_server_->setPreempted(result, "cmd_vel teleop callback");
    }
    reset();
    state = ((velocity.linear.x == 0.0) && (velocity.angular.z == 0.0)) ? INACTIVE : VELOCITY;

    std_msgs::String autonomy_level;
    autonomy_level.data = "teleop";
    autonomy_level_pub_.publish(autonomy_level);

    vehicle_control_interface_->executeUnlimitedTwist(velocity);
}

void Controller::speedCallback(const std_msgs::Float32& speed)
{
    mp_.commanded_speed = speed.data;
}

void Controller::followPathGoalCallback()
{
  //actionlib::SimpleActionServer<move_base_lite_msgs::FollowPathAction>::GoalConstPtr goal = follow_path_server_->acceptNewGoal();
  follow_path_goal_ = follow_path_server_->acceptNewGoal();
  setMapFrameId(follow_path_goal_->target_path.header.frame_id);
  drivepath(follow_path_goal_->target_path, follow_path_goal_->follow_path_options.desired_speed, false);//path_action->speed, path_action->fixed);
  drivepathPublisher.publish(follow_path_goal_->target_path);
}

void Controller::followPathPreemptCallback()
{
  move_base_lite_msgs::FollowPathResult result;
  result.result.val = move_base_lite_msgs::ErrorCodes::PREEMPTED;
  follow_path_server_->setPreempted(result, "preempt from incoming message to server");
  reset();
  setMapFrameId(default_map_frame_id);
}

void Controller::addLeg(geometry_msgs::Pose const& pose, double speed)
{
    Leg leg;
    double angles[3];

    leg.p2.x = pose.position.x;
    leg.p2.y = pose.position.y;

    if (legs.size() == 0)
    {
        leg.p1.x = start.position.x;
        leg.p1.y = start.position.y;
        leg.course = atan2(leg.p2.y - leg.p1.y, leg.p2.x - leg.p1.x);

        if (start.orientation.w == 0.0 && start.orientation.x == 0.0
         && start.orientation.y == 0.0 && start.orientation.z == 0.0)
        {
            leg.p1.orientation = leg.course;
        }
        else
        {
            quaternion2angles(start.orientation, angles);
            leg.p1.orientation = angles[0];
        }
    }
    else
    {
        const Leg& last = legs.back();
        leg.p1.x = last.p2.x;
        leg.p1.y = last.p2.y;
        leg.p1.orientation = last.p2.orientation;
        leg.course = atan2(leg.p2.y - leg.p1.y, leg.p2.x - leg.p1.x);
    }

    leg.backward = fabs(constrainAngle_mpi_pi(leg.course - leg.p1.orientation)) > M_PI_2;
    if (pose.orientation.w == 0.0 && pose.orientation.x == 0.0
     && pose.orientation.y == 0.0 && pose.orientation.z == 0.0)
    {
        leg.p2.orientation = leg.backward ? angularNorm(leg.course + M_PI) : leg.course;
    }
    else
    {
        quaternion2angles(pose.orientation, angles);
        leg.p2.orientation = angles[0];
    }

    leg.speed   = speed == 0.0 ? mp_.commanded_speed : speed;
    leg.length2 = std::pow(leg.p2.x - leg.p1.x, 2) + std::pow(leg.p2.y - leg.p1.y, 2);
    leg.length  = std::sqrt(leg.length2);
    leg.percent = 0.0f;

    if (leg.length2 == 0.0f) return;
    legs.push_back(leg);
}

void Controller::reset()
{
    state = INACTIVE;
    current = 0;
    final_twist_trials = 0;
    dt = 0.0;
    legs.clear();
}

void Controller::update()
{
    if (state < DRIVETO) return;

    // get current orientation
    double angles[3];
    quaternion2angles(robot_control_state.pose.orientation, angles);

    double linear_tolerance_for_current_path = goal_position_tolerance;
    double angular_tolerance_for_current_path = goal_angle_tolerance;

    if (follow_path_server_->isActive()){
      if (follow_path_goal_->follow_path_options.goal_pose_position_tolerance > 0.0){
        linear_tolerance_for_current_path = follow_path_goal_->follow_path_options.goal_pose_position_tolerance;
      }

      if (follow_path_goal_->follow_path_options.goal_pose_angle_tolerance > 0.0){
        angular_tolerance_for_current_path = follow_path_goal_->follow_path_options.goal_pose_angle_tolerance;
      }
    }

    // Check if goal has been reached based on goal_position_tolerance/goal_angle_tolerance
    double goal_position_error =
            std::sqrt(
                std::pow(legs.back().p2.x - robot_control_state.pose.position.x, 2)
              + std::pow(legs.back().p2.y - robot_control_state.pose.position.y, 2));
    double goal_angle_error_   = angularNorm(legs.back().p2.orientation - angles[0]);

    if (goal_position_error < linear_tolerance_for_current_path
     /*&& vehicle_control_interface_->hasReachedFinalOrientation(goal_angle_error_, angular_tolerance_for_current_path)*/)
    {   // Reached goal point. This task is handled in the following loop
        ROS_INFO_THROTTLE(1.0, "[vehicle_controller] Current position is within goal tolerance.");
        current = legs.size();
    }

    while(1)
    {
        if (current == legs.size())
        {
            goal_angle_error_ = constrainAngle_mpi_pi(goal_angle_error_);
            // ROS_INFO("[vehicle_controller] Reached goal point position. Angular error = %f, tol = %f", goal_angle_error_ * 180.0 / M_PI, angular_tolerance_for_current_path * 180.0 / M_PI);
            if (final_twist_trials > mp_.FINAL_TWIST_TRIALS_MAX_
             || vehicle_control_interface_->hasReachedFinalOrientation(goal_angle_error_,
                    angular_tolerance_for_current_path, reverse_allowed)
             || !mp_.USE_FINAL_TWIST_)
            {
                state = INACTIVE;
                ROS_INFO("[vehicle_controller] Finished orientation correction!"
                         " error = %f, tol = %f",
                         goal_angle_error_ * 180.0 / M_PI
                         , angular_tolerance_for_current_path * 180.0 / M_PI);
                final_twist_trials = 0;
                stop();
                if (follow_path_server_->isActive()){
                  move_base_lite_msgs::FollowPathResult result;
                  result.result.val = move_base_lite_msgs::ErrorCodes::SUCCESS;
                  follow_path_server_->setSucceeded(result, "reached goal");
                }
                return;
            }
            else // Perform twist at end of path to obtain a desired orientation
            {
                final_twist_trials++;
                ROS_DEBUG("[vehicle_controller] Performing final twist.");

                geometry_msgs::Vector3 desired_position;
                desired_position.x = legs.back().p2.x;
                desired_position.y = legs.back().p2.y;

                robot_control_state.setControlState(0.0,
                                                    desired_position,
                                                    goal_angle_error_,
                                                    goal_angle_error_,
                                                    mp_.carrot_distance,
                                                    0.0,
                                                    true,
                                                    reverse_allowed);

                vehicle_control_interface_->executeMotionCommand(robot_control_state);
                return;
            }
        }

        legs[current].percent =
                (  (robot_control_state.pose.position.x - legs[current].p1.x)
                   * (legs[current].p2.x - legs[current].p1.x)
                 + (robot_control_state.pose.position.y - legs[current].p1.y)
                   * (legs[current].p2.y - legs[current].p1.y))
                / legs[current].length2;

        ROS_DEBUG("[vehicle_controller] Robot has passed %.1f percent of leg %u.", legs[current].percent, current);
        if (legs[current].percent < 1.0) break;

        ++current;
        ROS_DEBUG("[vehicle_controller] Robot reached waypoint %d", current);
    }

    // calculate carrot
    Point carrot;
    unsigned int carrot_waypoint = current;
    float carrot_percent = legs[current].percent;
    float carrot_remaining = mp_.carrot_distance;

    while(carrot_waypoint < legs.size())
    {
        if (carrot_remaining <= (1.0f - carrot_percent) * legs[carrot_waypoint].length)
        {
            carrot_percent += carrot_remaining / legs[carrot_waypoint].length;
            break;
        }

        carrot_remaining -= (1.0f - carrot_percent) * legs[carrot_waypoint].length;
        if (carrot_waypoint + 1 < legs.size()
        && legs[carrot_waypoint].backward == legs[carrot_waypoint + 1].backward)
        {
            ROS_DEBUG("Carrot reached waypoint %d", carrot_waypoint);
            carrot_percent = 0.0f;
            carrot_waypoint++;
        }
        else
        {
            ROS_DEBUG("Carrot reached last waypoint or change of direction");
            carrot_percent = 1.0f + carrot_remaining / legs[carrot_waypoint].length;
            break;
        }
    }

    carrot.x           = (1.0f - carrot_percent) * legs[carrot_waypoint].p1.x + carrot_percent * legs[carrot_waypoint].p2.x;
    carrot.y           = (1.0f - carrot_percent) * legs[carrot_waypoint].p1.y + carrot_percent * legs[carrot_waypoint].p2.y;
    // carrot.orientation = legs[carrot_waypoint].p1.orientation + std::min(carrot_percent, 1.0f) * angular_norm(legs[carrot_waypoint].p2.orientation - legs[carrot_waypoint].p1.orientation);

    if (carrot_waypoint == legs.size() - 1)
    {
        carrot.orientation = legs[carrot_waypoint].p1.orientation + std::min(carrot_percent, 1.0f) * angularNorm(legs[carrot_waypoint].p2.orientation - legs[carrot_waypoint].p1.orientation);
    }
    else
    {
        carrot.orientation = legs[carrot_waypoint].p1.orientation + /* carrot_percent * */ 1.0f * angularNorm(legs[carrot_waypoint].p2.orientation - legs[carrot_waypoint].p1.orientation);
    }

    carrotPose.header = robot_state_header;
    carrotPose.pose.position.x = carrot.x;
    carrotPose.pose.position.y = carrot.y;
    double ypr[3] = { carrot.orientation, 0.0, 0.0 };
    angles2quaternion(ypr, carrotPose.pose.orientation);
    if (carrotPosePublisher)
        carrotPosePublisher.publish(carrotPose);

    // --------------------------------------------------------------------------------------------------- //
    // alpha = robot orientation                      # robot orientation in global cosy                   //
    // beta  = angle(carrot position, robot position) # path orientation in global cosy                    //
    // error_2_path   = beta - alpha                  # error of robot orientation to path orientation     //
    // error_2_carrot = carrot orientation - alpha    # error of robot orientation to carrot orientation   //
    // --------------------------------------------------------------------------------------------------- //

    geometry_msgs::Vector3 desired_position;
    desired_position.x = carrot.x;
    desired_position.y = carrot.y;
    desired_position.z = 0.0;

    double alpha = angles[0];
    double beta  = atan2(carrot.y - robot_control_state.pose.position.y,
                         carrot.x - robot_control_state.pose.position.x);

    double error_2_path   = constrainAngle_mpi_pi( beta - alpha );
    double error_2_carrot = constrainAngle_mpi_pi( carrot.orientation - alpha);
    double sign  = legs[current].backward ? -1.0 : 1.0;

    if (this->mp_.isYSymmetric() || this->reverse_allowed) {
        vec3 rdp(desired_position.x - robot_control_state.pose.position.x, desired_position.y - robot_control_state.pose.position.y, 0.0);
        rdp.normalize();
        quat rq(robot_control_state.pose.orientation.w, robot_control_state.pose.orientation.x, robot_control_state.pose.orientation.y, robot_control_state.pose.orientation.z);

        vec3 rpath = /*rq **/ rdp;
        vec3 rpos = rq * vec3(1,0,0);
//        ROS_ERROR("rpath = %f  %f  %f", rpath(0), rpath(1), rpath(2));
//        ROS_ERROR("rpos  = %f  %f  %f", rpos(0), rpos(1), rpos(2));
//        ROS_ERROR("rpos^T rpath = %f" , rpos.dot(rpath));
//        ROS_ERROR("alpha = %f" , alpha * 180 / M_PI);
//        ROS_ERROR("beta = %f" , beta * 180 / M_PI);
//        ROS_ERROR("beta - alpha = %f" , (beta - alpha) * 180 / M_PI);
//        ROS_ERROR("ang path = %f" , error_2_path * 180 / M_PI);
        sign = rpos.dot(rpath) >= 0.0 ? 1.0 : -1.0;
    }

    double speed = sign * legs[current].speed;
    double signed_carrot_distance_2_robot =
            sign * euclideanDistance2D(carrotPose.pose.position,
                                     robot_control_state.pose.position);
    bool approaching_goal_point = goal_position_error < 0.4;



    if(std::abs(signed_carrot_distance_2_robot) > (mp_.carrot_distance * 1.5))
    {
        ROS_WARN("[vehicle_controller] Control failed, distance to carrot is %f (allowed: %f)", signed_carrot_distance_2_robot, (mp_.carrot_distance * 1.5));
        state = INACTIVE;
        stop();

        if (follow_path_server_->isActive()){
          move_base_lite_msgs::FollowPathResult result;
          result.result.val = move_base_lite_msgs::ErrorCodes::CONTROL_FAILED;
          follow_path_server_->setAborted(result, std::string("Control failed, distance between trajectory and robot too large."));
        }
        return;
    }

    if (state == DRIVETO && goal_position_error < 0.6 /* && mp_.isYSymmetric() */)
    { // TODO: Does mp_.isYSymmetric() really make sense here?
        if(error_2_path > M_PI_2)
            error_2_path = error_2_path - M_PI;
        if(error_2_path < -M_PI_2)
            error_2_path = M_PI + error_2_path;
    }

    robot_control_state.setControlState(speed,
                                        desired_position,
                                        error_2_path,
                                        error_2_carrot,
                                        mp_.carrot_distance,
                                        signed_carrot_distance_2_robot,
                                        approaching_goal_point,
                                        reverse_allowed);

    vehicle_control_interface_->executeMotionCommand(robot_control_state);

    if (check_stuck && !isDtInvalid())
    {
        geometry_msgs::PoseStamped ps;
        ps.header = robot_state_header;
        ps.pose   = robot_control_state.pose;
        stuck->update(ps);
        if((*stuck)(robot_control_state.desired_velocity_linear))
        {
            ROS_WARN("[vehicle_controller] I think I am blocked! Terminating current drive goal.");
            state = INACTIVE;
            stop();
            stuck->reset();
            //publishActionResult(actionlib_msgs::GoalStatus::ABORTED,
            //                    vehicle_control_type == "differential_steering" ? "blocked_tracked" : "blocked");
            if (follow_path_server_->isActive()){
              move_base_lite_msgs::FollowPathResult result;
              result.result.val = move_base_lite_msgs::ErrorCodes::STUCK_DETECTED;
              follow_path_server_->setAborted(result, std::string("I think I am blocked! Terminating current drive goal."));
            }
        }
    }

    // camera control
    if (camera_control)
    {
        // calculate lookat position
        Point lookat;
        unsigned int lookat_waypoint = current;
        bool found_lookat_position = false;

        while(lookat_waypoint < legs.size())
        {
            lookat.x           = legs[lookat_waypoint].p2.x;
            lookat.y           = legs[lookat_waypoint].p2.y;
            lookat.orientation = legs[lookat_waypoint].p2.orientation;

            double distance =
                    std::sqrt(
                        (robot_control_state.pose.position.x - lookat.x)
                      * (robot_control_state.pose.position.x - lookat.x)
                     +  (robot_control_state.pose.position.y - lookat.y)
                      * (robot_control_state.pose.position.y - lookat.y));
            double relative_angle =
                    angularNorm(atan2(lookat.y - robot_control_state.pose.position.y,
                                      lookat.x - robot_control_state.pose.position.x)
                                - angles[0]);

            if (distance >= camera_lookat_distance && relative_angle >= -M_PI/2 && relative_angle <= M_PI/2) {
                found_lookat_position = true;
                break;
            }

            if (lookat_waypoint+1 < legs.size()) {
                ROS_DEBUG("lookat reached waypoint %d", lookat_waypoint);
                lookat_waypoint++;
            } else {
                ROS_DEBUG("lookat reached last waypoint");
                break;
            }
        }

        if (found_lookat_position) {
            geometry_msgs::PointStamped lookat_msg;
            lookat_msg.header  = robot_state_header;
            lookat_msg.point.x = lookat.x;
            lookat_msg.point.y = lookat.y;
            lookat_msg.point.z =   robot_control_state.pose.position.z
                                 + camera_lookat_height;
            lookatPublisher.publish(lookat_msg);
        } else {
            cameraOrientationPublisher.publish(cameraDefaultOrientation);
            //      lookat_msg.header.frame_id = "base_stabilized";
            //      lookat_msg.point.x = 1.0;
            //      lookat_msg.point.y = 0.0;
            //      lookat_msg.point.z = camera_lookat_height;

        }
    }
}

void Controller::stop()
{
    this->vehicle_control_interface_->stop();
    stuck->reset();
    drivepathPublisher.publish(empty_path);
    if (camera_control)
        cameraOrientationPublisher.publish(cameraDefaultOrientation);
}

void Controller::cleanup()
{
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, ROS_PACKAGE_NAME);

    Controller c;
    c.configure();
    while(ros::ok())
    {
        ros::spin();
    }
    c.cleanup();

    ros::shutdown();
    return 0;
}
