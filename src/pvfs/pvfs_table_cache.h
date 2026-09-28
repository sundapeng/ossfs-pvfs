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

#include <photon/thread/thread.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <photon/common/executor/executor.h>

#include "oss/oss_store.h"
#include "paimon_rest_client.h"

namespace PvfsFileSystem {

struct PvfsTableCacheOptions {
  std::string oss_endpoint;  // override for the endpoint from the token
  // External tables: the user's own OSS credential and endpoint, as the
  // Paimon clients use for isExternal tables; empty means "token anyway".
  std::string static_access_key_id;
  std::string static_access_key_secret;
  std::string external_oss_endpoint;
  int location_cache_ttl_sec = 300;
  int credential_refresh_ahead_sec = 60;
  int cred_refresh_interval_sec = 30;  // background refresh loop period
  int max_table_cache = 200;
  bool enable_ipv6 = false;
  // Client settings every table store starts from; endpoint, bucket and
  // prefix are filled in per table.
  OssFileSystem::ObjStoreOptions store_options;
};

enum class CredMode { kToken, kStatic };

// What a cached table is and how its OSS env is authenticated.
struct TableEntryInfo {
  bool is_external = false;
  std::string table_type;
  CredMode cred_mode = CredMode::kToken;
  std::string bucket;
  std::string prefix;
  std::string endpoint;
};

// Everything a table store is built from and signed with, replaced as a
// whole on refresh and read lock-free by the vCPU slots.
struct TableEnvSnapshot {
  std::string endpoint;
  std::string bucket;
  std::string prefix;
  OssFileSystem::ObjCredentials creds;
  time_t expiration = 0;
  uint64_t generation = 0;
  std::string env_key;  // endpoint|bucket|prefix; a change rebuilds stores
};

// The store of one table on one shared vCPU. Only that vCPU touches the
// slot; build_lock admits one builder at a time, as building a store yields.
struct VCpuStoreSlot {
  std::shared_ptr<OssFileSystem::IObjStore> store;
  uint64_t applied_generation = 0;
  std::string env_key;
  photon::mutex build_lock;
};

struct TableCacheEntry {
  std::string location;
  std::string oss_bucket;
  std::string oss_prefix;
  std::string oss_endpoint;
  bool is_external = false;
  std::string table_type;
  CredMode cred_mode = CredMode::kToken;
  bool announced = false;  // INFO line about type and credentials
  bool warned_external_token = false;
  std::atomic<time_t> credential_expire_time{0};
  std::atomic<uint64_t> credential_generation{0};
  std::atomic<time_t> location_expire_time{0};
  std::atomic<time_t> last_access_time{0};
  std::atomic<time_t> forced_refresh_time{0};  // last 403-driven token fetch
  std::atomic<bool> evicted{false};
  // The current snapshot and one store slot per shared vCPU.
  std::shared_ptr<const TableEnvSnapshot> snapshot;
  std::vector<std::unique_ptr<VCpuStoreSlot>> slots;
  // Per-entry lock: one refresh at a time; Photon-aware so a waiter never
  // blocks the vCPU the holder's REST call completes on.
  photon::mutex refresh_mutex;

  TableCacheEntry() = default;
  TableCacheEntry(const TableCacheEntry&) = delete;
  TableCacheEntry& operator=(const TableCacheEntry&) = delete;
};

// Table locations and temporary credentials from the REST catalog, refreshed
// in the background, with one OssStore per (table, shared vCPU).
class PvfsTableCache {
 public:
  PvfsTableCache(PaimonRESTClient* client, const PvfsTableCacheOptions& opts);
  ~PvfsTableCache();

  // Table stores live on these executors, created lazily on their own vCPU
  // and deleted there. Call before start().
  void attach_shared_vcpus(const std::vector<photon::Executor*>& executors);
  int shared_vcpu_count() const { return static_cast<int>(executors_.size()); }

  // Start the refresh executor and loop; init() of the owner calls it once.
  int start();

