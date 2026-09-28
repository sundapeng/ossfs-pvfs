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
#include <stdlib.h>

#include <atomic>
#include <thread>

#include "common/fuse.h"
#include "common/logger.h"
#include "pvfs_test_suite.h"

// Permission errors seen through a RAM user that may only list and
// describe one database and select from one of its tables.
class PvfsPermTest : public PvfsTestSuite {
 protected:
  static constexpr int64_t kRestBoundMs = 5000;
  static constexpr const char* kUngrantedDb = "db_bennett";
  std::string limited_ak_, limited_sk_;

  void SetUp() override {
    PvfsTestSuite::SetUp();
    const char* ak = getenv("PVFS_TEST_LIMITED_AK");
    const char* sk = getenv("PVFS_TEST_LIMITED_SK");
    if (!ak || !*ak || !sk || !*sk) {
      GTEST_SKIP() << "PVFS_TEST_LIMITED_AK/PVFS_TEST_LIMITED_SK not set: "
                      "limited-key permission tests skipped";
    }
    limited_ak_ = ak;
    limited_sk_ = sk;
  }

  PvfsFileSystem::PvfsRuntimeOptions limited_opts(
      const std::string& catalog = "") {
    auto o = base_opts();
    o.rest.access_key_id = limited_ak_;
    o.rest.access_key_secret = limited_sk_;
    o.rest.security_token.clear();
    if (!catalog.empty()) o.rest.catalog = catalog;
    return o;
  }

  // The instance under test signs with the limited key; no janitor pass.
  void init_limited() {
    int err = 0;
    inst_ = make_instance(limited_opts(), &err);
    ASSERT_NE(inst_, nullptr) << err;
    fs_ = inst_->fs;
  }

  // Nodeid of `name` under `parent` as readdirplus hands it out, or 0.
  uint64_t readdirplus_child(uint64_t parent, const std::string& name) {
    std::vector<DirEntry> entries;
    if (list_dir(parent, entries, false) != 0) return 0;
    uint64_t found = 0;
    for (auto& e : entries) {
      if (e.name == name && found == 0) {
        found = e.nodeid;
      } else {
        fs_->forget(e.nodeid, 1);
      }
    }
    return found;
  }

  // Full-key instance that plants and later removes a probe file.
  struct Probe {
    std::unique_ptr<Instance> full;
    uint64_t dir = 0, file = 0, tbl = 0, db = 0;
    std::string path;
  };
  void plant_probe(Probe& pr, const std::string& content) {
    int err = 0;
    pr.full = make_instance(base_opts(), &err);
    ASSERT_NE(pr.full, nullptr) << err;
    FsSwap swap(*this, pr.full->fs);
    ASSERT_EQ(find_table(pr.db, pr.tbl), 0);
    struct stat st;
    ASSERT_EQ(fs_->mkdir(pr.tbl, test_subdir_, 0755, 0, 0, 0, &pr.dir, &st),
              0);
    ASSERT_EQ(create_and_write(pr.dir, "probe.txt", content.data(),
                               content.size(), pr.file),
              static_cast<ssize_t>(content.size()));
    pr.path = "/" + FLAGS_pvfs_test_database + "/" + FLAGS_pvfs_test_table +
              "/" + test_subdir_;
  }
  void remove_probe(Probe& pr) {
    if (!pr.full) return;
    FsSwap swap(*this, pr.full->fs);
    fs_->forget(pr.file, 1);
    remove_tree(pr.tbl, test_subdir_);
    fs_->forget(pr.dir, 1);
    fs_->forget(pr.tbl, 1);
    fs_->forget(pr.db, 1);
  }

