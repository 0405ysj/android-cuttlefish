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

#include "cuttlefish/host/commands/cvd/acloud/config.h"

#include <fstream>
#include <sstream>
#include <string>

#include <google/protobuf/text_format.h>

#include "cuttlefish/common/libs/utils/files.h"
#include "cuttlefish/common/libs/utils/users.h"

namespace cuttlefish {

AcloudConfig::AcloudConfig(const acloud::UserConfig& usr_cfg)
    : launch_args(usr_cfg.launch_args()),
      project(usr_cfg.project()),
      zone(usr_cfg.zone()),
      use_legacy_acloud(usr_cfg.use_legacy_acloud()) {
  // TODO(weihsu): Add back fields/variables (except of cheeps and emulator
  // fields) in config files. Remove cheeps (Android on ChromeOS) and emulator
  // fields.

  // TODO(weihsu): Verify validity of configurations.
}

template <typename ProtoType>
Result<ProtoType> ParseTextProtoConfigHelper(const std::string& config_path) {
  std::ifstream t(config_path);
  std::stringstream buffer;
  buffer << t.rdbuf();

  ProtoType proto_result;
  google::protobuf::TextFormat::Parser p;
  CF_EXPECT(p.ParseFromString(buffer.str(), &proto_result),
            "Failed to parse config: " << config_path);
  return proto_result;
}

/**
 * Return path to default config file.
 */
Result<const std::string> GetDefaultConfigFile() {
  const std::string home = CF_EXPECT(SystemWideUserHome());
  return (std::string(home) + "/.config/acloud/acloud.config");
}

Result<AcloudConfig> LoadAcloudConfig(const std::string& user_config_path) {
  acloud::UserConfig proto_result_user;
  if (FileExists(user_config_path)) {
    proto_result_user = CF_EXPECT(
        ParseTextProtoConfigHelper<acloud::UserConfig>(user_config_path));
  } else {
    const std::string conf_path = CF_EXPECT(GetDefaultConfigFile());
    CF_EXPECT(user_config_path == conf_path,
              "The specified config file does not exist.");

    // If the default config does not exist, acloud creates an empty object.
    proto_result_user = acloud::UserConfig();
  }
  return AcloudConfig(proto_result_user);
}

}  // namespace cuttlefish
