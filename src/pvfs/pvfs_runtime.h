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

#include <photon/common/executor/executor.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "paimon_rest_client.h"
#include "pvfs_table_cache.h"

namespace PvfsFileSystem {

struct PvfsRuntimeOptions {
  PaimonRESTClientOptions rest;
  PvfsTableCacheOptions cache;
  bool readonly = true;  // writes are opt-in (--pvfs_allow_write)
  bool allow_metadata_write = false;  // mutations under snapshot/, manifest/...
  std::string prefix;  // mounted sub-tree "db[/table[/dir]]", no slashes
};

// The catalog side of a PVFS mount shared by every PvfsObjStore: the REST
// client, the table cache and the database existence cache.
class PvfsRuntime {
 public:
  explicit PvfsRuntime(const PvfsRuntimeOptions& opts);
  ~PvfsRuntime();

  // Initializes the REST client on executors[0] and hands the table cache
  // the shared vCPUs; the executors outlive this object.
  int init(const std::vector<photon::Executor*>& executors);
  // Deletes the table stores on their vCPUs, then the REST client on its
  // own; safe to call more than once, before the executors go away.
  void shutdown();

  PaimonRESTClient* rest_client() { return rest_client_.get(); }
  PvfsTableCache* table_cache() { return table_cache_.get(); }
  const PvfsRuntimeOptions& options() const { return opts_; }

  // 0 when the database exists (cached for a short while), -ENOENT or the
  // REST errno otherwise. Must run in a Photon context.
  int database_exists(const std::string& database);

  // -EPERM when a mutation of table-relative `subpath` would touch a
  // reserved Paimon directory (WARN once per table and name); 0 otherwise.
  int check_mutable(const std::string& database, const std::string& table,
                    std::string_view subpath);
  // Mutations refused under a reserved directory, one per (table, name).
  uint64_t reserved_write_refusals() const {
    return reserved_write_refusals_.load();
  }
  // Object stats (HEAD and directory probes) the stores issued, for tests.
  void count_stat() { stat_calls_++; }
  uint64_t oss_stat_calls() const { return stat_calls_.load(); }

  static constexpr int kDatabaseStatTtlSec = 30;

 private:
  PvfsRuntimeOptions opts_;
  photon::Executor* init_executor_ = nullptr;  // the REST client's vCPU
  std::unique_ptr<PaimonRESTClient> rest_client_;
  std::unique_ptr<PvfsTableCache> table_cache_;
  std::mutex db_lock_;
  std::unordered_map<std::string, time_t> db_seen_;
  // Reserved-directory refusals already logged, keyed "db/table|name".
  std::mutex reserved_warn_lock_;
  std::unordered_set<std::string> reserved_warned_;
  std::atomic<uint64_t> reserved_write_refusals_{0};
  std::atomic<uint64_t> stat_calls_{0};
};

}  // namespace PvfsFileSystem
