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

#include "pvfs_table_cache.h"

#include <chrono>
#include <limits>

#include "common/logger.h"
#include "common/macros.h"
#include "common/utils.h"

namespace PvfsFileSystem {

using OssFileSystem::CredsMeta;
using OssFileSystem::IObjStore;
using OssFileSystem::ObjCredentials;
using OssFileSystem::ObjStoreOptions;

static void parse_oss_location(const std::string& location,
                                std::string& bucket, std::string& prefix) {
  std::string_view loc = location;
  if (loc.substr(0, 6) == "oss://") loc.remove_prefix(6);
  auto slash = loc.find('/');
  if (slash == std::string_view::npos) {
    bucket = std::string(loc);
    prefix = "";
  } else {
    bucket = std::string(loc.substr(0, slash));
    prefix = std::string(loc.substr(slash + 1));
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  }
}

// The data plane carries table data with STS credentials: TLS unless the
// endpoint (from the option or the token) explicitly says http://.
std::string PvfsTableCache::normalize_oss_endpoint(const std::string& endpoint) {
  auto [host, https] = PaimonRESTClient::split_scheme(endpoint, true);
  return (https ? "https://" : "http://") + host;
}

PvfsTableCache::PvfsTableCache(PaimonRESTClient* client,
                               const PvfsTableCacheOptions& opts)
    : rest_client_(client), opts_(opts) {}

PvfsTableCache::~PvfsTableCache() {
  {
    std::lock_guard<std::mutex> lock(stop_lock_);
    stopping_ = true;
  }
  stop_cv_.notify_all();
  if (cred_refresh_th_) {
    cred_refresh_th_->join();
    delete cred_refresh_th_;
  }
  std::vector<std::shared_ptr<TableCacheEntry>> entries;
  {
    std::lock_guard<std::mutex> lock(table_cache_lock_);
    for (auto& [key, entry] : table_cache_) entries.push_back(entry);
    table_cache_.clear();
  }
  for (auto& entry : entries) release_stores(*entry);
  reap_retired_stores(true);
  // Whatever the slots lost still dies on its vCPU, or fini() waits forever.
  std::unordered_map<IObjStore*, int> leftover;
  {
    std::lock_guard<std::mutex> lock(retired_lock_);
    leftover.swap(live_);
  }
  for (auto& [raw, vcpu] : leftover) {
    LOG_WARN("PVFS store on vCPU ` was not in any slot; deleting it", vcpu);
    executors_[vcpu]->perform([&]() { delete raw; });
    stores_reaped_++;
  }
  delete refresh_executor_;
}

void PvfsTableCache::attach_shared_vcpus(
    const std::vector<photon::Executor*>& executors) {
  executors_ = executors;
  vcpus_.clear();
  for (auto* e : executors_) {
    vcpus_.push_back(e->perform([]() { return photon::get_vcpu(); }));
  }
}

int PvfsTableCache::start() {
  if (executors_.empty()) {
    LOG_ERROR("PVFS table cache needs the shared vCPUs before start()");
    return -EINVAL;
  }
  {
    ScopedBlockAllSignal block_signals;
    refresh_executor_ = new photon::Executor(
        OSSFS_EVENT_ENGINE, photon::INIT_IO_NONE, {}, {16, 1024});
  }
  cred_refresh_th_ = new std::thread([this]() { refresh_loop(); });
  return 0;
}

void PvfsTableCache::refresh_loop() {
  while (!stopping_.load()) {
    for (int i = 0; i < opts_.cred_refresh_interval_sec && !stopping_.load();
         i++) {
      std::unique_lock<std::mutex> lock(stop_lock_);
      stop_cv_.wait_for(lock, std::chrono::seconds(1),
                        [&]() { return stopping_.load(); });
      lock.unlock();
      if (!stopping_.load()) reap_retired_stores();
    }
    if (stopping_.load()) break;

    // Collect table keys that need refresh
    std::vector<std::pair<std::string, std::string>> to_refresh;
    {
      std::lock_guard<std::mutex> lock(table_cache_lock_);
      time_t now = time(nullptr);
      for (auto& [key, entry] : table_cache_) {
        if (!ready(*entry) || entry->cred_mode == CredMode::kStatic) continue;
        if (now + opts_.credential_refresh_ahead_sec >=
            entry->credential_expire_time) {
          auto slash = key.find('/');
          if (slash != std::string::npos) {
            to_refresh.emplace_back(key.substr(0, slash),
                                     key.substr(slash + 1));
          }
        }
      }
    }

    for (auto& [db, tbl] : to_refresh) {
      if (stopping_.load()) break;
      refresh_executor_->perform([&]() { return refresh(db, tbl); });
    }
  }
}

std::shared_ptr<TableCacheEntry> PvfsTableCache::find_entry(
    const std::string& key) {
  std::lock_guard<std::mutex> lock(table_cache_lock_);
  auto it = table_cache_.find(key);
  return it == table_cache_.end() ? nullptr : it->second;
}

int PvfsTableCache::refresh(const std::string& database,
                          const std::string& table) {
  auto entry = find_entry(database + "/" + table);
  if (!entry) return -ENOENT;
  photon::scoped_lock entry_lock(entry->refresh_mutex);
  int r = refresh_entry(database, table, *entry);
  if (r != 0) {
    // Keep serving with the current token while it is still valid; the
    // loop retries before it expires.
    LOG_WARN("PVFS refresh of `/` failed (`), keeping current token", database,
             table, r);
  }
  return r;
}

int PvfsTableCache::force_refresh(const std::string& database,
                                  const std::string& table,
                                  uint64_t observed_generation) {
  auto entry = find_entry(database + "/" + table);
  if (!entry) return -ENOENT;
  photon::scoped_lock entry_lock(entry->refresh_mutex);
  if (entry->credential_generation.load() != observed_generation) return 0;
  if (entry->cred_mode == CredMode::kStatic) return -EACCES;
  time_t now = time(nullptr);
  if (now - entry->forced_refresh_time.load() < kForcedRefreshMinIntervalSec) {
    return -EAGAIN;
  }
  entry->forced_refresh_time = now;
  forced_refreshes_++;
  LOG_WARN("PVFS `/`: OSS refused the token, fetching a new one", database,
           table);
  return refresh_entry(database, table, *entry, true);
}

bool PvfsTableCache::credential_generation(const std::string& database,
                                         const std::string& table,
                                         uint64_t* generation) {
  std::lock_guard<std::mutex> lock(table_cache_lock_);
  auto it = table_cache_.find(database + "/" + table);
  if (it == table_cache_.end()) return false;
  *generation = it->second->credential_generation.load();
  return true;
}

bool PvfsTableCache::entry_info(const std::string& database,
                                const std::string& table,
                                TableEntryInfo* info) {
  auto entry = find_entry(database + "/" + table);
  if (!entry) return false;
  photon::scoped_lock entry_lock(entry->refresh_mutex);
  info->is_external = entry->is_external;
  info->table_type = entry->table_type;
  info->cred_mode = entry->cred_mode;
  info->bucket = entry->oss_bucket;
  info->prefix = entry->oss_prefix;
  info->endpoint = entry->oss_endpoint;
  return true;
}

ObjStoreOptions PvfsTableCache::store_options_for(const std::string& endpoint,
                                                  const std::string& bucket,
                                                  const std::string& prefix) {
  ObjStoreOptions oss_opts = opts_.store_options;
  oss_opts.endpoint = endpoint;
  oss_opts.bucket = bucket;
  oss_opts.prefix = prefix;
  if (oss_opts.user_agent == ObjStoreOptions().user_agent) {
    oss_opts.user_agent = "ossfs2-pvfs";
  }
  oss_opts.ip_version = OssFileSystem::ip_version_for(opts_.enable_ipv6);
  return oss_opts;
}

void PvfsTableCache::announce(const std::string& database,
                              const std::string& table,
                              TableCacheEntry& entry) {
  if (entry.announced) return;
  entry.announced = true;
  LOG_INFO("PVFS table `/`: type=` external=` credentials=` bucket=` prefix=`",
           database, table,
           entry.table_type.empty() ? "paimon" : entry.table_type,
           entry.is_external,
           entry.cred_mode == CredMode::kStatic ? "user" : "token",
           entry.oss_bucket, entry.oss_prefix);
}

// The slots pick the new snapshot up on their own vCPU.
void PvfsTableCache::publish_snapshot(const std::string& database,
                                      const std::string& table,
                                      TableCacheEntry& entry,
                                      const TableCredential& cred) {
  auto snap = std::make_shared<TableEnvSnapshot>();
  snap->endpoint = entry.oss_endpoint;
  snap->bucket = entry.oss_bucket;
  snap->prefix = entry.oss_prefix;
  snap->creds = ObjCredentials{cred.access_key_id, cred.access_key_secret,
                               cred.security_token};
  snap->expiration = cred.expiration_sec;
  snap->generation = entry.credential_generation.load() + 1;
  snap->env_key = entry.oss_endpoint + "|" + entry.oss_bucket + "|" +
                  entry.oss_prefix + "|" +
                  (entry.cred_mode == CredMode::kStatic ? "user" : "token");
  std::atomic_store(&entry.snapshot,
                    std::shared_ptr<const TableEnvSnapshot>(std::move(snap)));
  announce(database, table, entry);
}

int PvfsTableCache::refresh_entry(const std::string& database,
                                 const std::string& table,
                                 TableCacheEntry& entry, bool force_token) {
  if (refresh_fault_hook_) {
    int h = refresh_fault_hook_();
    if (h != 0) return h;
  }
  time_t now = time(nullptr);
  bool has_current = ready(entry);

  // Refresh location if expired
  bool location_changed = false;
  if (entry.location.empty() || now >= entry.location_expire_time.load()) {
    TableInfo info;
    int r = rest_client_->get_table(database, table, info);
    if (r != 0) return r;
    // An external table uses the user's credential when there is one,
    // as the Paimon clients do; a flip of the flag rebuilds the env.
    bool has_static = !opts_.static_access_key_id.empty() &&
                      !opts_.static_access_key_secret.empty();
    CredMode mode = (info.is_external && has_static) ? CredMode::kStatic
                                                     : CredMode::kToken;
    location_changed = entry.location != info.location ||
                       entry.cred_mode != mode ||
                       entry.is_external != info.is_external;
    entry.location = info.location;
    entry.is_external = info.is_external;
    entry.table_type = info.table_type;
    entry.cred_mode = mode;
    parse_oss_location(info.location, entry.oss_bucket, entry.oss_prefix);
    entry.location_expire_time = now + opts_.location_cache_ttl_sec;
    if (location_changed) {
      entry.announced = false;
      LOG_INFO("PVFS table location: `/` -> `", database, table,
               info.location);
    }
  }

  if (entry.cred_mode == CredMode::kStatic) {
    if (has_current && !location_changed) return 0;
    TableCredential cred;
    cred.access_key_id = opts_.static_access_key_id;
    cred.access_key_secret = opts_.static_access_key_secret;
    cred.expiration_sec = std::numeric_limits<time_t>::max();
    std::string endpoint = !opts_.external_oss_endpoint.empty()
                               ? opts_.external_oss_endpoint
                               : opts_.oss_endpoint;
    if (endpoint.empty()) {
      // Neither endpoint option is set: the token, if the catalog issues
      // one, still names the region's endpoint.
      TableCredential probe;
      if (rest_client_->get_table_token(database, table, probe) == 0) {
        endpoint = probe.oss_endpoint;
      }
    }
    entry.oss_endpoint =
        endpoint.empty() ? std::string() : normalize_oss_endpoint(endpoint);
    if (entry.oss_endpoint.empty()) {
      LOG_ERROR(
          "PVFS `/`: no OSS endpoint for `, set pvfs_oss_endpoint or pvfs_external_oss_endpoint",
          database, table, entry.location);
      return -EINVAL;
    }
    publish_snapshot(database, table, entry, cred);
    entry.credential_generation++;
    entry.credential_expire_time = cred.expiration_sec;
    return 0;
  }

  // Refresh credentials if expired or not fetched yet
  if (!has_current || location_changed || force_token ||
      now + opts_.credential_refresh_ahead_sec >=
          entry.credential_expire_time.load()) {
    if (entry.is_external && !entry.warned_external_token) {
      // The catalog token only covers paths inside its managed bucket.
      entry.warned_external_token = true;
      external_token_warnings_++;
      LOG_WARN(
          "PVFS `/` is an external table at ` served with the catalog token; set oss_access_key_id/oss_access_key_secret to use your own credential for it",
          database, table, entry.location);
    }
    TableCredential cred;
    int r = rest_client_->get_table_token(database, table, cred);
    if (r != 0) return r;

    entry.oss_endpoint = normalize_oss_endpoint(
        opts_.oss_endpoint.empty() ? cred.oss_endpoint : opts_.oss_endpoint);

    publish_snapshot(database, table, entry, cred);
    entry.credential_generation++;
    entry.credential_expire_time = cred.expiration_sec;
    LOG_INFO("PVFS credentials refreshed: `/`", database, table);
  }
  return 0;
}

// Caller holds table_cache_lock_. Evicted entries are handed back so their
// stores are torn down after the lock is released.
void PvfsTableCache::evict_locked(
    const std::shared_ptr<TableCacheEntry>& keep,
    std::vector<std::shared_ptr<TableCacheEntry>>& evicted) {
  if (opts_.max_table_cache <= 0) return;
  while (static_cast<int>(table_cache_.size()) > opts_.max_table_cache) {
    auto oldest = table_cache_.end();
    time_t oldest_time = std::numeric_limits<time_t>::max();
    for (auto it = table_cache_.begin(); it != table_cache_.end(); ++it) {
      if (it->second == keep) continue;
      if (it->second->last_access_time < oldest_time) {
        oldest_time = it->second->last_access_time;
        oldest = it;
      }
    }
    if (oldest == table_cache_.end()) break;
    LOG_INFO("PVFS evicting table cache: `", oldest->first);
    evicted.push_back(std::move(oldest->second));
    table_cache_.erase(oldest);
  }
}

// Take the slots' stores away; in-flight users keep their reference and
// the reaper deletes each store on its vCPU afterwards.
void PvfsTableCache::release_stores(TableCacheEntry& entry) {
  entry.evicted = true;
  for (size_t i = 0; i < entry.slots.size(); i++) {
    auto store = std::atomic_exchange(&entry.slots[i]->store,
                                      std::shared_ptr<IObjStore>());
    if (store) retire_store(static_cast<int>(i), std::move(store));
  }
}

void PvfsTableCache::retire_store(int vcpu, std::shared_ptr<IObjStore> store) {
  std::lock_guard<std::mutex> lock(retired_lock_);
  retired_.push_back({vcpu, std::move(store)});
}

void PvfsTableCache::reap_retired_stores(bool final) {
  std::vector<RetiredStore> ready;
  {
    std::lock_guard<std::mutex> lock(retired_lock_);
    for (auto it = retired_.begin(); it != retired_.end();) {
      if (final || it->store.use_count() == 1) {
        ready.push_back(std::move(*it));
        it = retired_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& r : ready) {
    if (r.store.use_count() != 1) {
      LOG_WARN("PVFS store on vCPU ` deleted while still referenced", r.vcpu);
    }
    IObjStore* raw = r.store.get();
    {
      std::lock_guard<std::mutex> lock(retired_lock_);
      live_.erase(raw);
    }
    executors_[r.vcpu]->perform([&]() { delete raw; });
    stores_reaped_++;
  }
}

uint64_t PvfsTableCache::live_stores() {
  std::lock_guard<std::mutex> lock(retired_lock_);
  return live_.size();
}

std::shared_ptr<IObjStore> PvfsTableCache::store_for_vcpu(
    TableCacheEntry& entry, int vcpu) {
  auto snap = std::atomic_load(&entry.snapshot);
  if (!snap || vcpu < 0 || vcpu >= static_cast<int>(entry.slots.size()) ||
      stopping_.load()) {
    return nullptr;
  }
  if (photon::get_vcpu() != vcpus_[vcpu] && misrouted_.fetch_add(1) == 0) {
    LOG_ERROR("PVFS store slot ` used from vCPU `", vcpu, photon::get_vcpu());
  }
  auto& slot = *entry.slots[vcpu];
  auto store = std::atomic_load(&slot.store);
  if (!store || slot.env_key != snap->env_key) {
    // One builder per slot: the others wait and take what it published.
    photon::scoped_lock build(slot.build_lock);
    snap = std::atomic_load(&entry.snapshot);
    store = std::atomic_load(&slot.store);
    if (!store || slot.env_key != snap->env_key) {
      if (store) retire_store(vcpu, store);
      auto oss_opts =
          store_options_for(snap->endpoint, snap->bucket, snap->prefix);
      IObjStore* raw = OssFileSystem::new_oss_store(
          snap->creds.accessKeyId.c_str(), snap->creds.accessKeySecret.c_str(),
          oss_opts);
      {
        std::lock_guard<std::mutex> lock(retired_lock_);
        live_[raw] = vcpu;
      }
      stores_created_++;
      LOG_INFO("PVFS store built: slot ` (` so far)", vcpu,
               stores_created_.load());
      if (!snap->creds.securityToken.empty()) {
        raw->set_credentials(ObjCredentials(snap->creds),
                             CredsMeta{snap->expiration, snap->generation});
      }
      // The reaper deletes it on this vCPU once no caller holds it.
      store = std::shared_ptr<IObjStore>(raw, [](IObjStore*) {});
      slot.env_key = snap->env_key;
      slot.applied_generation = snap->generation;
      std::atomic_store(&slot.store, store);
      if (entry.evicted.load()) {
        auto s = std::atomic_exchange(&slot.store, std::shared_ptr<IObjStore>());
        if (s) retire_store(vcpu, std::move(s));
      }
      return store;
    }
  }
  if (slot.applied_generation != snap->generation) {
    store->set_credentials(ObjCredentials(snap->creds),
                           CredsMeta{snap->expiration, snap->generation});
    slot.applied_generation = snap->generation;
  }
  return store;
}

std::shared_ptr<TableCacheEntry> PvfsTableCache::resolve(
    const std::string& database, const std::string& table, int* err) {
  if (err) *err = -EIO;
  if (stopping_.load()) return nullptr;
  std::string key = database + "/" + table;
  std::shared_ptr<TableCacheEntry> entry;
  std::vector<std::shared_ptr<TableCacheEntry>> evicted;

  {
    std::lock_guard<std::mutex> lock(table_cache_lock_);
    auto it = table_cache_.find(key);
    if (it == table_cache_.end()) {
      entry = std::make_shared<TableCacheEntry>();
      entry->last_access_time = time(nullptr);
      for (size_t i = 0; i < executors_.size(); i++) {
        entry->slots.push_back(std::make_unique<VCpuStoreSlot>());
      }
      table_cache_[key] = entry;
      evict_locked(entry, evicted);
    } else {
      entry = it->second;
    }
  }
  for (auto& e : evicted) release_stores(*e);
  evicted.clear();

  time_t now = time(nullptr);
  entry->last_access_time = now;
  // Fast path: a valid token and a fresh location need no lock. Renewal
  // ahead of expiry is the background loop's job; only an expired token or
  // a location due for re-resolution is fetched here.
  auto fresh = [&](time_t t) {
    return t < entry->credential_expire_time.load() &&
           t < entry->location_expire_time.load();
  };
  if (ready(*entry) && fresh(now)) return entry;

  photon::scoped_lock entry_lock(entry->refresh_mutex);
  if (ready(*entry) && fresh(time(nullptr))) return entry;

  int r = refresh_entry(database, table, *entry);
  if (r != 0) {
    if (err) *err = r;
    return nullptr;
  }
  return entry;
}

}  // namespace PvfsFileSystem
