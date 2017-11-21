/***************************************************************************
 *   Copyright (C) 2006 - 2017 by                                          *
 *      Tarek Taha, KURI  <tataha@tarektaha.com>                           *
 *      Randa Almadhoun   <randa.almadhoun@kustar.ac.ae>                   *
 *                                                                         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Steet, Fifth Floor, Boston, MA  02111-1307, USA.          *
 ***************************************************************************/
#include <ros/ros.h>
#include "sspp/pathplanner.h"

#include <ros/package.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseArray.h>

#include "sspp/distance_heuristic.h"
#include "rviz_visual_tools/rviz_visual_tools.h"
#include <octomap_world/octomap_manager.h>
#include <pcl_conversions/pcl_conversions.h>

class ReactivePlanner
{
private:
  ros::Subscriber sub;
  ros::NodeHandle nh;
  ros::NodeHandle nh_private;
  volumetric_mapping::OctomapManager * manager = NULL;
  rviz_visual_tools::RvizVisualToolsPtr visualTools;
  SSPP::PathPlanner* pathPlanner = NULL;
  Robot* robot = NULL;
  geometry_msgs::Point robotCenter;
  bool gotCloud = false;
  bool donePlanning = false;
  bool visualizeSearchSpace = false;
  bool sampleOrientations = false;
  bool debug = false;
  Pose start,end;
  double orientationSamplingRes = 90.0;
  double debugDelay = 0.0;
  double regGridConRad;
  double gridRes;
  double distanceToGoal = 0;
  int treeProgressDisplayFrequency = -1;
  geometry_msgs::Vector3 gridSize;
  std::vector<std::pair<Eigen::Vector3d, double> > occupied_box_vector;
public:
  ReactivePlanner(const ros::NodeHandle& nh_, const ros::NodeHandle& nh_private_):
    nh(nh_),
    nh_private(nh_private_)
  {
    sub = nh.subscribe<sensor_msgs::PointCloud2>("cloud_pcd", 1, &ReactivePlanner::callback,this);
    visualTools.reset(new rviz_visual_tools::RvizVisualTools("world", "/sspp_visualisation"));
    visualTools->loadMarkerPub();

    visualTools->deleteAllMarkers();
    visualTools->enableBatchPublishing();
    manager = new volumetric_mapping::OctomapManager(nh, nh_private);
    Eigen::Vector3d origin(0,0,0);
    Eigen::Vector3d envBox(15,15,10);
    manager->setFree(origin,envBox);
    ROS_INFO("Starting the reactive planning");
    //ros::Duration(5.0).sleep();
    ros::Rate loopRate(10);
    while(ros::ok())
    {
      manager->getAllOccupiedBoxes(&occupied_box_vector);
      ROS_INFO_THROTTLE(1,"MAP SIZE:[%f %f %f] occupied cells:%lu",manager->getMapSize()[0],manager->getMapSize()[1],manager->getMapSize()[2],occupied_box_vector.size());
      if(gotCloud && !donePlanning && occupied_box_vector.size()>0)
      {
        if (manager->getMapSize().norm() <= 0.0)
        {
            ROS_ERROR_THROTTLE(1, "Planner not set up: Octomap is empty!");
        }
        else
        {
          donePlanning = true;
          planPath();
        }
      }
      loopRate.sleep();
      ros::spinOnce();
    }
  }

  ~ReactivePlanner()
  {
    if(robot)
      delete robot;
    if(pathPlanner)
      delete pathPlanner;
    if (manager)
      delete manager;
  }

  void callback(const sensor_msgs::PointCloud2::ConstPtr& cloudIn)
  {
    gotCloud = true;
    manager->insertPointcloudWithTf(cloudIn);
  }

