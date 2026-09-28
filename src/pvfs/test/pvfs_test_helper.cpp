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

#include "pvfs_test_helper.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/logger.h"
#include "common/macros.h"
#include "options.h"
#include "pvfs_options.h"

using OssFileSystem::BGVCpuObjStoreEnv;
using OssFileSystem::IObjStore;
using OssFileSystem::ObjectList;
using PvfsFileSystem::PvfsObjStore;
using PvfsFileSystem::PvfsRuntime;
using PvfsFileSystem::PvfsRuntimeOptions;

bool is_pvfs_test_mode() { return !FLAGS_pvfs_catalog.empty(); }

int pvfs_test_runtime_options(PvfsRuntimeOptions* opts,
                              const std::string& prefix) {
  int r = PvfsFileSystem::pvfs_runtime_options_from_flags(FLAGS_pvfs_catalog,
                                                          opts);
  if (r != 0) return r;
  opts->readonly = false;  // the suites write; mounts are read-only by default
  opts->prefix = prefix;
  return 0;
}

static std::string root_path(const std::string& osspath) {
  return osspath.empty() || osspath.front() != '/' ? "/" + osspath : osspath;
}

PvfsTestHelper::PvfsTestHelper() = default;

PvfsTestHelper::~PvfsTestHelper() {
  if (store_) {
    perform([&]() { delete store_; });
  }
  runtime_.reset();
  delete executor_;
}

int PvfsTestHelper::init() {
  PvfsRuntimeOptions opts;
  int r = pvfs_test_runtime_options(&opts, "");
  if (r != 0) return r;
  {
    ScopedBlockAllSignal block_signals;
    executor_ = new photon::Executor(OSSFS_EVENT_ENGINE, photon::INIT_IO_NONE,
                                     {}, EXECUTOR_QUEUE_OPTION);
  }
  runtime_ = std::make_unique<PvfsRuntime>(opts);
  r = runtime_->init({executor_});
  if (r != 0) {
    runtime_.reset();
    return r;
  }
  store_ = perform([&]() -> IObjStore * {
    return new PvfsObjStore(runtime_.get(), 0, executor_);
  });
  return 0;
}

int PvfsTestHelper::upload_file(const std::string& local_file,
                                const std::string& osspath) {
  std::string path = root_path(osspath);
  if (path.back() == '/') return create_dir(osspath);
  int fd = ::open(local_file.c_str(), O_RDONLY);
  if (fd < 0) return -errno;
  DEFER(::close(fd));
  struct stat st;
  if (::fstat(fd, &st) != 0) return -errno;
  ssize_t r = perform([&]() -> ssize_t {
    if (st.st_size == 0) {
      iovec iov{nullptr, 0};
      return store_->put_object(path, &iov, 1);
    }
    return store_->put_object_from_fd(path, fd, 0, st.st_size);
  });
  if (r < 0) {
    LOG_ERROR("PVFS helper: upload ` failed: `", path, r);
    return static_cast<int>(r);
  }
  return 0;
}

int PvfsTestHelper::copy_file(const std::string& src, const std::string& dst) {
  return perform([&]() {
    return store_->copy_object(root_path(src), root_path(dst), true);
  });
}

int PvfsTestHelper::delete_file(const std::string& osspath) {
  return perform([&]() { return store_->delete_object(root_path(osspath)); });
}

int PvfsTestHelper::create_dir(const std::string& osspath) {
  std::string path = root_path(osspath);
  if (path.back() != '/') path += '/';
  ssize_t r = perform([&]() -> ssize_t {
    iovec iov{nullptr, 0};
    return store_->put_object(path, &iov, 1);
  });
  return r < 0 ? static_cast<int>(r) : 0;
}

// ossutil rm -r -f: every descendant in batches, then the marker; a
// missing directory is not an error.
int PvfsTestHelper::delete_dir(const std::string& osspath) {
  std::string dir = root_path(osspath);
  if (dir.back() != '/') dir += '/';
  return perform([&]() -> int {
    std::vector<std::string> names;
    int r = store_->list_dir_descendants(dir, names);
    if (r != 0 && r != -ENOENT) return r;
    for (size_t i = 0; i < names.size(); i += 1000) {
      auto end = std::min(names.size(), i + 1000);
      std::vector<std::string_view> batch(names.begin() + i,
                                          names.begin() + end);
      r = store_->delete_objects_under_dir(dir, batch);
      if (r != 0) return r;
    }
    r = store_->delete_object(dir);
    return r == -ENOENT ? 0 : r;
  });
}

int PvfsTestHelper::stat_file(const std::string& osspath) {
  return perform([&]() {
    struct stat st;
    return store_->stat(root_path(osspath), &st, nullptr);
  });
}

std::map<std::string, std::string> PvfsTestHelper::get_file_meta(
    const std::string& osspath) {
  std::map<std::string, std::string> res;
  OssFileSystem::ObjHeaderMeta meta;
  int r = perform(
      [&]() { return store_->head_object(root_path(osspath), meta); });
  if (r != 0) return res;
  if (meta.has_size()) res["Content-Length"] = std::to_string(meta.size);
  if (meta.has_crc64()) res["X-Oss-Hash-Crc64ecma"] = std::to_string(meta.crc64);
  if (meta.has_etag()) {
    // ossutil prints the ETag without its quotes.
    std::string etag = meta.etag;
    if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
      etag = etag.substr(1, etag.size() - 2);
    }
    res["Etag"] = etag;
  }
  if (meta.has_type()) res["X-Oss-Object-Type"] = meta.type;
  return res;
}

std::vector<std::string> PvfsTestHelper::get_list_objects(
    const std::string& osspath, bool include_self) {
  std::vector<std::string> res;
  std::string dir = root_path(osspath);
  if (dir.back() != '/') dir += '/';
  perform([&]() {
    if (include_self) {
      OssFileSystem::ObjHeaderMeta meta;
      if (store_->head_object(dir, meta) == 0) res.emplace_back("");
    }
    std::string marker;
    do {
      ObjectList page;
      int r = store_->list_dir(dir, page, &marker);
      if (r != 0) break;
      for (auto& e : page) {
        res.emplace_back(std::string(e.name()) + (e.is_dir() ? "/" : ""));
      }
    } while (!marker.empty());
    return 0;
  });
  return res;
}

int PvfsTestEnv::init(const PvfsRuntimeOptions& opts, int vcpus) {
  std::vector<photon::Executor*> executors;
  {
    ScopedBlockAllSignal block_signals;
    for (int i = 0; i < vcpus; i++) {
      executors.push_back(new photon::Executor(
          OSSFS_EVENT_ENGINE, photon::INIT_IO_NONE, {}, EXECUTOR_QUEUE_OPTION));
    }
  }
  runtime_ = std::make_unique<PvfsRuntime>(opts);
  int r = runtime_->init(executors);
  if (r != 0) {
    runtime_.reset();
    for (auto e : executors) delete e;
    return r;
  }
  env_ = new BGVCpuObjStoreEnv;
  for (size_t i = 0; i < executors.size(); i++) {
    auto store = executors[i]->perform([&]() -> IObjStore * {
      return new PvfsObjStore(runtime_.get(), i, executors[i]);
    });
    env_->add_obj_store_env(executors[i], store);
  }
  return 0;
}

void PvfsTestEnv::destroy() {
  if (runtime_) runtime_->shutdown();
  runtime_.reset();
  delete env_;
  env_ = nullptr;
}
