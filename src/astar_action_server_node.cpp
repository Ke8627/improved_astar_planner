//
// Created by kevin on 3/24/18.
//

#include <ros/ros.h>
#include <actionlib/server/simple_action_server.h>
#include <iv_explore_msgs/PlannerControlAction.h>
#include "astar_planner/astar_search.h"
#include "astar_planner/search_info_ros.h"
#include <control_msgs/Traj_Node.h>
#include <control_msgs/Trajectory.h>
#include <iv_explore_msgs/GetAstarPlan.h>

using namespace astar_planner;

class AstarAction {
protected:
    ros::NodeHandle nh_;
    actionlib::SimpleActionServer<iv_explore_msgs::PlannerControlAction> as_;
    std::string action_name_;
    iv_explore_msgs::PlannerControlFeedback feedback_;
    iv_explore_msgs::PlannerControlResult result_;

public:
    AstarAction(std::string name) :
            as_(nh_, name,  false),
            action_name_(name),
            receive_goal_(false) {

        //register the goal and feeback callbacks
        as_.registerGoalCallback(boost::bind(&AstarAction::goalCB, this));
        as_.registerPreemptCallback(boost::bind(&AstarAction::preemptCB, this));

        // ROS param
        std::string vehicle_global_pose_topic;
        double path_update_rate_;
        nh_.param<std::string>("vehicle_global_pose_topic", vehicle_global_pose_topic, "/vehicle_global_pose_topic");
        nh_.param<double>("path_update_rate_", path_update_rate_, 0.2);
        nh_.param<double>("distance_threhold_to_goal_", distance_threhold_to_goal_, 2);

        // ROS subscribers
//        vehicle_pose_sub_ = nh_.subscribe("/vehicle_global_pose_topic", 1, &AstarAction::currentPoseCallback, this);
        // ROS publisher
        control_trajectory_pub_ = nh_.advertise<control_msgs::Trajectory>("/global_path", 1, false);
        // ROS service client
        plan_client_ = nh_.serviceClient<iv_explore_msgs::GetAstarPlan>(std::string("get_plan"));

        // ROS TIMER
        update_path_timer_ = nh_.createTimer(ros::Duration(path_update_rate_), &AstarAction::pathUpdateTimerCallback, this, false);

        as_.start();

    }

    ~AstarAction(void) {

    }

    void goalCB() {
        // do something quick here

        goal_list_.clear();
        // relative position to vehicle
        goal_list_ = as_.acceptNewGoal()->goals.poses;

        receive_goal_ = true;

        need_path_plan = true;

    }

    void preemptCB() {
        // do something here eg. reset flag and variable
        receive_goal_ = false;
        need_path_plan = false;

        ROS_INFO("%s: Preempted", action_name_.c_str());
        // set the action state to preempted
        as_.setPreempted();
    }

//    void currentPoseCallback(const nav_msgs::OdometryConstPtr &msg) {
//        //  within slam or dgps frame
//        current_vehicle_pose_ = msg->pose.pose;
//    }

