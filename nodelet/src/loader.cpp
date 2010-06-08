/*
 * Copyright (c) 2010, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <nodelet/loader.h>
#include <nodelet/nodelet.h>
#include <nodelet/detail/callback_queue_manager.h>
#include <pluginlib/class_loader.h>

#include <ros/ros.h>
#include <nodelet/NodeletLoad.h>
#include <nodelet/NodeletList.h>
#include <nodelet/NodeletUnload.h>

#include <sstream>
#include <map>
#include <boost/shared_ptr.hpp>

namespace nodelet
{

namespace detail
{
class LoaderROS
{
public:
  LoaderROS(Loader* parent, const ros::NodeHandle& nh)
  : parent_(parent)
  , nh_(nh)
  {
    load_server_ = nh_.advertiseService("load_nodelet", &LoaderROS::serviceLoad, this);
    unload_server_ = nh_.advertiseService("unload_nodelet", &LoaderROS::serviceUnload, this);
    list_server_ = nh_.advertiseService("list", &LoaderROS::serviceList, this);
  }

private:
  bool serviceLoad(nodelet::NodeletLoad::Request &req,
                                  nodelet::NodeletLoad::Response &res)
  {
    // build map
    M_string remappings;
    if (req.remap_source_args.size() != req.remap_target_args.size())
    {
      ROS_ERROR("Bad remapppings provided, target and source of different length");
    }
    else
    {
      //      std::cerr<< "remapping";
      for (size_t i = 0; i < req.remap_source_args.size(); ++i)
      {
        //std::cerr<< req.remap_source_args[i] << ":=" << req.remap_target_args[i] << std::endl;
        remappings[ros::names::resolve(req.remap_source_args[i])] = ros::names::resolve(req.remap_target_args[i]);
        ROS_DEBUG("%s:%s\n", ros::names::resolve(req.remap_source_args[i]).c_str(), remappings[ros::names::resolve(req.remap_source_args[i])].c_str());
      }
    }
    res.success = parent_->load(req.name, req.type, remappings, req.my_argv);
    return res.success;
  }

  bool serviceUnload(nodelet::NodeletUnload::Request &req,
                                    nodelet::NodeletUnload::Response &res)
  {
    res.success = parent_->unload(req.name);
    if (!res.success)
    {
      ROS_ERROR("Failed to find nodelet with name '%s' to unload.", req.name.c_str());
    }

    return res.success;
  }

  bool serviceList(nodelet::NodeletList::Request &req,
                   nodelet::NodeletList::Response &res)
  {

    res.nodelets = parent_->listLoadedNodelets();
    return true;
  }

  Loader* parent_;
  ros::NodeHandle nh_;
  ros::ServiceServer load_server_;
  ros::ServiceServer unload_server_;
  ros::ServiceServer list_server_;
};
} // namespace detail

/** \brief Create the filter chain object */
Loader::Loader(bool provide_ros_api)
: loader_(new pluginlib::ClassLoader<Nodelet>("nodelet", "nodelet::Nodelet"))
{
  std::string lib_string = "";
  std::vector<std::string> libs = loader_->getDeclaredClasses();
  for (size_t i = 0 ; i < libs.size(); ++i)
  {
    lib_string = lib_string + std::string(", ") + libs[i];
  }

  if (provide_ros_api)
  {
    ros::NodeHandle server_nh("~");
    services_.reset(new detail::LoaderROS(this, server_nh));
    ROS_DEBUG("In FilterChain ClassLoader found the following libs: %s", lib_string.c_str());
    
    int num_threads_param;
    if (server_nh.getParam ("num_worker_threads", num_threads_param))
    {
      callback_manager_ = detail::CallbackQueueManagerPtr (new detail::CallbackQueueManager (num_threads_param));
      ROS_INFO("Initializing nodelet with %d worker threads.", num_threads_param);
    }
  }
  if (!callback_manager_)
    callback_manager_ = detail::CallbackQueueManagerPtr (new detail::CallbackQueueManager);
}

Loader::~Loader()
{
  clear();

}

bool Loader::load(const std::string &name, const std::string& type, const ros::M_string& remappings, const std::vector<std::string> & my_argv)
{
  if (nodelets_.count(name) > 0)
  {
    ROS_ERROR("Cannot load nodelet %s for one exists with that name already", name.c_str());
    return false;
  }

  //\TODO store type in string format too, or provide accessors from pluginlib
  try
  {
    NodeletPtr p(loader_->createClassInstance(type));
    if (!p)
    {
      return false;
    }

    nodelets_[name] = p;
    ROS_DEBUG("Done loading nodelet %s", name.c_str());

    p->init(name, remappings, my_argv, callback_manager_.get());
    ROS_DEBUG("Done initing nodelet %s", name.c_str());
    return true;
  }
  catch (pluginlib::PluginlibException& e)
  {
    ROS_ERROR("Failed to load nodelet [%s] of type [%s]: %s", name.c_str(), type.c_str(), e.what());
  }

  return false;
}

bool Loader::unload(const std::string & name)
{
  M_stringToNodelet::iterator it = nodelets_.find(name);
  if (it != nodelets_.end())
  {
    nodelets_.erase(it);
    ROS_DEBUG("Done unloading nodelet %s", name.c_str());
    return true;
  }

  return false;
}

/** \brief Clear all nodelets from this chain */
bool Loader::clear()
{
  nodelets_.clear();
  return true;
};

/**\brief List the names of all loaded nodelets */
std::vector<std::string> Loader::listLoadedNodelets()
{
  std::vector<std::string> output;
  std::map< std::string, boost::shared_ptr<Nodelet> >::iterator it = nodelets_.begin();
  for (; it != nodelets_.end(); ++it)
  {
    output.push_back(it->first);
  }
  return output;
}

} // namespace nodelet
