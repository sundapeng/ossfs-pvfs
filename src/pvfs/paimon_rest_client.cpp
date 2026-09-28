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

#include "paimon_rest_client.h"

#include <photon/common/estring.h>
#include <photon/ecosystem/simple_dom.h>
#include <photon/net/http/client.h>
#include <photon/net/utils.h>
#include <photon/thread/thread.h>

#include <cctype>
#include <map>
#include <sstream>

#include "common/logger.h"
#include "common/macros.h"
#include "pvfs_log_throttle.h"

namespace PvfsFileSystem {

using Verb = photon::net::http::Verb;

static Verb to_verb(std::string_view method) {
  if (method == "GET") return Verb::GET;
  if (method == "POST") return Verb::POST;
  if (method == "PUT") return Verb::PUT;
  if (method == "DELETE") return Verb::DELETE;
  if (method == "HEAD") return Verb::HEAD;
  return Verb::GET;
}

// Percent-encode one path segment or query value (RFC 3986 unreserved
// characters pass through).
static std::string encode_segment(std::string_view in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

// Catalog listings are small; anything beyond this is not a REST reply.
static constexpr size_t kMaxResponseBody = 64UL << 20;

// Extract name from a JSON node that may be a string or an object with
// name/database/databaseName/table/tableName fields.
static bool is_json_true(std::string_view v) {
  return v == "true" || v == "True" || v == "TRUE" || v == "1";
}

static std::string extract_name_from_node(photon::SimpleDOM::Node node) {
  for (const char* field : {"name", "database", "databaseName", "table", "tableName"}) {
    auto child = node[field];
    if (child) return std::string(child.to_string_view());
  }
  return std::string(node.to_string_view());
}

// OpenAPI (ROA) signing for dlfnext endpoints, DLF4 otherwise; the
// signing_algorithm option overrides the endpoint-based choice.
static bool resolve_is_openapi(const std::string& endpoint,
                                const std::string& signing_algorithm) {
  if (signing_algorithm == "openapi") return true;
  if (signing_algorithm == "default") return false;
  // auto: detect from endpoint name
  return endpoint.find("dlfnext") != std::string::npos;
}

// IPv4-only resolver: Photon dials AAAA records first and wastes a timeout
// on hosts without an IPv6 route. Plain map cache, so no Photon timer thread.
class Ipv4OnlyResolver : public photon::net::Resolver {
 public:
  photon::net::IPAddr resolve(std::string_view host) override {
    return resolve_filter(host, {});
  }
  photon::net::IPAddr resolve_filter(
      std::string_view host, Delegate<bool, photon::net::IPAddr>) override {
    std::string key(host);
    time_t now = time(nullptr);
    {
      SCOPED_LOCK(lock_);
      auto it = cache_.find(key);
      if (it != cache_.end() && now < it->second.expire) return it->second.addr;
    }
    std::vector<photon::net::IPAddr> addrs;
    photon::net::gethostbyname_nb(host, addrs);
    for (const auto& addr : addrs) {
      if (!addr.is_ipv4()) continue;
      SCOPED_LOCK(lock_);
      cache_[key] = {addr, now + kCacheTtlSec};
      return addr;
    }
    LOG_ERROR("No IPv4 address for host `", host);
    return photon::net::IPAddr();
  }
  void discard_cache(std::string_view host, photon::net::IPAddr) override {
    SCOPED_LOCK(lock_);
    cache_.erase(std::string(host));
  }

 private:
  static constexpr time_t kCacheTtlSec = 60;
  struct Entry {
    photon::net::IPAddr addr;
    time_t expire;
  };
  photon::spinlock lock_;
  std::map<std::string, Entry> cache_;
};

int PaimonRESTClient::status_to_errno(int status) {
  if (status < 0) return status;
  switch (status) {
    case 200:
      return 0;
    case 400:
      return -EINVAL;
    case 401:
    case 403:
      return -EACCES;
    case 404:
      return -ENOENT;
    case 429:
      return -EAGAIN;
    default:
      return -EIO;
  }
}

std::pair<std::string, bool> PaimonRESTClient::split_scheme(
    const std::string& endpoint, bool default_https) {
  if (endpoint.compare(0, 8, "https://") == 0) {
    return {endpoint.substr(8), true};
  }
  if (endpoint.compare(0, 7, "http://") == 0) {
    return {endpoint.substr(7), false};
  }
  return {endpoint, default_https};
}

PaimonRESTClient::~PaimonRESTClient() {
  delete http_client_;  // also deletes the resolver it owns
}

PaimonRESTClient::PaimonRESTClient(const PaimonRESTClientOptions& opts)
    : opts_(opts),
      signer_(opts.region, opts.access_key_id, opts.access_key_secret,
              opts.security_token,
              resolve_is_openapi(opts.endpoint, opts.signing_algorithm)),
      rest_prefix_("v1") {
  // TLS unless the endpoint explicitly says http://: the token reply
  // carries OSS credentials.
  auto [host, https] = split_scheme(opts.endpoint, true);
  opts_.endpoint = host;
  use_https_ = https;
}

std::string PaimonRESTClient::build_url(const std::string& path,
                                         const std::string& query) {
  std::string url;
  url += use_https_ ? "https://" : "http://";
  url += opts_.endpoint;
  url += "/";
  url += path;
  if (!query.empty()) {
    url += "?" + query;
  }
  return url;
}

int PaimonRESTClient::do_request(std::string_view method,
                                  const std::string& path,
                                  const std::string& query,
                                  const std::string& body,
                                  std::string& response_body) {
  // DLF4 signs every Content-Type/Content-MD5 header the request carries,
  // so Content-Type is only added, and signed, when there is a body.
  std::map<std::string, std::string> headers;
  if (!body.empty()) {
    headers["content-type"] = "application/json";
  }

  std::string signing_path = "/" + path;
  signer_.sign(method, signing_path, query, body, headers);

  // accept is not a signed header for either algorithm.
  headers["accept"] = "application/json";

  std::string url = build_url(path, query);

  if (!http_client_) return -ENOTCONN;  // init() creates the client

  auto op = http_client_->new_operation(to_verb(method), url);
  DEFER(op->destroy());

  // Set headers
  for (const auto& [k, v] : headers) {
    op->req.headers.insert(k, v);
  }

  // Set body if present
  if (!body.empty()) {
    op->set_body(body);
  }

  int r = op->call();
  if (r != 0) {
    int saved_errno = errno;
    PVFS_LOG_ERROR_THROTTLED(
        "http|" + url + "|" + std::to_string(saved_errno),
        "HTTP request failed: ` `, error: `", method, url, saved_errno);
    return -saved_errno;
  }

  int status = op->status_code;

  // Read response body
  auto content_length = op->resp.headers.content_length();
  if (content_length > kMaxResponseBody) {
    LOG_ERROR("Response too large: ` `, ` bytes", method, url, content_length);
    return -EMSGSIZE;
  }
  if (content_length > 0) {
    response_body.resize(content_length);
    auto ret = op->resp.read(&response_body[0], content_length);
    if (ret != static_cast<ssize_t>(content_length)) {
      LOG_ERROR("Failed to read response body: ` `, read ` of `", method, url,
                ret, content_length);
      return -EIO;
    }
  } else {
    // Try chunked reading
    response_body.clear();
    char buf[4096];
    ssize_t n;
    while ((n = op->resp.read(buf, sizeof(buf))) > 0) {
      response_body.append(buf, n);
    }
  }

  return status;
}

#ifndef OSSFS_VERSION_ID
#define OSSFS_VERSION_ID unknown
#endif

const char* PaimonRESTClient::user_agent() {
  return "ossfs2-pvfs/" MACRO_STR(OSSFS_VERSION_ID);
}

int PaimonRESTClient::init() {
  if (!http_client_) {
    http_client_ = photon::net::http::new_http_client();
    http_client_->timeout(static_cast<uint64_t>(opts_.timeout_ms) * 1000);
    http_client_->set_user_agent(user_agent());
    if (!opts_.enable_ipv6) {
      http_client_->set_resolver(new Ipv4OnlyResolver(), true);
    }
  }

  // Fetch config prefix: GET /v1/config?warehouse={catalog}
  std::string query = "warehouse=" + encode_segment(opts_.catalog);
  std::string body;
  std::string response;

  int status = do_request("GET", "v1/config", query, body, response);
  if (status == 200 && !response.empty()) {
    auto root = photon::SimpleDOM::parse_copy(response.c_str(), response.size(),
                                              photon::SimpleDOM::DOC_JSON);
    // Try overrides.prefix first, then defaults.prefix
    auto prefix_node = root["overrides"]["prefix"];
    if (!prefix_node) {
      prefix_node = root["defaults"]["prefix"];
    }
    if (prefix_node) {
      std::string prefix = std::string(prefix_node.to_string_view());
      // Remove leading slash if present
      if (!prefix.empty() && prefix.front() == '/') {
        prefix = prefix.substr(1);
      }
      // Combine with API version: "v1/{config_prefix}"
      rest_prefix_ = "v1/" + prefix;
      LOG_INFO("PVFS config prefix resolved: `", rest_prefix_);
    }
  } else if (status == 404) {
    // Older servers have no config endpoint; the probe below tells a
    // missing catalog from a missing endpoint.
    rest_prefix_ = "v1/catalogs/" + opts_.catalog;
    LOG_WARN("No config for catalog ` (404), using prefix `", opts_.catalog,
             rest_prefix_);
  } else {
    LOG_ERROR("Fetching config for catalog ` failed, status: `, body: `",
              opts_.catalog, status, response);
    return status_to_errno(status);
  }

  // Wrong credentials, endpoint or catalog must fail the mount here rather
  // than every request after it.
  std::vector<std::string> probe;
  int r = list_databases(probe);
  if (r != 0) {
    LOG_ERROR("Catalog ` is not reachable through `, err=`", opts_.catalog,
              opts_.endpoint, r);
    return r;
  }
  return 0;
}

int PaimonRESTClient::list_page(const std::string& path, const char* field,
                                std::string& token,
                                std::vector<std::string>& out) {
  std::string query = "maxResults=" + std::to_string(opts_.list_page_size);
  if (!token.empty()) query += "&pageToken=" + encode_segment(token);
  std::string response;
  int status = do_request("GET", path, query, "", response);
  if (status != 200) {
    PVFS_LOG_ERROR_THROTTLED("list|" + path + "|" + std::to_string(status),
                             "list ` failed, status: `, body: `", path, status,
                             response);
    return status_to_errno(status);
  }
  auto root = photon::SimpleDOM::parse_copy(response.c_str(), response.size(),
                                            photon::SimpleDOM::DOC_JSON);
  auto items = root[field];
  if (!items) {
    LOG_ERROR("list `: no '`' field in response", path, field);
    return -EIO;
  }
  for (auto node : items.enumerable_children()) {
    out.push_back(extract_name_from_node(node));
  }
  auto next = root["nextPageToken"];
  token = next ? std::string(next.to_string_view()) : std::string();
  return 0;
}

// A server that keeps handing out page tokens must not spin the client
// forever or fill its memory: the walk stops at a repeated token or page cap.
int PaimonRESTClient::list_all(const std::string& path, const char* field,
                               std::atomic<uint64_t>& counter,
                               std::vector<std::string>& names) {
  std::string token, sent;
  names.clear();
  for (int page = 0;; page++) {
    if (page >= kMaxListPages) {
      LOG_ERROR("list `: more than ` pages (` names), giving up", path,
                kMaxListPages, names.size());
      return -EIO;
    }
    counter++;
    int r = list_page(path, field, token, names);
    if (r != 0) return r;
    if (token.empty()) return 0;
    if (token == sent) {
      LOG_ERROR("list `: server repeated page token `, giving up", path,
                token);
      return -EIO;
    }
    sent = token;
  }
}

int PaimonRESTClient::list_databases(std::vector<std::string>& out) {
  return list_all(rest_prefix_ + "/databases", "databases",
                  stats_.list_databases, out);
}

int PaimonRESTClient::list_tables(const std::string& database,
                                   std::vector<std::string>& out) {
  return list_all(
      rest_prefix_ + "/databases/" + encode_segment(database) + "/tables",
      "tables", stats_.list_tables, out);
}

int PaimonRESTClient::get_database(const std::string& database) {
  stats_.get_database++;
  std::string path = rest_prefix_ + "/databases/" + encode_segment(database);
  std::string response;
  int status = do_request("GET", path, "", "", response);
  if (status != 200) {
    if (status != 404) {
      PVFS_LOG_ERROR_THROTTLED(
          "getDatabase|" + path + "|" + std::to_string(status),
          "getDatabase failed for `, status: `, body: `", database, status,
          response);
    }
    return status_to_errno(status);
  }
  return 0;
}

int PaimonRESTClient::get_table(const std::string& database,
                                 const std::string& table, TableInfo& out) {
  stats_.get_table++;
  std::string path = rest_prefix_ + "/databases/" + encode_segment(database) +
                     "/tables/" + encode_segment(table);
  std::string response;

  int status = do_request("GET", path, "", "", response);
  if (status != 200) {
    if (status != 404) {
      PVFS_LOG_ERROR_THROTTLED(
          "getTable|" + path + "|" + std::to_string(status),
          "getTable failed for `/`, status: `, body: `", database, table,
          status, response);
    }
    return status_to_errno(status);
  }

  auto root = photon::SimpleDOM::parse_copy(response.c_str(), response.size(),
                                            photon::SimpleDOM::DOC_JSON);

  // Bennett answers with a top-level "path"; "location" is kept for the
  // stub and older servers, and the options carry the path too.
  auto loc = root["path"];
  if (!loc) loc = root["location"];
  if (loc) out.location = std::string(loc.to_string_view());

  auto ext = root["isExternal"];
  out.is_external = ext && is_json_true(ext.to_string_view());

  out.options.clear();
  auto opts = root["schema"]["options"];
  if (opts) {
    for (auto node : opts.enumerable_children()) {
      out.options.emplace(std::string(node.key()),
                          std::string(node.to_string_view()));
    }
  }
  auto type = out.options.find("type");
  out.table_type = type == out.options.end() ? std::string() : type->second;
  if (out.location.empty()) {
    auto path = out.options.find("path");
    if (path != out.options.end()) out.location = path->second;
  }

  if (out.location.empty()) {
    LOG_ERROR("getTable: empty location for `/`", database, table);
    return -EIO;
  }

  return 0;
}

int PaimonRESTClient::get_table_token(const std::string& database,
                                       const std::string& table,
                                       TableCredential& out) {
  stats_.get_table_token++;
  std::string path = rest_prefix_ + "/databases/" + encode_segment(database) +
                     "/tables/" + encode_segment(table) + "/token";
  std::string response;

  int status = do_request("GET", path, "", "", response);
  if (status != 200) {
    PVFS_LOG_ERROR_THROTTLED(
        "getTableToken|" + path + "|" + std::to_string(status),
        "getTableToken failed for `/`, status: `, body: `", database, table,
        status, response);
    return status_to_errno(status);
  }

  auto root = photon::SimpleDOM::parse_copy(response.c_str(), response.size(),
                                            photon::SimpleDOM::DOC_JSON);

  // Token fields are nested under "token" with dot-separated keys.
  // SimpleDOM uses '/' as separator by default; the keys have dots.
  auto token_node = root["token"];
  if (token_node) {
    // Iterate children to find fs.oss.* fields
    for (auto node : token_node.enumerable_children()) {
      std::string_view key = node.key();
      std::string_view val = node.to_string_view();
      if (key == "fs.oss.accessKeyId") {
        out.access_key_id = std::string(val);
      } else if (key == "fs.oss.accessKeySecret") {
        out.access_key_secret = std::string(val);
      } else if (key == "fs.oss.securityToken") {
        out.security_token = std::string(val);
      } else if (key == "fs.oss.endpoint") {
        out.oss_endpoint = std::string(val);
      }
    }
  }

  auto expires = root["expiresAtMillis"];
  if (expires) {
    out.expiration_sec = expires.to_int64_t() / 1000;
  } else {
    // Default: 1 hour from now
    out.expiration_sec = time(nullptr) + 3600;
    LOG_WARN("getTableToken: no expiresAtMillis, defaulting to 1h");
  }

  if (out.access_key_id.empty() || out.access_key_secret.empty()) {
    LOG_ERROR("getTableToken: empty credentials for `/`", database, table);
    return -EIO;
  }

  return 0;
}

}  // namespace PvfsFileSystem