  // The entry of a table with fresh location and credentials; on failure
  // *err carries the errno (EIO when unknown).
  std::shared_ptr<TableCacheEntry> resolve(const std::string& database,
                                           const std::string& table,
                                           int* err = nullptr);
  // The table's store on vCPU `vcpu`, created or re-signed in place from
  // the entry's snapshot. Must run on that vCPU.
  std::shared_ptr<OssFileSystem::IObjStore> store_for_vcpu(
      TableCacheEntry& entry, int vcpu);
  // Refresh a cached table now (background loop and tests); a failure
  // keeps the current env.
  int refresh(const std::string& database, const std::string& table);
  // A store answered 403 with the token of `observed_generation`: fetch a
  // new token now, once per kForcedRefreshMinIntervalSec per table. 0 when
  // a newer token is in place (fetched here or meanwhile), -errno otherwise.
  int force_refresh(const std::string& database, const std::string& table,
                    uint64_t observed_generation);
  static constexpr int kForcedRefreshMinIntervalSec = 30;
  uint64_t forced_refreshes() const { return forced_refreshes_.load(); }
  // Credential generation of a cached table, false when not cached.
  bool credential_generation(const std::string& database,
                             const std::string& table, uint64_t* generation);
  // Type, external flag and credential mode of a cached table.
  bool entry_info(const std::string& database, const std::string& table,
                  TableEntryInfo* info);
  // External tables served with a token because no user credential is set.
  uint64_t external_token_warnings() const {
    return external_token_warnings_.load();
  }
  // Stores built so far, and stores deleted on their vCPU so far (eviction,
  // relocation, shutdown); equal once the cache is gone.
  uint64_t stores_created() const { return stores_created_.load(); }
  uint64_t stores_reaped() const { return stores_reaped_.load(); }
  // Calls that reached store_for_vcpu on a vCPU other than the slot's.
  uint64_t misrouted_calls() const { return misrouted_.load(); }
  // Stores built and not yet deleted.
  uint64_t live_stores();
  // Delete every retired store nobody uses any more (the loop does this
  // periodically; tests call it to observe eviction).
  void reap_retired_stores(bool final = false);
  static std::string normalize_oss_endpoint(const std::string& endpoint);
  // Test hook: a nonzero result makes the next refreshes fail with it.
  void set_refresh_fault_hook_for_test(std::function<int()> hook) {
    refresh_fault_hook_ = std::move(hook);
  }

 private:
  struct RetiredStore {
    int vcpu;
    std::shared_ptr<OssFileSystem::IObjStore> store;
  };

  int refresh_entry(const std::string& database, const std::string& table,
                    TableCacheEntry& entry, bool force_token = false);
  void publish_snapshot(const std::string& database, const std::string& table,
                        TableCacheEntry& entry, const TableCredential& cred);
  void announce(const std::string& database, const std::string& table,
                TableCacheEntry& entry);
  OssFileSystem::ObjStoreOptions store_options_for(const std::string& endpoint,
                                                    const std::string& bucket,
                                                    const std::string& prefix);
  static bool ready(TableCacheEntry& entry) {
    return std::atomic_load(&entry.snapshot) != nullptr;
  }
  std::shared_ptr<TableCacheEntry> find_entry(const std::string& key);
  void evict_locked(const std::shared_ptr<TableCacheEntry>& keep,
                    std::vector<std::shared_ptr<TableCacheEntry>>& evicted);
  void release_stores(TableCacheEntry& entry);
  void retire_store(int vcpu, std::shared_ptr<OssFileSystem::IObjStore> store);
  void refresh_loop();

  PaimonRESTClient* rest_client_;  // not owned
  PvfsTableCacheOptions opts_;
  std::vector<photon::Executor*> executors_;  // not owned
  std::vector<photon::vcpu_base*> vcpus_;     // the vCPU of each executor

  std::mutex table_cache_lock_;
  std::unordered_map<std::string, std::shared_ptr<TableCacheEntry>>
      table_cache_;
  std::mutex retired_lock_;
  std::vector<RetiredStore> retired_;
  // Every store built and not yet deleted, by vCPU; swept at shutdown.
  std::unordered_map<OssFileSystem::IObjStore*, int> live_;

  // Background credential refresh runs on its own Photon executor: the
  // REST client and the DNS resolver need a Photon context.
  photon::Executor* refresh_executor_ = nullptr;
  std::thread* cred_refresh_th_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::mutex stop_lock_;  // wakes the loop's sleep at shutdown
  std::condition_variable stop_cv_;
  std::atomic<uint64_t> external_token_warnings_{0};
  std::atomic<uint64_t> stores_reaped_{0};
  std::atomic<uint64_t> stores_created_{0};
  std::atomic<uint64_t> misrouted_{0};
  std::atomic<uint64_t> forced_refreshes_{0};
  std::function<int()> refresh_fault_hook_;
};

}  // namespace PvfsFileSystem
