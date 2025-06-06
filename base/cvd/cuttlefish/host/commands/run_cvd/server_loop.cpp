/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "cuttlefish/host/commands/run_cvd/server_loop.h"

#include <fruit/fruit.h>

#include "cuttlefish/host/commands/run_cvd/launch/webrtc_controller.h"
#include "cuttlefish/host/commands/run_cvd/server_loop_impl.h"
#include "cuttlefish/host/libs/config/cuttlefish_config.h"
#include "cuttlefish/host/libs/feature/feature.h"
#include "cuttlefish/host/libs/feature/inject.h"

namespace cuttlefish {

ServerLoop::~ServerLoop() = default;

fruit::Component<
    fruit::Required<const CuttlefishConfig,
                    const CuttlefishConfig::InstanceSpecific,
                    AutoSnapshotControlFiles::Type, WebRtcController>,
    ServerLoop>
serverLoopComponent() {
  using run_cvd_impl::ServerLoopImpl;
  return fruit::createComponent()
      .bind<ServerLoop, ServerLoopImpl>()
      .addMultibinding<LateInjected, ServerLoopImpl>()
      .addMultibinding<SetupFeature, ServerLoopImpl>();
}

}  // namespace cuttlefish