    // first choose available goal point, then update path, publish to control
    void pathUpdateTimerCallback(const ros::TimerEvent& event) {
        double last_lasting_time = event.profile.last_duration.toSec();
        ROS_DEBUG("last path update cost time: %f [s]", last_lasting_time);
        static bool choose_path_flag = false;
        static int path_search_fail_counter = 0;

        // make sure that the action hasn't been canceled
        if (!as_.isActive())
            return;

        // get current vehicle position
        getCurrentPosition();
        // Note: no need to execute following code under condition:
        // * no accessible goals at first, or choose goals is unaccessible now
        // * astar planner is dead
        // * goal is reached, or goal is cancelled by client
        if(true == need_path_plan) {
            if (true == receive_goal_) {
                receive_goal_ = false;
                choose_path_flag = false;
            }
            // only enter this condition once
            if (false == choose_path_flag) {
                // first element is most expected one
                for (int i = 0; i < goal_list_.size(); i++) {
                    iv_explore_msgs::GetAstarPlan srv;
                    srv.request.start = current_vehicle_pose_;
                    srv.request.goal = goal_list_[i];
                    if (plan_client_.call(srv)) {
                        if (0 == srv.response.status_code) {
                            result_path_ = srv.response.path;
                            // record vaild goal_pose
                            goal_pose_ = goal_list_[i];
                            choose_path_flag = true;
                            break;
                        }
                    } else {
                        ROS_WARN("astar planner service callback fail : Could not get a planner map.");
                        ROS_INFO("%s: Aborted", action_name_.c_str());
                        as_.setPreempted();
                        need_path_plan = false;
                        return;
                    }
                }

                if (false == choose_path_flag) {
                    ROS_WARN("All goal point is unavailable !");
                    ROS_INFO("%s: Aborted", action_name_.c_str());
                    result_.success_flag = false;
                    //set the action state to aborted
                    as_.setAborted(result_);
                    need_path_plan = false;
                    return;
                }
            } else {
                // found a vaild goal point
                iv_explore_msgs::GetAstarPlan srv;
                srv.request.start = current_vehicle_pose_;  // todo vehicle position?
                srv.request.goal = goal_pose_;
                if (plan_client_.call(srv)) {
                    if (0 == srv.response.status_code) {
                        result_path_ = srv.response.path;
                        path_search_fail_counter = 0;
                    } else {
                        // more time to search here
                        result_path_.points.clear();  // stop here, hesitate
                        path_search_fail_counter++;
                    }
                } else {
                    ROS_WARN("astar planner service callback fail : Could not get a planner map.");
                    ROS_INFO("%s: Aborted", action_name_.c_str());
                    as_.setPreempted();
                    need_path_plan = false;
                    return;
                }

                if (path_search_fail_counter > 10) {
                    ROS_WARN("choose goal is unavailable now!");
                    ROS_INFO("%s: Aborted", action_name_.c_str());
                    result_.success_flag = false;
                    //set the action state to aborted
                    as_.setAborted(result_);
                    need_path_plan = false;
                    return;
                }

            }

            // publisher path to control msg
            control_trajectory_pub_.publish(result_path_);

            double dis2goal = astar::calcDistance(current_vehicle_pose_.position.x, current_vehicle_pose_.position.y,
                                                  goal_pose_.position.x, goal_pose_.position.y);
            if (dis2goal < distance_threhold_to_goal_) {
                result_.success_flag = true;
                as_.setSucceeded(result_);
                need_path_plan = false;
            }
        }


    }


private:

    void getCurrentPosition()
    {

        tf::StampedTransform transform;
        int temp = 0;
        while (temp == 0) {
            try {
                temp = 1;
                tf_listener_.lookupTransform( std::string("/odom"), std::string("base_link"), ros::Time(0), transform);
            } catch (tf::TransformException ex) {
                temp = 0;
                ros::Duration(0.1).sleep();
                ROS_INFO("no tf tree bet 'explore_map' and 'base_link' is received!");
            }
        }
        double x = transform.getOrigin().x();
        double y = transform.getOrigin().y();

        current_vehicle_pose_.position.x = x;
        current_vehicle_pose_.position.y = y;
        tf::quaternionTFToMsg(transform.getRotation(), current_vehicle_pose_.orientation);
    }




    ros::Subscriber vehicle_pose_sub_;

    ros::Publisher control_trajectory_pub_;
    ros::ServiceClient plan_client_;

    ros::Timer update_path_timer_;

    std::vector<geometry_msgs::Pose> goal_list_;

    geometry_msgs::Pose current_vehicle_pose_;
    geometry_msgs::Pose goal_pose_;

    bool receive_goal_;

    tf::TransformListener tf_listener_;

    control_msgs::Trajectory result_path_;

    double distance_threhold_to_goal_;

    bool need_path_plan = true;




};


int main(int argc, char **argv) {
    ros::init(argc, argv, "astar_action_server_node");

    AstarAction astar_server("astar_move_base");
    ros::spin();

    return 0;
}
