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

#include "pvfs_test_suite.h"

#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>

#include "common/fuse.h"
#include "common/logger.h"

DEFINE_string(pvfs_test_database, "", "Only this database is visible to the tests");
DEFINE_string(pvfs_test_table, "", "Only this table is visible to the tests");
DEFINE_string(pvfs_test_table2, "",
              "Second table in the same database for cross-table tests");
DEFINE_bool(pvfs_test_fast, false, "Skip the slow cases (large listings)");

using OssFileSystem::OssFs;
using OssFileSystem::OssFsOptions;
using PvfsFileSystem::PvfsRuntimeOptions;

PvfsTestSuite::Instance::~Instance() {
  delete fs;  // stops the OssFs threads before its stores go
  env.destroy();
}

int PvfsTestSuite::Instance::init(const PvfsRuntimeOptions& ro,
                                  const OssFsOptions& fo, int vcpus) {
  int r = env.init(ro, vcpus);
  if (r != 0) return r;
  OssFileSystem::BackgroundVCpuEnv bg;
  bg.bg_obj_store_env = env.env();
  fs = new OssFs(fo, bg);
  return fs->init();
}

void PvfsTestSuite::SetUp() {
  auto case_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  // Unique per run so parallel or aborted runs never share a directory.
  case_prefix_ = std::string("pvfs_test_") + case_name + "_";
  test_subdir_ = case_prefix_ + std::to_string(getpid()) + "_" +
                 std::to_string(time(nullptr));
  LOG_INFO("PVFS test setup: `", test_subdir_);
}

void PvfsTestSuite::TearDown() {
  fs_ = nullptr;
  inst_.reset();
  LOG_INFO("PVFS test teardown: `", test_subdir_);
}

PvfsRuntimeOptions PvfsTestSuite::base_opts() {
  PvfsRuntimeOptions opts;
  opts.rest.endpoint = FLAGS_pvfs_endpoint;
  opts.rest.region = FLAGS_pvfs_region;
  opts.rest.catalog = FLAGS_pvfs_catalog;
  opts.rest.access_key_id = FLAGS_pvfs_access_key_id;
  opts.rest.access_key_secret = FLAGS_pvfs_access_key_secret;
  opts.rest.security_token = FLAGS_pvfs_security_token;
  opts.rest.signing_algorithm = FLAGS_pvfs_signing_algorithm;
  opts.rest.enable_ipv6 = false;
  opts.cache.oss_endpoint = FLAGS_pvfs_oss_endpoint;
  opts.cache.enable_ipv6 = false;
  opts.cache.store_options.user_agent = "ossfs2-pvfs-test";
  opts.readonly = false;  // the suite writes; mounts are read-only by default
  return opts;
}

OssFsOptions PvfsTestSuite::base_fs_opts() {
  OssFsOptions fo;
  fo.enable_admin_server = false;  // several instances share the process
  fo.enable_appendable_object = false;
  fo.enable_symlink = false;
  return fo;
}

std::unique_ptr<PvfsTestSuite::Instance> PvfsTestSuite::make_instance(
    const PvfsRuntimeOptions& ro, const OssFsOptions& fo, int* err) {
  auto inst = std::make_unique<Instance>();
  int r = inst->init(ro, fo, vcpus_);
  if (err) *err = r;
  if (r != 0) return nullptr;
  return inst;
}

void PvfsTestSuite::init() {
  ASSERT_FALSE(FLAGS_pvfs_test_database.empty())
      << "--pvfs_test_database is required";
  ASSERT_FALSE(FLAGS_pvfs_test_table.empty())
      << "--pvfs_test_table is required";
  PvfsRuntimeOptions ro = base_opts();
  OssFsOptions fo = base_fs_opts();
  if (upload_part_size_ > 0) fo.upload_buffer_size = upload_part_size_;
  if (max_table_cache_ > 0) ro.cache.max_table_cache = max_table_cache_;
  if (tweak_opts_) tweak_opts_(ro, fo);

  // Leftovers are removed through a throwaway (writable) instance so the
  // instance under test starts with empty caches and zeroed counters.
  if (auto janitor = make_instance(base_opts())) {
    fs_ = janitor->fs;
    purge_stale_subdirs();
    fs_ = nullptr;
  }

  int err = 0;
  inst_ = make_instance(ro, fo, &err);
  ASSERT_NE(inst_, nullptr) << "init failed: " << err;
  fs_ = inst_->fs;
}

int PvfsTestSuite::remove_tree(uint64_t parent, const std::string& name) {
  uint64_t nodeid = 0;
  struct stat st;
  int r = fs_->lookup(parent, name, &nodeid, &st);
  if (r != 0) return r;
  DEFER(fs_->forget(nodeid, 1));
  if (!S_ISDIR(st.st_mode)) return fs_->unlink(parent, name);
  std::vector<DirEntry> entries;
  r = list_dir(nodeid, entries);
  if (r != 0) return r;
  for (auto& e : entries) {
    fs_->forget(e.nodeid, 1);
    r = remove_tree(nodeid, e.name);
    if (r != 0) return r;
  }
  return fs_->rmdir(parent, name);
}

