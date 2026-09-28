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

#include <ctime>
#include <map>
#include <string>
#include <string_view>

namespace PvfsFileSystem {

// Request signer for DLF/Paimon REST API.
// Supports both OpenAPI (dlfnext, ROA HMAC-SHA1) and VPC (DLF4-HMAC-SHA256).
class DlfSigner {
 public:
  DlfSigner(std::string_view region, std::string_view access_key_id,
            std::string_view access_key_secret,
            std::string_view security_token = "",
            bool is_openapi = true);

  // Sign a request. Populates auth-related headers in `headers`.
  void sign(std::string_view method, std::string_view path,
            std::string_view query, std::string_view body,
            std::map<std::string, std::string>& headers);

  // Freeze the clock (golden-vector tests); 0 restores the wall clock.
  void set_clock_for_test(time_t now) { fixed_now_ = now; }

 private:
  // ROA signing for dlfnext (OpenAPI) endpoints
  void sign_roa(std::string_view method, std::string_view path,
                std::string_view query, std::string_view body,
                std::map<std::string, std::string>& headers);

  // DLF4 signing for VPC endpoints
  void sign_dlf4(std::string_view method, std::string_view path,
                 std::string_view query, std::string_view body,
                 std::map<std::string, std::string>& headers);

  std::string region_;
  std::string access_key_id_;
  std::string access_key_secret_;
  std::string security_token_;
  bool is_openapi_;
  time_t fixed_now_ = 0;
};

}  // namespace PvfsFileSystem