  // A catalog whose OpenAPI endpoint is disabled answers the config probe
  // and refuses the first listing: the mount fails with EACCES right away.
  void verify_init_on_disabled_catalog() {
    INIT_PHOTON();
    int err = 0;
    auto t0 = clock_now();
    EXPECT_EQ(make_instance(limited_opts("flink_test"), &err), nullptr);
    EXPECT_EQ(err, -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
  }

  // A database the user may not describe or list: lookup and readdir
  // report the REST status as EACCES rather than a generic EIO.
  void verify_ungranted_database_is_eacces() {
    INIT_PHOTON();
    init_limited();
    // Point lookup first, before a root listing caches the inode.
    uint64_t db = 0;
    struct stat st;
    auto t0 = clock_now();
    EXPECT_EQ(fs_->lookup(root_nodeid_, kUngrantedDb, &db, &st), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);

    std::vector<DirEntry> entries;
    t0 = clock_now();
    ASSERT_EQ(list_dir(root_nodeid_, entries), 0);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, FLAGS_pvfs_test_database);
    uint64_t granted = entries[0].nodeid;
    DEFER(fs_->forget(granted, 1));
    ASSERT_EQ(list_dir(granted, entries), 0);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, FLAGS_pvfs_test_table);
    fs_->forget(entries[0].nodeid, 1);

    // The root listing still names the database; listing it is refused
    // and a table that is not there answers 404, hence ENOENT.
    db = readdirplus_child(root_nodeid_, kUngrantedDb);
    ASSERT_NE(db, 0u);
    DEFER(fs_->forget(db, 1));
    t0 = clock_now();
    EXPECT_EQ(list_dir(db, entries), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    uint64_t tbl = 0;
    t0 = clock_now();
    EXPECT_EQ(fs_->lookup(db, FLAGS_pvfs_test_table, &tbl, &st), -ENOENT);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
  }

  // Within the granted database only the granted table opens; the other
  // one is refused by the catalog with exactly one describe call.
  void verify_ungranted_table_is_eacces() {
    INIT_PHOTON();
    init_limited();
    if (FLAGS_pvfs_test_table2.empty()) {
      GTEST_SKIP() << "--pvfs_test_table2 not set";
    }
    uint64_t db = 0;
    struct stat st;
    ASSERT_EQ(fs_->lookup(root_nodeid_, FLAGS_pvfs_test_database, &db, &st), 0);
    DEFER(fs_->forget(db, 1));
    uint64_t tbl = 0;
    auto t0 = clock_now();
    EXPECT_EQ(fs_->lookup(db, FLAGS_pvfs_test_table2, &tbl, &st), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);

    uint64_t before = rest_stats().get_table.load();
    int err = 0;
    t0 = clock_now();
    EXPECT_EQ(cache()->resolve(FLAGS_pvfs_test_database, FLAGS_pvfs_test_table2,
                               &err),
              nullptr);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    EXPECT_EQ(err, -EACCES);
    EXPECT_EQ(rest_stats().get_table.load() - before, 1u);
    EXPECT_EQ(rest_stats().get_table_token.load(), 0u);
  }

  // SELECT lets the user list and read the table; every write is refused
  // by OSS with EACCES (after one token re-fetch) and leaves no trace.
  void verify_select_only_table() {
    INIT_PHOTON();
    Probe pr;
    plant_probe(pr, "probe-content");
    DEFER(remove_probe(pr));
    init_limited();
    uint64_t dir = 0;
    struct stat st;
    ASSERT_EQ(lookup_path(pr.path, dir, st), 0);
    DEFER(fs_->forget(dir, 1));
    std::vector<DirEntry> entries;
    ASSERT_EQ(list_dir(dir, entries), 0);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "probe.txt");
    uint64_t file = entries[0].nodeid;
    DEFER(fs_->forget(file, 1));
    std::string content;
    ASSERT_EQ(read_file(file, content), 13);
    EXPECT_EQ(content, "probe-content");

