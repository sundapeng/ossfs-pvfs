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

#include "dlf4_signer.h"

#include <photon/net/utils.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

namespace PvfsFileSystem {

static std::string hex_encode(std::string_view data);
static std::string base64_encode(std::string_view data);

static constexpr const char* kDlf4Algorithm = "DLF4-HMAC-SHA256";
static constexpr const char* kRequestType = "aliyun_v4_request";
static constexpr const char* kProduct = "DlfNext";
static constexpr const char* kSeedPrefix = "aliyun_v4";
static constexpr const char* kAcsVersion = "2026-01-18";

// Get current UTC time in ISO8601 compact format: YYYYMMDDTHHMMSSz
static std::string get_iso8601_time(time_t now) {
  struct tm utc;
  gmtime_r(&now, &utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
  return buf;
}

// Get current UTC time in RFC 1123 format for Date header
static std::string get_gmt_date(time_t now) {
  struct tm utc;
  gmtime_r(&now, &utc);
  char buf[64];
  strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &utc);
  return buf;
}

// Extract date portion: YYYYMMDD
static std::string get_date(const std::string& iso8601) {
  return iso8601.substr(0, 8);
}

static std::string sha256_hex(std::string_view data) {
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, data.data(), data.size());
  EVP_DigestFinal_ex(ctx, hash, &len);
  EVP_MD_CTX_free(ctx);
  return hex_encode(std::string_view(reinterpret_cast<char*>(hash), len));
}

static std::string hmac_sha256(std::string_view key, std::string_view data) {
  unsigned char result[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(data.data()), data.size(), result,
       &len);
  return std::string(reinterpret_cast<char*>(result), len);
}

static std::string hmac_sha1(std::string_view key, std::string_view data) {
  unsigned char result[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(data.data()), data.size(), result,
       &len);
  return std::string(reinterpret_cast<char*>(result), len);
}

static std::string hex_encode(std::string_view data) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string result;
  result.reserve(data.size() * 2);
  for (unsigned char c : data) {
    result.push_back(hex_chars[c >> 4]);
    result.push_back(hex_chars[c & 0x0f]);
  }
  return result;
}

static std::string base64_encode(std::string_view data) {
  std::string out;
  photon::net::Base64Encode(data, out);
  return out;
}

static std::string md5_base64(std::string_view data) {
  unsigned char md5[MD5_DIGEST_LENGTH];
  MD5(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md5);
  return base64_encode(
      std::string_view(reinterpret_cast<char*>(md5), MD5_DIGEST_LENGTH));
}

DlfSigner::DlfSigner(std::string_view region, std::string_view access_key_id,
                      std::string_view access_key_secret,
                      std::string_view security_token, bool is_openapi)
    : region_(region),
      access_key_id_(access_key_id),
      access_key_secret_(access_key_secret),
      security_token_(security_token),
      is_openapi_(is_openapi) {}

void DlfSigner::sign(std::string_view method, std::string_view path,
                      std::string_view query, std::string_view body,
                      std::map<std::string, std::string>& headers) {
  if (is_openapi_) {
    sign_roa(method, path, query, body, headers);
  } else {
    sign_dlf4(method, path, query, body, headers);
  }
}

// ROA signing for Aliyun OpenAPI (dlfnext) endpoints.
// Uses HMAC-SHA1 with canonical headers.
void DlfSigner::sign_roa(std::string_view method, std::string_view path,
                           std::string_view query, std::string_view body,
                           std::map<std::string, std::string>& headers) {
  std::string date = get_gmt_date(fixed_now_ ? fixed_now_ : time(nullptr));

  // Set required ACS headers
  headers["x-acs-version"] = kAcsVersion;
  headers["x-acs-signature-method"] = "HMAC-SHA1";
  headers["x-acs-signature-version"] = "1.0";
  headers["date"] = date;

  if (!security_token_.empty()) {
    headers["x-acs-security-token"] = security_token_;
  }

  if (headers.find("content-type") == headers.end()) {
    headers["content-type"] = "application/json";
  }
  if (headers.find("accept") == headers.end()) {
    headers["accept"] = "application/json";
  }

  std::string content_md5;
  if (!body.empty()) {
    content_md5 = md5_base64(body);
    headers["content-md5"] = content_md5;
  }

  // Build canonical x-acs-* headers (sorted by key)
  std::string canonical_headers;
  std::vector<std::pair<std::string, std::string>> acs_headers;
  for (const auto& [k, v] : headers) {
    std::string lower_key = k;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                   ::tolower);
    if (lower_key.substr(0, 6) == "x-acs-") {
      acs_headers.emplace_back(lower_key, v);
    }
  }
  std::sort(acs_headers.begin(), acs_headers.end());
  for (const auto& [k, v] : acs_headers) {
    canonical_headers += k + ":" + v + "\n";
  }

  // Build pathname with query string
  std::string pathname = std::string(path);
  if (!query.empty()) {
    pathname += "?" + std::string(query);
  }

  // Build string to sign:
  // METHOD\nAccept\nContent-MD5\nContent-Type\nDate\nCanonicalHeaders\nPathname
  std::string string_to_sign;
  string_to_sign += std::string(method) + "\n";
  string_to_sign += headers["accept"] + "\n";
  string_to_sign += content_md5 + "\n";
  string_to_sign += headers["content-type"] + "\n";
  string_to_sign += date + "\n";
  string_to_sign += canonical_headers;
  string_to_sign += pathname;

  // Sign with HMAC-SHA1
  std::string raw_sig = hmac_sha1(access_key_secret_, string_to_sign);
  std::string signature = base64_encode(raw_sig);

  // Authorization: acs {accessKeyId}:{signature}
  headers["authorization"] = "acs " + access_key_id_ + ":" + signature;

}

