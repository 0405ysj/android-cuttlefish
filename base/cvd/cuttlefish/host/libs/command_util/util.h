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

#pragma once

#include <optional>

#include "common/libs/fs/shared_fd.h"
#include "common/libs/utils/result.h"
#include "host/libs/command_util/runner/defs.h"

namespace cuttlefish {

Result<RunnerExitCodes> ReadExitCode(SharedFD monitor_socket);
Result<void> WaitForRead(SharedFD monitor_socket, int timeout_seconds);

// Writes the `action` request to `monitor_socket`, then waits for the response
// and checks for errors.
Result<void> RunLauncherAction(SharedFD monitor_socket, LauncherAction action,
                               std::optional<int> timeout_seconds);

}  // namespace cuttlefish
