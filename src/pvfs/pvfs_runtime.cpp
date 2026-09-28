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

#include "pvfs_runtime.h"

#include "common/logger.h"
#include "pvfs_path.h"

namespace PvfsFileSystem {

PvfsRuntime::PvfsRuntime(const PvfsRuntimeOptions& opts) : opts_(opts) {}

PvfsRuntime::~PvfsRuntime() { shutdown(); }

void PvfsRuntime::shutdown() {
  // The cache deletes its stores on the shared vCPUs, so it goes first; the
  // REST client dies on the vCPU that made it, where its connection pool lives.
  table_cache_.reset();
  if (rest_client_ && init_executor_) {
    init_executor_->perform([&]() { rest_client_.reset(); });
  }
  rest_client_.reset();
}

int PvfsRuntime::init(const std::vector<photon::Executor*>& executors) {
  if (executors.empty()) {
    LOG_ERROR("PVFS runtime needs at least one shared vCPU");
    return -EINVAL;
  }
  init_executor_ = executors[0];
  rest_client_ = std::make_unique<PaimonRESTClient>(opts_.rest);
  int r = init_executor_->perform([&]() {
    int ir = rest_client_->init();
    if (ir != 0) rest_client_.reset();
    return ir;
  });
  if (r != 0) {
    LOG_ERROR("Failed to initialize PaimonRESTClient: `", r);
    return r;
  }
  LOG_INFO("PVFS initialized: catalog=`, endpoint=`, signing=`, ` vCPUs, `",
           opts_.rest.catalog, opts_.rest.endpoint,
           opts_.rest.signing_algorithm, executors.size(),
           opts_.readonly ? "read-only" : "writable");
  LOG_INFO("PVFS external tables: user OSS credential `, endpoint `",
           opts_.cache.static_access_key_id.empty() ? "not configured (token)"
                                                    : "configured",
           opts_.cache.external_oss_endpoint.empty()
               ? "from pvfs_oss_endpoint/token"
               : opts_.cache.external_oss_endpoint);
  table_cache_ = std::make_unique<PvfsTableCache>(rest_client_.get(), opts_.cache);
  table_cache_->attach_shared_vcpus(executors);
  return table_cache_->start();
}

int PvfsRuntime::database_exists(const std::string& database) {
  time_t now = time(nullptr);
  {
    std::lock_guard<std::mutex> lock(db_lock_);
    auto it = db_seen_.find(database);
    if (it != db_seen_.end() && now - it->second < kDatabaseStatTtlSec) {
      return 0;
    }
  }
  int r = rest_client_->get_database(database);
  if (r != 0) return r;
  std::lock_guard<std::mutex> lock(db_lock_);
  db_seen_[database] = now;
  return 0;
}

int PvfsRuntime::check_mutable(const std::string& database,
                               const std::string& table,
                               std::string_view subpath) {
  if (opts_.allow_metadata_write || !is_reserved_first_component(subpath)) {
    return 0;
  }
  while (!subpath.empty() && subpath.front() == '/') subpath.remove_prefix(1);
  std::string name(subpath.substr(0, subpath.find('/')));
  std::string key = database + "/" + table + "|" + name;
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(reserved_warn_lock_);
    if (reserved_warned_.size() < 4096) {
      first = reserved_warned_.insert(key).second;
    }
  }
  if (first) {
    reserved_write_refusals_++;
    LOG_WARN(
        "PVFS `/`: writes under ` are refused, it is Paimon metadata; pvfs_allow_metadata_write overrides",
        database, table, name);
  }
  return -EPERM;
}

}  // namespace PvfsFileSystem