// DLF4-HMAC-SHA256 signing for VPC endpoints.
void DlfSigner::sign_dlf4(std::string_view method, std::string_view path,
                            std::string_view query, std::string_view body,
                            std::map<std::string, std::string>& headers) {
  std::string timestamp =
      get_iso8601_time(fixed_now_ ? fixed_now_ : time(nullptr));
  std::string date = get_date(timestamp);

  headers["x-dlf-date"] = timestamp;
  headers["x-dlf-version"] = "v1";
  headers["x-dlf-content-sha256"] = "UNSIGNED-PAYLOAD";

  if (!security_token_.empty()) {
    headers["x-dlf-security-token"] = security_token_;
  }

  // Only set Content-Type and Content-MD5 when body is present
  if (!body.empty()) {
    headers["content-md5"] = md5_base64(body);
    if (headers.find("content-type") == headers.end()) {
      headers["content-type"] = "application/json";
    }
  }

  // Signed headers, sorted by key: content-md5, content-type, x-dlf-* and
  // x-dlf-security-token only, matching the Python signer.
  static const std::vector<std::string> kSignedHeaderKeys = {
      "content-md5", "content-type", "x-dlf-content-sha256",
      "x-dlf-date",  "x-dlf-version", "x-dlf-security-token",
  };
  std::vector<std::pair<std::string, std::string>> signed_headers;
  for (const auto& [k, v] : headers) {
    std::string lower_key = k;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                   ::tolower);
    for (const auto& sk : kSignedHeaderKeys) {
      if (lower_key == sk) {
        signed_headers.emplace_back(lower_key, v);
        break;
      }
    }
  }
  std::sort(signed_headers.begin(), signed_headers.end());

  std::string canonical_headers;
  for (const auto& [k, v] : signed_headers) {
    canonical_headers += k + ":" + v + "\n";
  }

  // Canonical request format (matching Python DLFDefaultSigner):
  //   METHOD\nPATH\nQUERY\nheader1:val1\n...\nUNSIGNED-PAYLOAD
  std::string canonical_request;
  canonical_request += std::string(method) + "\n";
  canonical_request += std::string(path) + "\n";
  canonical_request += std::string(query) + "\n";
  canonical_request += canonical_headers;
  canonical_request += "UNSIGNED-PAYLOAD";

  std::string scope =
      date + "/" + region_ + "/" + kProduct + "/" + kRequestType;

  std::string string_to_sign;
  string_to_sign += std::string(kDlf4Algorithm) + "\n";
  string_to_sign += timestamp + "\n";
  string_to_sign += scope + "\n";
  string_to_sign += sha256_hex(canonical_request);

  std::string seed_key = std::string(kSeedPrefix) + access_key_secret_;
  std::string date_key = hmac_sha256(seed_key, date);
  std::string date_region_key = hmac_sha256(date_key, region_);
  std::string date_region_service_key = hmac_sha256(date_region_key, kProduct);
  std::string signing_key =
      hmac_sha256(date_region_service_key, kRequestType);

  std::string signature = hex_encode(hmac_sha256(signing_key, string_to_sign));

  std::string authorization;
  authorization += std::string(kDlf4Algorithm) + " ";
  authorization += "Credential=" + access_key_id_ + "/" + scope + ",";
  authorization += "Signature=" + signature;

  headers["authorization"] = authorization;
}

}  // namespace PvfsFileSystem
