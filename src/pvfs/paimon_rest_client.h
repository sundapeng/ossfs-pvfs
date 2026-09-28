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

#include <photon/net/http/client.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dlf4_signer.h"

namespace PvfsFileSystem {

struct TableInfo {
  std::string location;  // top-level "path" ("location" is still accepted)
  bool is_external = false;  // top-level isExternal: a user-managed path
  std::string table_type;    // schema.options["type"], empty when absent
  std::map<std::string, std::string> options;  // schema.options as sent
};

struct TableCredential {
  std::string access_key_id;
  std::string access_key_secret;
  std::string security_token;
  std::string oss_endpoint;
  int64_t expiration_sec = 0;  // Unix timestamp in seconds
};

struct PaimonRESTClientOptions {
  std::string endpoint;       // e.g. "dlfnext.cn-shanghai.aliyuncs.com"
  std::string region;         // e.g. "cn-shanghai"
  std::string catalog;        // catalog name
  std::string access_key_id;
  std::string access_key_secret;
  std::string security_token;
  std::string signing_algorithm = "auto";  // auto, default, openapi
  int timeout_ms = 30000;
  bool enable_ipv6 = false;  // false: resolve the endpoint to IPv4 only
  int list_page_size = 1000;  // maxResults per list request
};

// REST client for DLF Paimon catalog API.
// Uses PhotonLibOS HTTP client (coroutine-safe).
class PaimonRESTClient {
 public:
  explicit PaimonRESTClient(const PaimonRESTClientOptions& opts);
  ~PaimonRESTClient();

  // Initialize: create the HTTP client, resolve the config prefix and
  // verify that the catalog answers. Must be called from a Photon context.
  int init();

  // Map an HTTP status (or a negative errno from the transport) to -errno.
  static int status_to_errno(int status);

  // Split an optional http:// or https:// prefix off an endpoint. Returns
  // the bare host and whether TLS is used; without a scheme, default_https.
  static std::pair<std::string, bool> split_scheme(const std::string& endpoint,
                                                   bool default_https);

  // Number of REST calls per operation, for tests and diagnostics.
  struct Stats {
    std::atomic<uint64_t> list_databases{0};
    std::atomic<uint64_t> list_tables{0};
    std::atomic<uint64_t> get_database{0};
    std::atomic<uint64_t> get_table{0};
    std::atomic<uint64_t> get_table_token{0};
  };
  const Stats& stats() const { return stats_; }
  // Pages followed per listing before it is given up as looping.
  static constexpr int kMaxListPages = 100;
  // User-Agent of every REST call, so server logs can attribute the traffic.
  static const char* user_agent();

  // List all database names in the catalog (follows nextPageToken).
  int list_databases(std::vector<std::string>& out);

  // List all table names in a database (follows nextPageToken).
  int list_tables(const std::string& database,
                  std::vector<std::string>& out);

  // Check that a database exists: 0, -ENOENT or another errno.
  int get_database(const std::string& database);

  // Get table info (location, tableId).
  int get_table(const std::string& database, const std::string& table,
                TableInfo& out);

  // Get temporary credentials for accessing table data in OSS.
  int get_table_token(const std::string& database, const std::string& table,
                      TableCredential& out);

 private:
  // Perform an HTTP request. Returns HTTP status code, or -errno on error.
  // Response body is written to `response_body`.
  int do_request(std::string_view method, const std::string& path,
                 const std::string& query, const std::string& body,
                 std::string& response_body);

  // Build the full URL for a request.
  std::string build_url(const std::string& path, const std::string& query);

  // One page of a listing; `token` is updated for the next call.
  int list_page(const std::string& path, const char* field,
                std::string& token, std::vector<std::string>& out);
  int list_all(const std::string& path, const char* field,
               std::atomic<uint64_t>& counter,
               std::vector<std::string>& names);

  PaimonRESTClientOptions opts_;
  Stats stats_;
  DlfSigner signer_;
  std::string rest_prefix_;
  bool use_https_ = false;
  photon::net::http::Client* http_client_ = nullptr;  // reused across requests
};

}  // namespace PvfsFileSystem
