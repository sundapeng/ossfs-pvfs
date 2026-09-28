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

#include "pvfs_options.h"

#include <cstdlib>
#include <regex>

#include "common/logger.h"
#include "options.h"

namespace PvfsFileSystem {

static bool flag_is_default(const char* name) {
  return gflags::GetCommandLineFlagInfoOrDie(name).is_default;
}

std::string derive_region_from_endpoint(const std::string& endpoint) {
  std::string host = PaimonRESTClient::split_scheme(endpoint, true).first;
  static const std::regex kPublic("^dlfnext\\.([a-z]{2}-[a-z]+(?:-[0-9]+)?)\\.");
  static const std::regex kVpc("^dlf-.*-([a-z]{2}-[a-z]+(?:-[0-9]+)?)-vpc\\.");
  std::smatch m;
  if (std::regex_search(host, m, kPublic) || std::regex_search(host, m, kVpc)) {
    return m[1].str();
  }
  return "";
}

bool pvfs_mode_selected(std::string* catalog) {
  if (!FLAGS_pvfs_catalog.empty()) {
    if (catalog) *catalog = FLAGS_pvfs_catalog;
    return true;
  }
  if (FLAGS_oss_bucket.compare(0, 7, "pvfs://") == 0) {
    if (catalog) *catalog = FLAGS_oss_bucket.substr(7);
    return true;
  }
  return false;
}

int pvfs_runtime_options_from_flags(const std::string& catalog,
                                    PvfsRuntimeOptions* opts) {
  std::string ak = FLAGS_pvfs_access_key_id;
  std::string sk = FLAGS_pvfs_access_key_secret;
  if (ak.empty()) {
    if (const char* env = std::getenv("PVFS_ACCESS_KEY_ID")) ak = env;
  }
  if (sk.empty()) {
    if (const char* env = std::getenv("PVFS_ACCESS_KEY_SECRET")) sk = env;
  }

  if (FLAGS_pvfs_endpoint.empty()) {
    LOG_ERROR("pvfs_endpoint is required for PVFS mode");
    return -EINVAL;
  }
  std::string region = FLAGS_pvfs_region;
  if (region.empty()) {
    region = derive_region_from_endpoint(FLAGS_pvfs_endpoint);
    if (region.empty()) {
      LOG_ERROR("pvfs_region cannot be derived from `, set it explicitly",
                FLAGS_pvfs_endpoint);
      return -EINVAL;
    }
    LOG_INFO("PVFS: region ` derived from endpoint `", region,
             FLAGS_pvfs_endpoint);
  }
  if (ak.empty() || sk.empty()) {
    LOG_ERROR(
        "PVFS credentials required: set pvfs_access_key_id/"
        "pvfs_access_key_secret or the PVFS_ACCESS_KEY_ID/"
        "PVFS_ACCESS_KEY_SECRET env vars");
    return -EINVAL;
  }
  // IPv4 only unless --enable_ipv6 is given explicitly: the OSS default
  // allows both and costs a failed dial per connection without a route.
  bool enable_ipv6 = !flag_is_default("enable_ipv6") && FLAGS_enable_ipv6;

  auto& rest = opts->rest;
  rest.endpoint = FLAGS_pvfs_endpoint;
  rest.region = region;
  rest.catalog = catalog;
  rest.access_key_id = ak;
  rest.access_key_secret = sk;
  rest.security_token = FLAGS_pvfs_security_token;
  rest.signing_algorithm = FLAGS_pvfs_signing_algorithm;
  rest.enable_ipv6 = enable_ipv6;

  auto& cache = opts->cache;
  cache.oss_endpoint = FLAGS_pvfs_oss_endpoint;
  // External tables use the user's OSS credential, given the way OSS mode
  // takes it: the flags, or OSS_ACCESS_KEY_ID/SECRET when they are unset.
  cache.static_access_key_id = FLAGS_oss_access_key_id;
  cache.static_access_key_secret = FLAGS_oss_access_key_secret;
  if (flag_is_default("oss_access_key_id") ||
      flag_is_default("oss_access_key_secret")) {
    const char* id = std::getenv("OSS_ACCESS_KEY_ID");
    const char* secret = std::getenv("OSS_ACCESS_KEY_SECRET");
    cache.static_access_key_id = id ? id : "";
    cache.static_access_key_secret = secret ? secret : "";
  }
  cache.external_oss_endpoint = FLAGS_pvfs_external_oss_endpoint.empty()
                                    ? FLAGS_pvfs_oss_endpoint
                                    : FLAGS_pvfs_external_oss_endpoint;
  cache.location_cache_ttl_sec = FLAGS_pvfs_location_cache_ttl;
  cache.credential_refresh_ahead_sec = FLAGS_pvfs_credential_refresh_ahead;
  cache.max_table_cache = FLAGS_pvfs_max_table_cache;
  cache.enable_ipv6 = enable_ipv6;
  // The table stores take the OSS client settings of an OSS mount.
  cache.store_options.max_list_ret_cnt = FLAGS_max_list_ret_count;
  cache.store_options.use_list_obj_v2 = FLAGS_use_list_obj_v2;
  cache.store_options.request_timeout_us = FLAGS_oss_request_timeout_ms * 1000;
  cache.store_options.user_agent = "ossfs2-pvfs";

  // Writes are opt-in on PVFS; --ro still wins over --pvfs_allow_write.
  opts->readonly = FLAGS_ro || !FLAGS_pvfs_allow_write;
  opts->allow_metadata_write = FLAGS_pvfs_allow_metadata_write;
  std::string_view prefix = FLAGS_oss_bucket_prefix;
  while (!prefix.empty() && prefix.front() == '/') prefix.remove_prefix(1);
  while (!prefix.empty() && prefix.back() == '/') prefix.remove_suffix(1);
  opts->prefix = std::string(prefix);
  return 0;
}

}  // namespace PvfsFileSystem
