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

#include "pvfs_log_throttle.h"

namespace PvfsFileSystem {

bool PvfsLogThrottle::allow(const std::string& key, time_t now) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = last_.find(key);
  if (it != last_.end()) {
    if (now - it->second < window_sec_) return false;
    it->second = now;
    return true;
  }
  if (last_.size() >= max_keys_) {
    // Drop keys outside the window first; a table full of live keys is
    // reset rather than grown.
    for (auto e = last_.begin(); e != last_.end();) {
      if (now - e->second >= window_sec_) e = last_.erase(e);
      else ++e;
    }
    if (last_.size() >= max_keys_) last_.clear();
  }
  last_.emplace(key, now);
  return true;
}

size_t PvfsLogThrottle::size() {
  std::lock_guard<std::mutex> lock(lock_);
  return last_.size();
}

PvfsLogThrottle& pvfs_log_throttle() {
  static PvfsLogThrottle instance;
  return instance;
}

}  // namespace PvfsFileSystem
