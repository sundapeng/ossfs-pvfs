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

#include <photon/common/executor/executor.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fs/bg_vcpu_env.h"
#include "pvfs/pvfs_obj_store.h"
#include "pvfs/pvfs_runtime.h"

// True when the test binary runs against a Paimon catalog (--pvfs_catalog)
// instead of an OSS bucket.
bool is_pvfs_test_mode();

// PvfsRuntimeOptions from the pvfs test flags, writable, with `prefix` as
// the mounted sub-tree. Returns -errno when the flags are unusable.
int pvfs_test_runtime_options(PvfsFileSystem::PvfsRuntimeOptions* opts,
                              const std::string& prefix);

// Stand-in for ossutil in Ossfs2TestSuite: the same operations on object
// paths relative to the catalog root ("db/table/dir/file"), served by one
// PvfsObjStore on its own executor.
class PvfsTestHelper {
 public:
  PvfsTestHelper();
  ~PvfsTestHelper();

  int init();

  int upload_file(const std::string& local_file, const std::string& osspath);
  int copy_file(const std::string& src, const std::string& dst);
  int delete_file(const std::string& osspath);
  int create_dir(const std::string& osspath);
  int delete_dir(const std::string& osspath);
  int stat_file(const std::string& osspath);
  // Content-Length, X-Oss-Hash-Crc64ecma, Etag and X-Oss-Object-Type of an
  // object, as ossutil stat prints them; empty when it does not exist.
  std::map<std::string, std::string> get_file_meta(const std::string& osspath);
  // Immediate children of osspath ("" entry for the marker when
  // include_self), directories with a trailing slash.
  std::vector<std::string> get_list_objects(const std::string& osspath,
                                            bool include_self);

  PvfsFileSystem::PvfsRuntime* runtime() { return runtime_.get(); }

 private:
  template <class F>
  auto perform(F&& fn) {
    return executor_->perform(std::forward<F>(fn));
  }

  photon::Executor* executor_ = nullptr;
  std::unique_ptr<PvfsFileSystem::PvfsRuntime> runtime_;
  OssFileSystem::IObjStore* store_ = nullptr;
};

// The shared vCPU env of a suite in PVFS mode: N executors, the runtime and
// one PvfsObjStore per executor, torn down in the right order.
class PvfsTestEnv {
 public:
  ~PvfsTestEnv() { destroy(); }
  // Builds the env; on failure nothing is left behind.
  int init(const PvfsFileSystem::PvfsRuntimeOptions& opts, int vcpus);
  // Hands the env over to a BackgroundVCpuEnv owner; the runtime stays here
  // and must outlive the file system using it.
  OssFileSystem::BGVCpuObjStoreEnv* release_env() {
    auto e = env_;
    env_ = nullptr;
    return e;
  }
  void destroy();

  PvfsFileSystem::PvfsRuntime* runtime() { return runtime_.get(); }
  OssFileSystem::BGVCpuObjStoreEnv* env() { return env_; }

 private:
  std::unique_ptr<PvfsFileSystem::PvfsRuntime> runtime_;
  OssFileSystem::BGVCpuObjStoreEnv* env_ = nullptr;
};