void PvfsTestSuite::purge_stale_subdirs() {
  uint64_t db_nodeid = 0, tbl_nodeid = 0;
  if (find_table(db_nodeid, tbl_nodeid) != 0) return;
  std::vector<DirEntry> entries;
  if (list_dir(tbl_nodeid, entries) == 0) {
    for (auto& e : entries) {
      fs_->forget(e.nodeid, 1);
      if (e.name.compare(0, case_prefix_.size(), case_prefix_) != 0) continue;
      if (e.name == test_subdir_) continue;
      LOG_INFO("PVFS test: purging leftover `", e.name);
      remove_tree(tbl_nodeid, e.name);
    }
  }
  fs_->forget(tbl_nodeid, 1);
  fs_->forget(db_nodeid, 1);
}

int PvfsTestSuite::find_table(uint64_t& db_nodeid, uint64_t& tbl_nodeid) {
  std::vector<DirEntry> dbs;
  int r = list_dir(root_nodeid_, dbs);
  if (r != 0) return r;
  for (const auto& db : dbs) {
    struct stat st;
    r = fs_->lookup(root_nodeid_, db.name, &db_nodeid, &st);
    if (r != 0) continue;
    std::vector<DirEntry> tables;
    r = list_dir(db_nodeid, tables);
    if (r != 0 || tables.empty()) {
      fs_->forget(db_nodeid, 1);
      continue;
    }
    r = fs_->lookup(db_nodeid, tables[0].name, &tbl_nodeid, &st);
    if (r == 0) return 0;
    fs_->forget(db_nodeid, 1);
  }
  return -ENOENT;
}

int PvfsTestSuite::lookup_path(const std::string& path, uint64_t& nodeid,
                               struct stat& st) {
  nodeid = root_nodeid_;
  if (path.empty() || path == "/") {
    return fs_->getattr(nodeid, &st);
  }

  std::string p = path;
  if (p[0] == '/') p = p.substr(1);
  if (!p.empty() && p.back() == '/') p.pop_back();

  std::istringstream ss(p);
  std::string component;
  while (std::getline(ss, component, '/')) {
    if (component.empty()) continue;
    uint64_t child = 0;
    int r = fs_->lookup(nodeid, component, &child, &st);
    if (r != 0) return r;
    if (nodeid != root_nodeid_) {
      fs_->forget(nodeid, 1);
    }
    nodeid = child;
  }
  return 0;
}

int PvfsTestSuite::filler(void* ctx, uint64_t nodeid, const char* name,
                          const struct stat* stbuf, off_t off) {
  auto* entries = reinterpret_cast<std::vector<DirEntry>*>(ctx);
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
  entries->emplace_back(nodeid, name, S_ISDIR(stbuf->st_mode));
  return 0;
}

int PvfsTestSuite::list_dir(uint64_t nodeid, std::vector<DirEntry>& entries,
                            bool filter) {
  entries.clear();
  struct fuse_file_info fi;
  memset(&fi, 0, sizeof(fi));
  int r = fs_->opendir(nodeid, &fi);
  if (r != 0) return r;

  void* dh = reinterpret_cast<void*>(fi.fh);
  r = fs_->readdir(nodeid, 0, dh, filler, &entries, nullptr, true, nullptr);
  fs_->releasedir(nodeid, dh);
  if (r == 0 && filter) filter_entries(nodeid, entries);
  return r;
}

// Hide every database/table except the configured pair so shared catalogs
// are never written to at random. Hidden entries give their reference back.
void PvfsTestSuite::filter_entries(uint64_t nodeid,
                                   std::vector<DirEntry>& entries) {
  std::string keep;
  if (nodeid == root_nodeid_) {
    keep = FLAGS_pvfs_test_database;
  } else if (!FLAGS_pvfs_test_database.empty() &&
             !FLAGS_pvfs_test_table.empty()) {
    uint64_t db_nodeid = 0;
    struct stat st;
    if (fs_->lookup(root_nodeid_, FLAGS_pvfs_test_database, &db_nodeid, &st) ==
        0) {
      if (db_nodeid == nodeid) keep = FLAGS_pvfs_test_table;
      fs_->forget(db_nodeid, 1);
    }
  }
  if (keep.empty()) return;
  for (auto& e : entries) {
    if (e.name != keep) fs_->forget(e.nodeid, 1);
  }
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const DirEntry& e) { return e.name != keep; }),
                entries.end());
}

ssize_t PvfsTestSuite::create_and_write(uint64_t parent,
                                        const std::string& name,
                                        const void* data, size_t size,
                                        uint64_t& nodeid) {
  struct stat st;
  void* fh = nullptr;
  int r = fs_->creat(parent, name, O_CREAT | O_RDWR, 0644, 0, 0, 0, &nodeid,
                     &st, &fh);
  if (r != 0) return r;

  auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
  ssize_t written = handle->pwrite(data, size, 0);

  r = fs_->release(nodeid, fh);
  if (written >= 0 && r != 0) return r;
  return written;
}

ssize_t PvfsTestSuite::read_file(uint64_t nodeid, std::string& content) {
  struct stat st;
  int r = fs_->getattr(nodeid, &st);
  if (r != 0) return r;

  void* fh = nullptr;
  bool keep_page_cache = false;
  r = fs_->open(nodeid, O_RDONLY, &fh, &keep_page_cache);
  if (r != 0) return r;

  content.resize(st.st_size);
  auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
  ssize_t bytes_read =
      st.st_size == 0 ? 0 : handle->pread(content.data(), st.st_size, 0);

  fs_->release(nodeid, fh);
  if (bytes_read >= 0) content.resize(bytes_read);
  return bytes_read;
}
