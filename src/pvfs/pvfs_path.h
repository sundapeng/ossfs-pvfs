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

#include <string>
#include <string_view>

namespace PvfsFileSystem {

// A parsed FUSE path: "/" (root), "/db", "/db/table" or "/db/table/sub...";
// the catalog comes from the mount URI, not from the path.
struct PvfsPath {
  enum Level {
    ROOT,      // "/"              -> list databases
    DATABASE,  // "/db"            -> list tables in db
    TABLE,     // "/db/table"      -> table root dir (delegate to OSS)
    SUBPATH,   // "/db/table/x/y"  -> file within table (delegate to OSS)
  };

  Level level = ROOT;
  std::string database;
  std::string table;
  std::string subpath;  // relative path within table (no leading '/')

  // Parse a FUSE path (e.g. "/db/table/data/file.parquet").
  static PvfsPath parse(std::string_view path);

  // Cache key for table-level caching: "database/table"
  std::string cache_key() const { return database + "/" + table; }

  bool is_virtual() const { return level == ROOT || level == DATABASE; }
};

// True when the first component of a table-relative path is one of Paimon's
// metadata directories (snapshot, manifest, ..., bucket-*).
bool is_reserved_first_component(std::string_view subpath);

}  // namespace PvfsFileSystem
