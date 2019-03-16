/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "LogPlayback.hh"

#include <ignition/msgs/pose_v.pb.h>

#include <string>

#include <ignition/msgs/Utility.hh>

#include <ignition/math/Pose3.hh>

#include <ignition/plugin/RegisterMore.hh>

#include <ignition/common/Filesystem.hh>
#include <ignition/common/Time.hh>
#include <ignition/transport/log/QueryOptions.hh>
#include <ignition/transport/log/Log.hh>
#include <ignition/transport/log/Message.hh>

#include <sdf/Root.hh>

#include "ignition/gazebo/Conversions.hh"
#include "ignition/gazebo/Events.hh"
#include "ignition/gazebo/SdfEntityCreator.hh"
#include "ignition/gazebo/components/Pose.hh"


using namespace ignition;
using namespace gazebo;
using namespace systems;

/// \brief Private LogPlayback data class.
class ignition::gazebo::systems::LogPlaybackPrivate
{
  /// \brief Reads the next PoseV message in log file and updates the ECM
  /// \param[in] _ecm Mutable ECM.
  public: void ParsePoseV(EntityComponentManager &_ecm);

  /// \brief Reads the next SerializedState message in log file and updates the
  /// ECM
  /// \param[in] _ecm Mutable ECM.
  public: void ParseSerializedState(EntityComponentManager &_ecm);

  /// \brief A batch of data from log file, of all pose messages
  public: transport::log::Batch batch;

  /// \brief Iterator to go through messages in Batch
  public: transport::log::MsgIter iter;

  /// \brief Flag to print finish message once
  public: bool printedEnd{false};
};


//////////////////////////////////////////////////
LogPlayback::LogPlayback()
  : System(), dataPtr(std::make_unique<LogPlaybackPrivate>())
{
}

//////////////////////////////////////////////////
LogPlayback::~LogPlayback() = default;

//////////////////////////////////////////////////
void LogPlaybackPrivate::ParsePoseV(EntityComponentManager &_ecm)
{
  // Protobuf message
  msgs::Pose_V posevMsg;

  // Convert binary bytes in string into a ign-msgs msg
  posevMsg.ParseFromString(this->iter->Data());

  // Maps entity to pose recorded
  // Key: entity. Value: pose
  std::map <Entity, msgs::Pose> idToPose;

  for (int i = 0; i < posevMsg.pose_size(); ++i)
  {
    msgs::Pose pose = posevMsg.pose(i);

    // Update entity pose in map
    idToPose.insert_or_assign(pose.id(), pose);
  }

  // Loop through entities in world
  _ecm.Each<components::Pose>(
      [&](const Entity &_entity, components::Pose *_poseComp) -> bool
  {
    // Check if we have an updated pose for this entity
    if (idToPose.find(_entity) == idToPose.end())
      return true;

    // Look for pose in log entry loaded
    msgs::Pose pose = idToPose.at(_entity);

    // Set current pose to recorded pose
    // Use copy assignment operator
    *_poseComp = components::Pose(msgs::Convert(pose));

    return true;
  });
}

//////////////////////////////////////////////////
void LogPlaybackPrivate::ParseSerializedState(EntityComponentManager &_ecm)
{
  msgs::SerializedState msg;
  msg.ParseFromString(this->iter->Data());

  // TODO(anyone) Support setting only user-selected components
  _ecm.SetState(msg);
}

