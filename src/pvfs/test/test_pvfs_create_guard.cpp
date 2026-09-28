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

#include <fcntl.h>

#include <string>
#include <vector>

#include "common/utils.h"
#include "fs/test/test_suite.h"

// Creates that PvfsObjStore refuses fail in creat itself, through OssFs,
// instead of at flush: the OssFs is rooted at the catalog so the virtual
// levels are reachable, and oss_bucket_prefix (db/table/scratch) is walked
// from there.
class PvfsCreateGuardTest : public Ossfs2TestSuite {
 protected:
  uint64_t db_ = 0, tbl_ = 0, scratch_ = 0;
  std::vector<uint64_t> held_;

  void SetUp() override {
    if (!is_pvfs_test_mode()) GTEST_SKIP() << "PVFS mode only";
    if (split_string(FLAGS_oss_bucket_prefix, "/").size() < 2) {
      GTEST_SKIP() << "needs --oss_bucket_prefix=db/table[/dir]";
    }
    Ossfs2TestSuite::SetUp();
  }
  void TearDown() override {
    for (auto it = held_.rbegin(); it != held_.rend(); ++it) {
      fs_->forget(*it, 1);
    }
    held_.clear();
    Ossfs2TestSuite::TearDown();
  }
  uint64_t walk(uint64_t parent, const std::string &name) {
    uint64_t nodeid = 0;
    struct stat st;
    EXPECT_EQ(fs_->lookup(parent, name, &nodeid, &st), 0) << name;
    if (nodeid) held_.push_back(nodeid);
    return nodeid;
  }
  // Root, database, table and the suite's scratch directory of this case.
  void resolve_tree() {
    auto parts = split_string(FLAGS_oss_bucket_prefix, "/");
    ASSERT_GE(parts.size(), 2u) << "prefix must be db/table[/dir]";
    db_ = walk(root_nodeid_, std::string(parts[0]));
    tbl_ = walk(db_, std::string(parts[1]));
    uint64_t dir = tbl_;
    for (size_t i = 2; i < parts.size() && dir; i++) {
      dir = walk(dir, std::string(parts[i]));
    }
    scratch_ = walk(dir, gtest_base_dir);
    ASSERT_NE(scratch_, 0u);
  }
  int creat(uint64_t parent, const char *name, void **fh, uint64_t *nodeid) {
    struct stat st;
    return fs_->creat(parent, name, O_CREAT | O_WRONLY, 0644, 0, 0, 0, nodeid,
                      &st, fh);
  }
  int mkdir(uint64_t parent, const char *name) {
    uint64_t nodeid = 0;
    struct stat st;
    return fs_->mkdir(parent, name, 0755, 0, 0, 0, &nodeid, &st);
  }
};

TEST_F(PvfsCreateGuardTest, CreatIsRefusedAtOnce) {
  INIT_PHOTON();
  OssFsOptions opts;
  pvfs_prefix_ = "";
  init(opts);
  resolve_tree();
  uint64_t snapshot = 0;
  struct stat st;
  ASSERT_EQ(fs_->lookup(tbl_, "snapshot", &snapshot, &st), 0)
      << "the pinned table has no snapshot directory";
  held_.push_back(snapshot);
  auto *rt = pvfs_env_->runtime();
  auto &stats = rt->rest_client()->stats();
  uint64_t heads = rt->oss_stat_calls(), tokens = stats.get_table_token.load();

  void *fh = nullptr;
  uint64_t nodeid = 0;
  // A database or a table cannot be made through the mount.
  EXPECT_EQ(creat(root_nodeid_, "new_db", &fh, &nodeid), -EPERM);
  EXPECT_EQ(creat(db_, "new_table", &fh, &nodeid), -EPERM);
  EXPECT_EQ(mkdir(root_nodeid_, "new_db"), -EPERM);
  EXPECT_EQ(mkdir(db_, "new_table"), -EPERM);
  EXPECT_EQ(fs_->symlink(db_, "l", "t", 0, 0, &nodeid, nullptr), -ENOTSUP);
  // Paimon's metadata directories are refused before any request.
  EXPECT_EQ(creat(snapshot, "LATEST", &fh, &nodeid), -EPERM);
  EXPECT_EQ(creat(snapshot, "snapshot-9", &fh, &nodeid), -EPERM);
  EXPECT_EQ(mkdir(tbl_, "manifest"), -EPERM);
  EXPECT_EQ(mkdir(tbl_, "bucket-0"), -EPERM);
  EXPECT_EQ(rt->reserved_write_refusals(), 3u);
  EXPECT_EQ(rt->oss_stat_calls(), heads) << "refusals must not probe OSS";
  EXPECT_EQ(stats.get_table_token.load(), tokens);

  // An ordinary create in user space still works end to end.
  ASSERT_EQ(creat(scratch_, "data_x", &fh, &nodeid), 0);
  held_.push_back(nodeid);
  auto *handle = static_cast<IFileHandleFuseLL *>(fh);
  ASSERT_EQ(handle->pwrite("ok", 2, 0), 2);
  ASSERT_EQ(fs_->release(nodeid, fh), 0);
  uint64_t again = 0;
  ASSERT_EQ(fs_->lookup(scratch_, "data_x", &again, &st), 0);
  EXPECT_EQ(st.st_size, 2);
  fs_->forget(again, 1);
  ASSERT_EQ(fs_->unlink(scratch_, "data_x"), 0);
}

TEST_F(PvfsCreateGuardTest, ReadOnlyMountRefusesCreatAtOnce) {
  INIT_PHOTON();
  OssFsOptions opts;
  pvfs_prefix_ = "";
  pvfs_readonly_ = true;
  init(opts);
  resolve_tree();
  auto *rt = pvfs_env_->runtime();
  uint64_t heads = rt->oss_stat_calls();
  void *fh = nullptr;
  uint64_t nodeid = 0;
  EXPECT_EQ(creat(scratch_, "ro_x", &fh, &nodeid), -EROFS);
  EXPECT_EQ(mkdir(scratch_, "ro_d"), -EROFS);
  EXPECT_EQ(creat(db_, "new_table", &fh, &nodeid), -EPERM);  // shape first
  EXPECT_EQ(rt->oss_stat_calls(), heads);
  struct stat st;
  EXPECT_EQ(fs_->lookup(scratch_, "ro_x", &nodeid, &st), -ENOENT);
}
