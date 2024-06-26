/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "host/commands/cvd/selector/instance_group_record.h"

#include <set>

#include <android-base/parseint.h>

#include "common/libs/utils/result.h"
#include "host/commands/cvd/selector/instance_database_types.h"
#include "host/commands/cvd/selector/instance_database_utils.h"
#include "host/commands/cvd/selector/selector_constants.h"
#include "host/commands/cvd/selector/instance_record.h"

namespace cuttlefish {
namespace selector {

namespace {

static constexpr const char kJsonGroupName[] = "Group Name";
static constexpr const char kJsonHomeDir[] = "Runtime/Home Dir";
static constexpr const char kJsonHostArtifactPath[] = "Host Tools Dir";
static constexpr const char kJsonProductOutPath[] = "Product Out Dir";
static constexpr const char kJsonStartTime[] = "Start Time";
static constexpr const char kJsonInstances[] = "Instances";
static constexpr const char kJsonParent[] = "Parent Group";

std::vector<LocalInstance> Filter(
    const std::vector<LocalInstance>& instances,
    std::function<bool(const LocalInstance&)> predicate) {
  std::vector<LocalInstance> ret;
  std::copy_if(instances.begin(), instances.end(), std::back_inserter(ret),
               predicate);
  return ret;
}

}  // namespace

Result<LocalInstanceGroup> LocalInstanceGroup::Create(
    const cvd::InstanceGroup& group_proto) {
  CF_EXPECT(!group_proto.instances().empty(), "New group can't be empty");
  std::vector<LocalInstance> instances;
  std::set<unsigned> ids;
  std::set<std::string> names;

  for (const auto& instance_proto : group_proto.instances()) {
    instances.emplace_back(group_proto, instance_proto);
    ids.insert(instance_proto.id());
    names.insert(instance_proto.name());
  }
  CF_EXPECT(ids.size() == group_proto.instances_size(),
            "Instances must have unique ids");
  CF_EXPECT(names.size() == group_proto.instances_size(),
            "Instances must have unique names");
  return LocalInstanceGroup(group_proto, instances);
}

TimeStamp LocalInstanceGroup::StartTime() const {
  return CvdServerClock::from_time_t(group_proto_.start_time_sec());
}

LocalInstanceGroup::LocalInstanceGroup(
    const cvd::InstanceGroup& group_proto, const std::vector<LocalInstance>& instances)
    : internal_group_name_(GenInternalGroupName()),
      group_proto_(group_proto),
      instances_(instances) {};

std::vector<LocalInstance> LocalInstanceGroup::FindById(
    const unsigned id) const {
  return Filter(instances_, [id](const LocalInstance& instance) {
    return id == instance.InstanceId();
  });
}

std::vector<LocalInstance> LocalInstanceGroup::FindByInstanceName(
    const std::string& instance_name) const {
  return Filter(instances_, [instance_name](const LocalInstance& instance) {
    return instance.PerInstanceName() == instance_name;
  });
}

Result<LocalInstanceGroup> LocalInstanceGroup::Deserialize(
    const Json::Value& group_json) {
  CF_EXPECT(group_json.isMember(kJsonGroupName));
  const std::string group_name = group_json[kJsonGroupName].asString();
  CF_EXPECT(group_json.isMember(kJsonHomeDir));
  const std::string home_dir = group_json[kJsonHomeDir].asString();
  CF_EXPECT(group_json.isMember(kJsonHostArtifactPath));
  const std::string host_artifacts_path =
      group_json[kJsonHostArtifactPath].asString();
  CF_EXPECT(group_json.isMember(kJsonProductOutPath));
  const std::string product_out_path =
      group_json[kJsonProductOutPath].asString();
  TimeStamp start_time = CvdServerClock::now();

  // test if the field is available as the field has been added
  // recently as of b/315855286
  if (group_json.isMember(kJsonStartTime)) {
    auto restored_start_time_result =
        DeserializeTimePoint(group_json[kJsonStartTime]);
    if (restored_start_time_result.ok()) {
      start_time = std::move(*restored_start_time_result);
    } else {
      LOG(ERROR) << "Start time restoration from json failed, so we use "
                 << " the current system time. Reasons: "
                 << restored_start_time_result.error().FormatForEnv();
    }
  }

  cvd::InstanceGroup group_proto;
  group_proto.set_name(group_name);
  group_proto.set_home_directory(home_dir);
  group_proto.set_host_artifacts_path(host_artifacts_path);
  group_proto.set_product_out_path(product_out_path);
  group_proto.set_start_time_sec(CvdServerClock::to_time_t(start_time));

  CF_EXPECT(group_json.isMember(kJsonInstances));
  const Json::Value& instances_json_array = group_json[kJsonInstances];
  CF_EXPECT(instances_json_array.isArray());
  for (int i = 0; i < instances_json_array.size(); i++) {
    const Json::Value& instance_json = instances_json_array[i];
    CF_EXPECT(instance_json.isMember(LocalInstance::kJsonInstanceName));
    const std::string instance_name =
        instance_json[LocalInstance::kJsonInstanceName].asString();
    CF_EXPECT(instance_json.isMember(LocalInstance::kJsonInstanceId));
    const std::string instance_id =
        instance_json[LocalInstance::kJsonInstanceId].asString();

    int id;
    CF_EXPECTF(android::base::ParseInt(instance_id, std::addressof(id)),
               "Invalid instance ID in instance json: {}", instance_id);
    auto instance = group_proto.add_instances();
    instance->set_id(id);
    instance->set_name(instance_name);
  }

  return Create(group_proto);
}

}  // namespace selector
}  // namespace cuttlefish