    // creat is deferred to the upload, which is refused; no object and,
    // once the kernel lets go, no inode is left behind.
    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(fs_->creat(dir, "new.txt", O_CREAT | O_WRONLY, 0644, 0, 0, 0,
                         &nodeid, &st, &fh),
              0);
    auto t0 = clock_now();
    EXPECT_EQ(fs_->release(nodeid, fh), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    fs_->forget(nodeid, 1);
    t0 = clock_now();
    EXPECT_EQ(fs_->lookup(dir, "new.txt", &nodeid, &st), -ENOENT);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    t0 = clock_now();
    EXPECT_EQ(fs_->mkdir(dir, "newdir", 0755, 0, 0, 0, &nodeid, &st), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    t0 = clock_now();
    EXPECT_EQ(fs_->unlink(dir, "probe.txt"), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    ASSERT_EQ(read_file(file, content), 13);

    // Appending: the buffered write is accepted, the upload is refused and
    // the error sticks to the handle until release.
    bool keep = false;
    ASSERT_EQ(fs_->open(file, O_WRONLY, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    EXPECT_EQ(handle->pwrite("+more", 5, 13), 5);
    t0 = clock_now();
    EXPECT_EQ(fs_->flush(file, fh), -EACCES);
    EXPECT_LT(ms_since(t0), kRestBoundMs);
    EXPECT_LT(handle->pwrite("x", 1, 18), 0);
    EXPECT_LT(fs_->flush(file, fh), 0);
    EXPECT_LT(fs_->release(file, fh), 0);
    ASSERT_EQ(read_file(file, content), 13);
    EXPECT_EQ(content, "probe-content");
    // One forced token fetch in all, not one per refused request.
    EXPECT_EQ(rest_stats().get_table_token.load(), 2u);
    EXPECT_EQ(cache()->forced_refreshes(), 1u);

    // Positive control: the full key may still write here.
    {
      FsSwap swap(*this, pr.full->fs);
      uint64_t ok = 0;
      ASSERT_EQ(create_and_write(pr.dir, "full.txt", "ok", 2, ok), 2);
      fs_->forget(ok, 1);
    }
  }

  // Denied tables must not slow down the granted one: token failures are
  // per table and never hold a lock the readers need.
  void verify_denied_table_does_not_block_reads() {
    INIT_PHOTON();
    if (FLAGS_pvfs_test_table2.empty()) {
      GTEST_SKIP() << "--pvfs_test_table2 not set";
    }
    Probe pr;
    plant_probe(pr, std::string(64 * 1024, 'r'));
    DEFER(remove_probe(pr));
    init_limited();
    uint64_t dir = 0, file = 0;
    struct stat st;
    ASSERT_EQ(lookup_path(pr.path, dir, st), 0);
    DEFER(fs_->forget(dir, 1));
    ASSERT_EQ(fs_->lookup(dir, "probe.txt", &file, &st), 0);
    DEFER(fs_->forget(file, 1));

    std::atomic<int> denied{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 20; i++) {
      workers.emplace_back([&]() {
        INIT_PHOTON();
        int err = 0;
        if (cache()->resolve(FLAGS_pvfs_test_database, FLAGS_pvfs_test_table2,
                             &err) == nullptr &&
            err == -EACCES) {
          denied.fetch_add(1);
        }
      });
    }
    for (int i = 0; i < 5; i++) {
      auto t0 = clock_now();
      std::string content;
      EXPECT_EQ(read_file(file, content), 64 * 1024);
      EXPECT_LT(ms_since(t0), 2000);
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(denied.load(), 20);
  }
};

TEST_F(PvfsPermTest, InitOnDisabledCatalogIsEacces) {
  verify_init_on_disabled_catalog();
}
TEST_F(PvfsPermTest, UngrantedDatabaseIsEacces) {
  verify_ungranted_database_is_eacces();
}
TEST_F(PvfsPermTest, UngrantedTableIsEacces) {
  verify_ungranted_table_is_eacces();
}
TEST_F(PvfsPermTest, SelectOnlyTable) { verify_select_only_table(); }
TEST_F(PvfsPermTest, DeniedTableDoesNotBlockReads) {
  verify_denied_table_does_not_block_reads();
}
