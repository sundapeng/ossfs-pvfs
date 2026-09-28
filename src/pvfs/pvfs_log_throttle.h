/*
 * Copyright 2025 The Ossfs Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <time.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include "common/logger.h"

namespace PvfsFileSystem {

// One ERROR line per key and window; repeats within the window go to DEBUG.
class PvfsLogThrottle {
 public:
  explicit PvfsLogThrottle(int window_sec = 60, size_t max_keys = 4096)
      : window_sec_(window_sec), max_keys_(max_keys) {}

  // True when `key` has not been allowed within the window ending at `now`.
  bool allow(const std::string& key, time_t now = time(nullptr));
  size_t size();

 private:
  std::mutex lock_;
  std::unordered_map<std::string, time_t> last_;
  int window_sec_;
  size_t max_keys_;
};

// Process-wide instance shared by every PVFS component.
PvfsLogThrottle& pvfs_log_throttle();

}  // namespace PvfsFileSystem

// Repeated failures of one (call, path, status) must not flood the log.
#define PVFS_LOG_ERROR_THROTTLED(key, ...)                          \
  do {                                                              \
    if (::PvfsFileSystem::pvfs_log_throttle().allow(key))           \
      LOG_ERROR(__VA_ARGS__);                                       \
    else                                                            \
      LOG_DEBUG(__VA_ARGS__);                                       \
  } while (0)