  void planPath()
  {
    manager->getAllOccupiedBoxes(&occupied_box_vector);
    ROS_INFO_THROTTLE(1,"While Planning MAP SIZE:[%f %f %f] occupied cells:%lu",manager->getMapSize()[0],manager->getMapSize()[1],manager->getMapSize()[2],occupied_box_vector.size());

    ros::Time timer_start = ros::Time::now();
    geometry_msgs::Pose gridStartPose;

    gridStartPose.position.x = 0.0;
    gridStartPose.position.y = 0.0;
    gridStartPose.position.z = 0.0;
    std::string ns = ros::this_node::getName();
    std::cout<<"Node name is:"<<ns<<"\n";
    ros::param::get(ns + "/start_x",start.p.position.x);
    ros::param::get(ns + "/start_y",start.p.position.y);
    ros::param::get(ns + "/start_z",start.p.position.z);

    ros::param::get(ns + "/end_x",end.p.position.x);
    ros::param::get(ns + "/end_y",end.p.position.y);
    ros::param::get(ns + "/end_z",end.p.position.z);
    ros::param::get(ns + "/connection_rad",regGridConRad);
    ros::param::get(ns + "/grid_resolution",gridRes);
    ros::param::get(ns + "/grid_size_x",gridSize.x);
    ros::param::get(ns + "/grid_size_y",gridSize.y);
    ros::param::get(ns + "/grid_size_z",gridSize.z);
    ros::param::get(ns + "/visualize_search_space",visualizeSearchSpace);
    ros::param::get(ns + "/debug",debug);
    ros::param::get(ns + "/debug_delay",debugDelay);
    ros::param::get(ns + "/dist_to_goal",distanceToGoal);
    ros::param::get(ns + "/sample_orientations",sampleOrientations);
    ros::param::get(ns + "/orientation_sampling_res",orientationSamplingRes);
    ros::param::get(ns + "/tree_progress_display_freq",treeProgressDisplayFrequency);


    start.phi = end.phi = DTOR(0.0);

    visualTools->publishSphere(start.p, rviz_visual_tools::BLUE, 0.3,"start_pose");
    visualTools->publishSphere(end.p, rviz_visual_tools::ORANGE, 0.3,"end_pose");
    visualTools->trigger();

    double robotH = 0.9, robotW = 0.5, narrowestPath = 0.987;

    robotCenter.x = -0.3f;
    robotCenter.y = 0.0f;
    if(!robot)
      robot = new Robot("Robot", robotH, robotW, narrowestPath, robotCenter);

    //Every how many iterations to display the tree; -1 disable display

    pathPlanner = new SSPP::PathPlanner(nh, robot, regGridConRad, treeProgressDisplayFrequency);

    // This causes the planner to pause for the desired amount of time and display
    // the search tree, useful for debugging
    pathPlanner->setDebugDelay(debugDelay);

    SSPP::DistanceHeuristic distanceHeuristic(nh, false,manager,visualTools);
    distanceHeuristic.setEndPose(end.p);
    distanceHeuristic.setTolerance2Goal(distanceToGoal);
    pathPlanner->setHeuristicFucntion(&distanceHeuristic);

    // Generate Grid Samples and visualise it
    pathPlanner->generateRegularGrid(gridStartPose, gridSize, gridRes, sampleOrientations, orientationSamplingRes,false, true);
    std::vector<geometry_msgs::Point> searchSpaceNodes = pathPlanner->getSearchSpace();

    std::vector<geometry_msgs::PoseArray> sensorsPoseSS;
    geometry_msgs::PoseArray robotPoseSS;
    pathPlanner->getRobotSensorPoses(robotPoseSS, sensorsPoseSS);
    std::cout << "\n\n---->>> Total Nodes in search Space ="<< searchSpaceNodes.size();

    visualTools->publishSpheres(searchSpaceNodes, rviz_visual_tools::PURPLE, 0.1,"search_space_nodes");
    visualTools->trigger();

    // Connect nodes and visualise it
    pathPlanner->connectNodes();
    std::cout << "\nSpace Generation took:"
              << double(ros::Time::now().toSec() - timer_start.toSec())
              << " secs";
    if(visualizeSearchSpace)
    {
      std::vector<geometry_msgs::Point> searchSpaceConnections = pathPlanner->getConnections();
      for(int i =0; i<(searchSpaceConnections.size() - 1) ;i+=2)
      {
        visualTools->publishLine(searchSpaceConnections[i], searchSpaceConnections[i+1], rviz_visual_tools::BLUE,rviz_visual_tools::LARGE);
      }
      visualTools->trigger();
    }

    // Find path and visualise it
    ros::Time timer_restart = ros::Time::now();
    SSPP::Node* path = pathPlanner->startSearch(start);
    ros::Time timer_end = ros::Time::now();
    std::cout << "\nPath Finding took:" << double(timer_end.toSec() - timer_restart.toSec()) << " secs";

    if (path)
    {
      pathPlanner->printNodeList();
    }
    else
    {
      std::cout << "\nNo Path Found";
    }

    geometry_msgs::Point linePoint;
    std::vector<geometry_msgs::Point> pathSegments;
    geometry_msgs::PoseArray robotPose, sensorPose;
    double dist = 0;
    double yaw;
    while (path != NULL)
    {
      tf::Quaternion qt(path->pose.p.orientation.x, path->pose.p.orientation.y,
                        path->pose.p.orientation.z, path->pose.p.orientation.w);
      yaw = tf::getYaw(qt);
      if (path->next != NULL)
      {
        linePoint.x = path->pose.p.position.x;
        linePoint.y = path->pose.p.position.y;
        linePoint.z = path->pose.p.position.z;
        robotPose.poses.push_back(path->pose.p);
        for (int i = 0; i < path->senPoses.size(); i++)
          sensorPose.poses.push_back(path->senPoses[i].p);
        pathSegments.push_back(linePoint);

        linePoint.x = path->next->pose.p.position.x;
        linePoint.y = path->next->pose.p.position.y;
        linePoint.z = path->next->pose.p.position.z;
        robotPose.poses.push_back(path->next->pose.p);
        for (int i = 0; i < path->next->senPoses.size(); i++)
          sensorPose.poses.push_back(path->next->senPoses[i].p);
        pathSegments.push_back(linePoint);

        dist = dist + Dist(path->next->pose.p, path->pose.p);
      }
      path = path->next;
    }

    std::cout << "\nDistance calculated from the path: " << dist << "m\n";

    for(int i =0; i<(pathSegments.size() - 1) ;i++)
    {
      visualTools->publishLine(pathSegments[i], pathSegments[i+1], rviz_visual_tools::RED);
    }
    visualTools->trigger();

    for (int i = 0; i < robotPose.poses.size(); i++)
    {
      visualTools->publishArrow(robotPose.poses[i], rviz_visual_tools::YELLOW, rviz_visual_tools::LARGE, 0.3);
    }
    visualTools->trigger();

    for (int i = 0; i < robotPoseSS.poses.size(); i++)
    {
      visualTools->publishArrow(robotPoseSS.poses[i], rviz_visual_tools::CYAN, rviz_visual_tools::LARGE, 0.3);
    }
    visualTools->trigger();

    for (int i = 0; i < sensorsPoseSS.size(); i++)
    {
      for(int j = 0; j < sensorsPoseSS[i].poses.size(); j++)
        visualTools->publishArrow(sensorsPoseSS[i].poses[j], rviz_visual_tools::DARK_GREY, rviz_visual_tools::LARGE, 0.3);
    }
    visualTools->trigger();
  }
};

int main(int argc, char** argv)
{
  ros::init(argc, argv, "reactive_planner_test");
  ros::NodeHandle nh;
  ros::NodeHandle nh_private("~");
  ReactivePlanner reactivePlanner(nh,nh_private);
  ros::spin();
  return 0;
}