//////////////////////////////////////////////////
void LogPlayback::Configure(const Entity &_worldEntity,
    const std::shared_ptr<const sdf::Element> &_sdf,
    EntityComponentManager &_ecm, EventManager &_eventMgr)
{
  // Get directory paths from SDF
  auto logPath = _sdf->Get<std::string>("path");

  if (logPath.empty())
  {
    ignerr << "Unspecified log path to playback. Nothing to play.\n";
    return;
  }

  if (!common::isDirectory(logPath))
  {
    ignerr << "Specified log path [" << logPath << "] must be a directory.\n";
    return;
  }

  // Append file name
  std::string dbPath = common::joinPaths(logPath, "state.tlog");

  // Temporary. Name of recorded SDF file
  std::string sdfPath = common::joinPaths(logPath, "state.sdf");

  if (!common::exists(dbPath) ||
      !common::exists(sdfPath))
  {
    ignerr << "Log path invalid. File(s) [" << dbPath << "] / [" << sdfPath
           << "] do not exist. Nothing to play.\n";
    return;
  }

  ignmsg << "Loading log files:"  << std::endl
         << "* " << dbPath << std::endl
         << "* " << sdfPath << std::endl;

  // Load recorded SDF file
  sdf::Root root;
  if (root.Load(sdfPath).size() != 0 || root.WorldCount() <= 0)
  {
    ignerr << "Error loading SDF file [" << sdfPath << "]" << std::endl;
    return;
  }
  const sdf::World *sdfWorld = root.WorldByIndex(0);

  std::vector <sdf::ElementPtr> pluginsRm;

  // Look for LogRecord plugin in the SDF and remove it, so that playback
  //   is not re-recorded.
  // Remove Physics plugin, so that it does not clash with recorded poses.
  // TODO(anyone) Cherry-picking plugins to remove doesn't scale well,
  // handle this better once we're logging the initial world state in the DB
  // file.
  if (sdfWorld->Element()->HasElement("plugin"))
  {
    sdf::ElementPtr pluginElt = sdfWorld->Element()->GetElement("plugin");

    // If never found, nothing to remove
    while (pluginElt != nullptr)
    {
      if (pluginElt->HasAttribute("name"))
      {
        if ((pluginElt->GetAttribute("name")->GetAsString().find("LogRecord")
          != std::string::npos) ||
          (pluginElt->GetAttribute("name")->GetAsString().find("Physics")
          != std::string::npos))
        {
          // Flag for removal.
          // Do not actually remove plugin from parent while looping through
          //   children of this parent. Else cannot access next element.
          pluginsRm.push_back(pluginElt);
        }
      }

      // Go to next plugin
      pluginElt = pluginElt->GetNextElement("plugin");
    }
  }

  // Remove the marked plugins
  for (auto &it : pluginsRm)
  {
    it->RemoveFromParent();
    igndbg << "Removed " << it->GetAttribute("name")->GetAsString()
      << " plugin from loaded SDF\n";
  }

  // Create all Entities in SDF <world> tag
  ignition::gazebo::SdfEntityCreator creator =
    ignition::gazebo::SdfEntityCreator(_ecm, _eventMgr);

  // Models
  for (uint64_t modelIndex = 0; modelIndex < sdfWorld->ModelCount();
      ++modelIndex)
  {
    auto model = sdfWorld->ModelByIndex(modelIndex);
    auto modelEntity = creator.CreateEntities(model);

    creator.SetParent(modelEntity, _worldEntity);
  }

  // Lights
  for (uint64_t lightIndex = 0; lightIndex < sdfWorld->LightCount();
      ++lightIndex)
  {
    auto light = sdfWorld->LightByIndex(lightIndex);
    auto lightEntity = creator.CreateEntities(light);

    creator.SetParent(lightEntity, _worldEntity);
  }

  _eventMgr.Emit<events::LoadPlugins>(_worldEntity, sdfWorld->Element());

  // Call Log.hh directly to load a .tlog file
  auto log = std::make_unique<transport::log::Log>();
  if (!log->Open(dbPath))
  {
    ignerr << "Failed to open log file [" << dbPath << "]" << std::endl;
  }

  // Access all messages in .tlog file
  this->dataPtr->batch = log->QueryMessages();
  this->dataPtr->iter = this->dataPtr->batch.begin();

  if (this->dataPtr->iter == this->dataPtr->batch.end())
  {
    ignerr << "No messages found in log file [" << dbPath << "]" << std::endl;
  }
}

//////////////////////////////////////////////////
void LogPlayback::Update(const UpdateInfo &_info, EntityComponentManager &_ecm)
{
  if (_info.paused)
    return;

  // TODO(anyone) Support rewind
  // Sanity check. If reached the end, done.
  if (this->dataPtr->iter == this->dataPtr->batch.end())
  {
    // Print only once
    if (!this->dataPtr->printedEnd)
    {
      ignmsg << "Finished playing all recorded data\n";
      this->dataPtr->printedEnd = true;
    }
    return;
  }

  // Sim time stamp from /clock topic
  auto msgStamp = this->dataPtr->iter->TimeReceived();

  if (_info.simTime < msgStamp)
    return;

  auto msgType = this->dataPtr->iter->Type();

  // TODO(anyone) Support multiple msgs per update, in case playback has a lower
  // frequency than record - using transport::log::TimeRangeOption should help.
  if (msgType == "ignition.msgs.Pose_V")
  {
    this->dataPtr->ParsePoseV(_ecm);
  }
  else if (msgType == "ignition.msgs.SerializedState")
  {
    this->dataPtr->ParseSerializedState(_ecm);
  }
  else
  {
    ignwarn << "Trying to playback unsupported message type ["
            << msgType << "]" << std::endl;
  }

  ++(this->dataPtr->iter);
}

IGNITION_ADD_PLUGIN(ignition::gazebo::systems::LogPlayback,
                    ignition::gazebo::System,
                    LogPlayback::ISystemConfigure,
                    LogPlayback::ISystemUpdate)

IGNITION_ADD_PLUGIN_ALIAS(ignition::gazebo::systems::LogPlayback,
                          "ignition::gazebo::systems::LogPlayback")