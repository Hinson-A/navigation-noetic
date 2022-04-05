/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/
#include <clear_costmap_recovery/clear_costmap_recovery.h>
#include <pluginlib/class_list_macros.h>
#include <boost/pointer_cast.hpp>
#include <vector>

//register this planner as a RecoveryBehavior plugin
// 该插件将costmap中给定半径（reset_distance_默认值3.0）范围之内的区域进行清理，即将栅格状态更新为未知信息
PLUGINLIB_EXPORT_CLASS(clear_costmap_recovery::ClearCostmapRecovery, nav_core::RecoveryBehavior)

using costmap_2d::NO_INFORMATION;

namespace clear_costmap_recovery {
ClearCostmapRecovery::ClearCostmapRecovery(): global_costmap_(NULL), local_costmap_(NULL),
  tf_(NULL), initialized_(false) {}

void ClearCostmapRecovery::initialize(std::string name, tf2_ros::Buffer* tf,
    costmap_2d::Costmap2DROS* global_costmap, costmap_2d::Costmap2DROS* local_costmap){
  if(!initialized_){
    name_ = name;
    tf_ = tf;
    global_costmap_ = global_costmap;
    local_costmap_ = local_costmap;

    // 参数服务器中获得参数
    ros::NodeHandle private_nh("~/" + name_);
  // 代价地图清除的距离　以该范围为边长　画一个正方形
  //从地图上清除用户该区域以外的障碍物
    private_nh.param("reset_distance", reset_distance_, 3.0);
    private_nh.param("invert_area_to_clear", invert_area_to_clear_, false);
    private_nh.param("force_updating", force_updating_, false);
    private_nh.param("affected_maps", affected_maps_, std::string("both"));
    // 地图清理的范围　local global 或者两者都包括
    if (affected_maps_ != "local" && affected_maps_ != "global" && affected_maps_ != "both")
    {
      ROS_WARN("Wrong value for affected_maps parameter: '%s'; valid values are 'local', 'global' or 'both'; " \
               "defaulting to 'both'", affected_maps_.c_str());
      affected_maps_ = "both";
    }
　// 清理的地图层　默认障碍物层
    std::vector<std::string> clearable_layers_default, clearable_layers;
    clearable_layers_default.push_back( std::string("obstacles") );
    private_nh.param("layer_names", clearable_layers, clearable_layers_default);

    for(unsigned i=0; i < clearable_layers.size(); i++) {
        ROS_INFO("Recovery behavior will clear layer '%s'", clearable_layers[i].c_str());
        clearable_layers_.insert(clearable_layers[i]);
    }

    initialized_ = true;
  }
  else{
    ROS_ERROR("You should not call initialize twice on this object, doing nothing");
  }
}

void ClearCostmapRecovery::runBehavior(){
  if(!initialized_){
    ROS_ERROR("This object must be initialized before runBehavior is called");
    return;
  }

  if(global_costmap_ == NULL || local_costmap_ == NULL){
    ROS_ERROR("The costmaps passed to the ClearCostmapRecovery object cannot be NULL. Doing nothing.");
    return;
  }

  if (!invert_area_to_clear_){
    ROS_WARN("Clearing %s costmap%s outside a square (%.2fm) large centered on the robot.", affected_maps_.c_str(),
           affected_maps_ == "both" ? "s" : "", reset_distance_);
  }else {
    ROS_WARN("Clearing %s costmap%s inside a square (%.2fm) large centered on the robot.", affected_maps_.c_str(),
           affected_maps_ == "both" ? "s" : "", reset_distance_);
  }

  ros::WallTime t0 = ros::WallTime::now();
  if (affected_maps_ == "global" || affected_maps_ == "both")
  {
    clear(global_costmap_);

    if (force_updating_)
      global_costmap_->updateMap();

    ROS_DEBUG("Global costmap cleared in %fs", (ros::WallTime::now() - t0).toSec());
  }

  t0 = ros::WallTime::now();
  if (affected_maps_ == "local" || affected_maps_ == "both")
  {
    clear(local_costmap_);

    if (force_updating_)
      local_costmap_->updateMap();

    ROS_DEBUG("Local costmap cleared in %fs", (ros::WallTime::now() - t0).toSec());
  }
}

/*
获取了costmap的各个地图层，在for循环中不断寻找clearable_layers_中是否有各地图层的名字，
若有，则对其调用clearMap、进行清理。
*/
void ClearCostmapRecovery::clear(costmap_2d::Costmap2DROS* costmap){
  std::vector<boost::shared_ptr<costmap_2d::Layer> >* plugins = costmap->getLayeredCostmap()->getPlugins();

  geometry_msgs::PoseStamped pose;

  // 获得机器人全局位姿
  if(!costmap->getRobotPose(pose)){
    ROS_ERROR("Cannot clear map because pose cannot be retrieved");
    return;
  }

  double x = pose.pose.position.x;
  double y = pose.pose.position.y;

  for (std::vector<boost::shared_ptr<costmap_2d::Layer> >::iterator pluginp = plugins->begin(); pluginp != plugins->end(); ++pluginp) {
    boost::shared_ptr<costmap_2d::Layer> plugin = *pluginp;
    std::string name = plugin->getName();
    // 返回该字符在字符串中的下标
    int slash = name.rfind('/');
    // 如果找到匹配的
    // npos是一个常数，用来表示不存在的位置,string::npos代表字符串到头了结束了
    if( slash != std::string::npos ){
        name = name.substr(slash+1);
    }

    if(clearable_layers_.count(name)!=0){

      // 检查该layer是否来自costmap_2d::CostmapLayer
      if(!dynamic_cast<costmap_2d::CostmapLayer*>(plugin.get())){
        ROS_ERROR_STREAM("Layer " << name << " is not derived from costmap_2d::CostmapLayer");
        continue;
      }

      boost::shared_ptr<costmap_2d::CostmapLayer> costmap;
      costmap = boost::static_pointer_cast<costmap_2d::CostmapLayer>(plugin);
      // 重置特定层的代价地图
      clearMap(costmap, x, y);
    }
  }
}


/*
clearMap函数就执行真正的清理工作了，保留机器人周围以reset_distance_为边长的矩形区域，
将其余区域的cell都设置为未知。
*/
void ClearCostmapRecovery::clearMap(boost::shared_ptr<costmap_2d::CostmapLayer> costmap,
                                        double pose_x, double pose_y){
  boost::unique_lock<costmap_2d::Costmap2D::mutex_t> lock(*(costmap->getMutex()));

// 以当前位置为中心　reset_distance_为边长的正方形区域
  double start_point_x = pose_x - reset_distance_ / 2;
  double start_point_y = pose_y - reset_distance_ / 2;
  double end_point_x = start_point_x + reset_distance_;
  double end_point_y = start_point_y + reset_distance_;

 //从世界坐标系转换到地图坐坐标系下，地图没有边界　即像素索引
  int start_x, start_y, end_x, end_y;
  costmap->worldToMapNoBounds(start_point_x, start_point_y, start_x, start_y);   // meter --> pixel
  costmap->worldToMapNoBounds(end_point_x, end_point_y, end_x, end_y);
  // 将特定栅格更新为未知信息
  costmap->clearArea(start_x, start_y, end_x, end_y, invert_area_to_clear_);

  double ox = costmap->getOriginX(), oy = costmap->getOriginY();
  double width = costmap->getSizeInMetersX(), height = costmap->getSizeInMetersY();
  costmap->addExtraBounds(ox, oy, ox + width, oy + height);  // 1e6
  return;
}

};
