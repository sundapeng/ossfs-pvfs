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

#include "pvfs_path.h"

namespace PvfsFileSystem {

PvfsPath PvfsPath::parse(std::string_view path) {
  PvfsPath result;

  // Strip leading slashes
  while (!path.empty() && path.front() == '/') {
    path.remove_prefix(1);
  }
  // Strip trailing slashes
  while (!path.empty() && path.back() == '/') {
    path.remove_suffix(1);
  }

  if (path.empty()) {
    result.level = ROOT;
    return result;
  }

  // Find first component (database)
  auto pos = path.find('/');
  if (pos == std::string_view::npos) {
    result.level = DATABASE;
    result.database = std::string(path);
    return result;
  }

  result.database = std::string(path.substr(0, pos));
  path.remove_prefix(pos + 1);

  // Find second component (table)
  pos = path.find('/');
  if (pos == std::string_view::npos) {
    result.level = TABLE;
    result.table = std::string(path);
    return result;
  }

  result.table = std::string(path.substr(0, pos));
  result.subpath = std::string(path.substr(pos + 1));
  result.level = SUBPATH;
  return result;
}

bool is_reserved_first_component(std::string_view subpath) {
  while (!subpath.empty() && subpath.front() == '/') subpath.remove_prefix(1);
  auto slash = subpath.find('/');
  std::string_view first =
      slash == std::string_view::npos ? subpath : subpath.substr(0, slash);
  if (first.empty()) return false;
  if (first.compare(0, 7, "bucket-") == 0) return true;
  for (std::string_view name : {"snapshot", "manifest", "schema", "index",
                                "changelog", "statistics", "tag", "branch",
                                "consumer"}) {
    if (first == name) return true;
  }
  return false;
}

}  // namespace PvfsFileSystem
