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

#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/photon.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/filesystem.h"
#include "common/macros.h"
#include "fs/fs.h"
#include "options.h"
#include "pvfs/pvfs_runtime.h"
#include "pvfs/pvfs_table_cache.h"
#include "pvfs_test_helper.h"

DECLARE_string(pvfs_test_database);
DECLARE_string(pvfs_test_table);
DECLARE_string(pvfs_test_table2);
DECLARE_bool(pvfs_test_fast);

// Wall-clock helpers: every failure path is asserted against a bound.
inline std::chrono::steady_clock::time_point clock_now() {
  return std::chrono::steady_clock::now();
}
inline int64_t ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(clock_now() -
                                                               t0)
      .count();
}

// A PVFS mount as the tests see it: OssFs over PvfsObjStore, driven through
// the FUSE low-level API. Every instance owns its runtime and vCPUs.
class PvfsTestSuite : public ::testing::Test {
 public:
  void SetUp() override;
  void TearDown() override;

 public:
  struct DirEntry {
    uint64_t nodeid;
    std::string name;
    bool is_dir;
    DirEntry(uint64_t id, const std::string& n, bool d)
        : nodeid(id), name(n), is_dir(d) {}
  };

  struct Instance {
    PvfsTestEnv env;
    OssFileSystem::OssFs* fs = nullptr;
    ~Instance();
    // Builds the stores, the OssFs and runs its init (check_bucket).
    int init(const PvfsFileSystem::PvfsRuntimeOptions& ro,
             const OssFileSystem::OssFsOptions& fo, int vcpus);
    PvfsFileSystem::PvfsRuntime* runtime() { return env.runtime(); }
    PvfsFileSystem::PvfsTableCache* cache() {
      return env.runtime()->table_cache();
    }
  };

  void init();
  // Runtime options from the test flags: writable, rooted at the catalog.
  PvfsFileSystem::PvfsRuntimeOptions base_opts();
  // OssFs options every instance starts from.
  OssFileSystem::OssFsOptions base_fs_opts();
  // A ready instance, or nullptr with the error in *err.
  std::unique_ptr<Instance> make_instance(
      const PvfsFileSystem::PvfsRuntimeOptions& ro,
      const OssFileSystem::OssFsOptions& fo, int* err = nullptr);
  std::unique_ptr<Instance> make_instance(
      const PvfsFileSystem::PvfsRuntimeOptions& ro, int* err = nullptr) {
    return make_instance(ro, base_fs_opts(), err);
  }
  PvfsFileSystem::PvfsRuntime* runtime() { return inst_->runtime(); }
  PvfsFileSystem::PvfsTableCache* cache() { return inst_->cache(); }
  const PvfsFileSystem::PaimonRESTClient::Stats& rest_stats() {
    return runtime()->rest_client()->stats();
  }

  // Run the suite helpers against another instance for a while.
  struct FsSwap {
    PvfsTestSuite& s;
    IFileSystemFuseLL* prev;
    FsSwap(PvfsTestSuite& suite, IFileSystemFuseLL* fs)
        : s(suite), prev(suite.fs_) {
      s.fs_ = fs;
    }
    ~FsSwap() { s.fs_ = prev; }
  };

  // Find the first database/table pair the tests may use. Both nodeids
  // must be forgotten by the caller.
  int find_table(uint64_t& db_nodeid, uint64_t& tbl_nodeid);
  // Remove leftovers of earlier runs of this case from the pinned table.
  void purge_stale_subdirs();
  int remove_tree(uint64_t parent, const std::string& name);

  // Lookup a path like "/db/table/subdir/file" by walking each component.
  // Returns 0 on success, negative errno on failure.
  int lookup_path(const std::string& path, uint64_t& nodeid, struct stat& st);

  // List directory entries (excluding . and ..); filter=false shows every
  // database and table, not only the configured pair.
  int list_dir(uint64_t nodeid, std::vector<DirEntry>& entries,
               bool filter = true);
  void filter_entries(uint64_t nodeid, std::vector<DirEntry>& entries);

  // Create a file and write data, returns bytes written or negative errno.
  ssize_t create_and_write(uint64_t parent, const std::string& name,
                           const void* data, size_t size, uint64_t& nodeid);

  // Read entire file content.
  ssize_t read_file(uint64_t nodeid, std::string& content);

  IFileSystemFuseLL* fs_ = nullptr;  // the instance under test
  std::unique_ptr<Instance> inst_;
  size_t upload_part_size_ = 0;  // 0 keeps the OssFs default
  int max_table_cache_ = 0;      // 0 keeps the cache default
  int vcpus_ = 2;
  std::function<void(PvfsFileSystem::PvfsRuntimeOptions&,
                     OssFileSystem::OssFsOptions&)>
      tweak_opts_;
  uint64_t root_nodeid_ = OssFileSystem::kMountPointNodeId;
  std::string test_subdir_;  // unique subdir name per test case and run
  std::string case_prefix_;  // shared by every run of this case

  static int filler(void* ctx, uint64_t nodeid, const char* name,
                    const struct stat* stbuf, off_t off);
};
