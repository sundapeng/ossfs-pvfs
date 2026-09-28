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

#include "common/fuse.h"

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "common/logger.h"

class PvfsEdgeTest : public PvfsTestSuite {
 protected:
  void verify_symlink_not_supported() {
    INIT_PHOTON();
    init();
    uint64_t nodeid = 0;
    struct stat st;
    int r = fs_->symlink(root_nodeid_, "test_link", "target", 0, 0, &nodeid,
                         &st);
    ASSERT_EQ(r, -ENOTSUP);
    char buf[256];
    ssize_t len = fs_->readlink(root_nodeid_, buf, sizeof(buf));
    ASSERT_EQ(len, -ENOTSUP);
  }

  void verify_virtual_level_mkdir_fails() {
    INIT_PHOTON();
    init();
    // mkdir at root would create a "database": refused, but never ENOSYS
    uint64_t nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(root_nodeid_, "fake_database", 0755, 0, 0, 0, &nodeid,
                       &st);
    ASSERT_EQ(r, -EPERM);

    // Cannot mkdir at database level (would create a "table" via filesystem)
    std::vector<DirEntry> dbs;
    r = list_dir(root_nodeid_, dbs);
    ASSERT_EQ(r, 0);
    if (dbs.empty()) return;

    uint64_t db_nodeid = 0;
    r = fs_->lookup(root_nodeid_, dbs[0].name, &db_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER(fs_->forget(db_nodeid, 1));
    fs_->forget(dbs[0].nodeid, 1);
    r = fs_->mkdir(db_nodeid, "fake_table", 0755, 0, 0, 0, &nodeid, &st);
    ASSERT_EQ(r, -EPERM);
  }

  // creat is refused in the call itself, not at flush.
  void verify_virtual_level_creat_fails() {
    INIT_PHOTON();
    init();
    uint64_t nodeid = 0;
    struct stat st;
    void* fh = nullptr;
    int r = fs_->creat(root_nodeid_, "fake_file", O_CREAT | O_RDWR, 0644, 0,
                       0, 0, &nodeid, &st, &fh);
    ASSERT_EQ(r, -EPERM);

    std::vector<DirEntry> dbs;
    r = list_dir(root_nodeid_, dbs);
    ASSERT_EQ(r, 0);
    if (dbs.empty()) return;

    uint64_t db_nodeid = 0;
    r = fs_->lookup(root_nodeid_, dbs[0].name, &db_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER(fs_->forget(db_nodeid, 1));
    fs_->forget(dbs[0].nodeid, 1);
    r = fs_->creat(db_nodeid, "fake_file", O_CREAT | O_RDWR, 0644, 0, 0, 0,
                   &nodeid, &st, &fh);
    ASSERT_EQ(r, -EPERM);
  }

  void pinned_table(std::string& db, std::string& tbl, uint64_t& db_nodeid,
                    uint64_t& tbl_nodeid) {
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    std::vector<DirEntry> dbs;
    ASSERT_EQ(list_dir(root_nodeid_, dbs), 0);
    db = dbs[0].name;
    fs_->forget(dbs[0].nodeid, 1);
    std::vector<DirEntry> tables;
    ASSERT_EQ(list_dir(db_nodeid, tables), 0);
    tbl = tables[0].name;
    fs_->forget(tables[0].nodeid, 1);
  }

  // With max_table_cache=1 every miss on another table evicts the pinned
  // one; a reader opened before keeps reading, the evicted store is
  // deleted on its vCPU, and the table comes back on the next use.
  void verify_table_cache_eviction() {
    INIT_PHOTON();
    max_table_cache_ = 1;
    init();
    std::string db, tbl;
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    pinned_table(db, tbl, db_nodeid, tbl_nodeid);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    uint64_t dir = 0;
    struct stat st;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, test_subdir_, 0755, 0, 0, 0, &dir, &st),
              0);
    uint64_t f = 0;
    std::string data(64 * 1024, 'e');
    ASSERT_EQ(create_and_write(dir, "e", data.data(), data.size(), f),
              (ssize_t)data.size());
    DEFER({
      fs_->unlink(dir, "e");
      fs_->rmdir(tbl_nodeid, test_subdir_);
      fs_->forget(f, 1);
      fs_->forget(dir, 1);
    });
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(f, O_RDONLY, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    std::string buf(data.size(), '\0');
    ASSERT_EQ(handle->pread(&buf[0], 4096, 0), 4096);

    for (int i = 0; i < 3; i++) {
      // Each miss inserts a new entry and evicts the previous one.
      int err = 0;
      ASSERT_EQ(cache()->resolve(db, "no_such_table_" + std::to_string(i),
                                 &err),
                nullptr);
      ASSERT_EQ(err, -ENOENT);
    }
    // The reader is served by whichever store the table has now.
    ASSERT_EQ(handle->pread(&buf[0], buf.size(), 0), (ssize_t)data.size());
    ASSERT_EQ(buf, data);
    ASSERT_EQ(fs_->release(f, fh), 0);
    cache()->reap_retired_stores();
    ASSERT_GE(cache()->stores_reaped(), 1u);
    int err = 0;
    ASSERT_NE(cache()->resolve(db, tbl, &err), nullptr);
    std::string content;
    ASSERT_EQ(read_file(f, content), (ssize_t)data.size());
  }

  // A huge refresh-ahead makes every token look due; the background loop
  // must refresh it on its own executor without a crash.
  void verify_background_refresh() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions& o,
                     OssFileSystem::OssFsOptions&) {
      o.cache.credential_refresh_ahead_sec = 10 * 365 * 24 * 3600;  // always due
      o.cache.cred_refresh_interval_sec = 1;
    };
    init();
    std::string db, tbl;
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    pinned_table(db, tbl, db_nodeid, tbl_nodeid);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    ASSERT_NE(cache()->resolve(db, tbl), nullptr);
    uint64_t gen0 = 0;
    ASSERT_TRUE(cache()->credential_generation(db, tbl, &gen0));
    uint64_t gen = gen0;
    for (int i = 0; i < 20 && gen == gen0; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      ASSERT_TRUE(cache()->credential_generation(db, tbl, &gen));
    }
    ASSERT_GT(gen, gen0);
    // The stores pick the new token up in place: data still flows.
    std::vector<DirEntry> entries;
    ASSERT_EQ(list_dir(tbl_nodeid, entries), 0);
    for (auto& e : entries) fs_->forget(e.nodeid, 1);
  }

  // Callers never wait for a refresh in progress: they keep using the
  // current token while the background loop is stuck in a slow refresh.
  void verify_no_blocking_during_refresh() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions& o,
                     OssFileSystem::OssFsOptions&) {
      o.cache.credential_refresh_ahead_sec = 10 * 365 * 24 * 3600;  // always due
      o.cache.cred_refresh_interval_sec = 1;
    };
    init();
    std::string db, tbl;
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    pinned_table(db, tbl, db_nodeid, tbl_nodeid);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));
    ASSERT_NE(cache()->resolve(db, tbl), nullptr);

    cache()->set_refresh_fault_hook_for_test([]() {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      return 0;
    });
    DEFER(cache()->set_refresh_fault_hook_for_test(nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // in flight

    std::atomic<int> failures{0};
    std::atomic<int64_t> worst_ms{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; t++) {
      workers.emplace_back([&]() {
        INIT_PHOTON();
        auto t0 = std::chrono::steady_clock::now();
        auto entry = cache()->resolve(db, tbl);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        if (!entry) failures.fetch_add(1);
        int64_t seen = worst_ms.load();
        while (ms > seen && !worst_ms.compare_exchange_weak(seen, ms)) {
        }
      });
    }
    for (auto& w : workers) w.join();
    ASSERT_EQ(failures.load(), 0);
    ASSERT_LT(worst_ms.load(), 1000);
  }

  // A failing refresh must not turn a still valid token into EIO.
  void verify_failed_refresh_keeps_env() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions& o,
                     OssFileSystem::OssFsOptions&) {
      o.cache.credential_refresh_ahead_sec = 10 * 365 * 24 * 3600;  // always due
    };
    init();
    std::string db, tbl;
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    pinned_table(db, tbl, db_nodeid, tbl_nodeid);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    ASSERT_NE(cache()->resolve(db, tbl), nullptr);
    uint64_t gen1 = 0;
    ASSERT_TRUE(cache()->credential_generation(db, tbl, &gen1));
    cache()->set_refresh_fault_hook_for_test([]() { return -EIO; });
    DEFER(cache()->set_refresh_fault_hook_for_test(nullptr));
    ASSERT_EQ(cache()->refresh(db, tbl), -EIO);
    ASSERT_NE(cache()->resolve(db, tbl), nullptr);
    uint64_t gen2 = 0;
    ASSERT_TRUE(cache()->credential_generation(db, tbl, &gen2));
    ASSERT_EQ(gen2, gen1);
    std::vector<DirEntry> entries;
    ASSERT_EQ(list_dir(tbl_nodeid, entries), 0);
    for (auto& e : entries) fs_->forget(e.nodeid, 1);
  }

  // A wrong catalog or wrong credentials must fail init(), i.e. the
  // mount, instead of failing every request afterwards.
  void verify_init_rejects_bad_config() {
    INIT_PHOTON();
    int err = 0;
    {
      auto opts = base_opts();
      opts.rest.catalog = "no_such_catalog_pvfs_test";
      ASSERT_EQ(make_instance(opts, &err), nullptr);
      ASSERT_NE(err, 0);
    }
    {
      auto opts = base_opts();
      opts.rest.access_key_secret = "not-the-secret";
      ASSERT_EQ(make_instance(opts, &err), nullptr);
      ASSERT_NE(err, 0);
    }
    {
      auto opts = base_opts();
      opts.rest.access_key_id = "LTAIbogus";
      ASSERT_EQ(make_instance(opts, &err), nullptr);
      ASSERT_NE(err, 0);
    }
  }

  // REST errors keep their meaning: a missing table is ENOENT.
  void verify_missing_table_is_enoent() {
    INIT_PHOTON();
    init();
    std::string db, tbl;
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    pinned_table(db, tbl, db_nodeid, tbl_nodeid);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));
    int err = 0;
    ASSERT_EQ(cache()->resolve(db, "no_such_table_pvfs_test", &err), nullptr);
    ASSERT_EQ(err, -ENOENT);
    ASSERT_EQ(cache()->resolve("no_such_db_pvfs_test", tbl, &err), nullptr);
    ASSERT_EQ(err, -ENOENT);
    uint64_t nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->lookup(db_nodeid, "no_such_table_pvfs_test", &nodeid, &st),
              -ENOENT);
  }

  // Endpoint schemes: TLS by default, plaintext only when asked for.
  void verify_endpoint_scheme() {
    using PvfsFileSystem::PaimonRESTClient;
    using PvfsFileSystem::PvfsTableCache;
    auto r = PaimonRESTClient::split_scheme("dlfnext.cn-hangzhou.aliyuncs.com",
                                            true);
    ASSERT_EQ(r.first, "dlfnext.cn-hangzhou.aliyuncs.com");
    ASSERT_TRUE(r.second);
    r = PaimonRESTClient::split_scheme("http://dlf-vpc.example.com", true);
    ASSERT_EQ(r.first, "dlf-vpc.example.com");
    ASSERT_FALSE(r.second);
    r = PaimonRESTClient::split_scheme("https://dlf-vpc.example.com", false);
    ASSERT_EQ(r.first, "dlf-vpc.example.com");
    ASSERT_TRUE(r.second);
    r = PaimonRESTClient::split_scheme("dlf-vpc.example.com", false);
    ASSERT_FALSE(r.second);
    ASSERT_EQ(
        PvfsTableCache::normalize_oss_endpoint("oss-cn-hangzhou.aliyuncs.com"),
        "https://oss-cn-hangzhou.aliyuncs.com");
    ASSERT_EQ(PvfsTableCache::normalize_oss_endpoint("http://oss-internal.local"),
              "http://oss-internal.local");
    ASSERT_EQ(PvfsTableCache::normalize_oss_endpoint("https://oss.example"),
              "https://oss.example");
  }

  // Databases and tables are looked up with point calls, not by paging
  // through a listing.
  void verify_point_lookups() {
    INIT_PHOTON();
    init();
    const auto& st = rest_stats();
    uint64_t list_dbs = st.list_databases, list_tbls = st.list_tables;
    uint64_t get_db = st.get_database, get_tbl = st.get_table;
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    struct stat stbuf;
    ASSERT_EQ(fs_->lookup(root_nodeid_, FLAGS_pvfs_test_database, &db_nodeid,
                          &stbuf),
              0);
    DEFER(fs_->forget(db_nodeid, 1));
    ASSERT_EQ(fs_->lookup(db_nodeid, FLAGS_pvfs_test_table, &tbl_nodeid,
                          &stbuf),
              0);
    DEFER(fs_->forget(tbl_nodeid, 1));
    ASSERT_EQ(st.get_database, get_db + 1);
    ASSERT_EQ(st.get_table, get_tbl + 1);
    ASSERT_EQ(st.list_databases, list_dbs);
    ASSERT_EQ(st.list_tables, list_tbls);
  }

  // Listings follow nextPageToken: with one entry per page the pinned
  // database's tables still all show up.
  void verify_list_paging() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions& o,
                     OssFileSystem::OssFsOptions&) {
      o.rest.list_page_size = 1;
    };
    init();
    if (FLAGS_pvfs_test_table2.empty()) {
      GTEST_SKIP() << "--pvfs_test_table2 not set, paging needs two tables";
    }
    uint64_t db_nodeid = 0;
    struct stat stbuf;
    ASSERT_EQ(fs_->lookup(root_nodeid_, FLAGS_pvfs_test_database, &db_nodeid,
                          &stbuf),
              0);
    DEFER(fs_->forget(db_nodeid, 1));
    const auto& st = rest_stats();
    uint64_t calls = st.list_tables;
    std::vector<DirEntry> tables;
    int r = list_dir(db_nodeid, tables, false);  // every table
    ASSERT_EQ(r, 0);
    std::vector<std::string> names;
    for (auto& t : tables) {
      names.push_back(t.name);
      fs_->forget(t.nodeid, 1);
    }
    ASSERT_NE(std::find(names.begin(), names.end(), FLAGS_pvfs_test_table),
              names.end());
    ASSERT_NE(std::find(names.begin(), names.end(), FLAGS_pvfs_test_table2),
              names.end());
    ASSERT_GE(st.list_tables - calls, 1u);
    std::vector<DirEntry> dbs;
    ASSERT_EQ(list_dir(root_nodeid_, dbs), 0);
    ASSERT_EQ(dbs.size(), 1u);
    ASSERT_EQ(dbs[0].name, FLAGS_pvfs_test_database);
    fs_->forget(dbs[0].nodeid, 1);
  }

  // Renames never cross a table: the source and destination tables have
  // different OSS locations and credentials.
  void verify_cross_table_rename_fails() {
    INIT_PHOTON();
    init();
    if (FLAGS_pvfs_test_table2.empty()) {
      GTEST_SKIP() << "--pvfs_test_table2 not set, cross-table rename skipped";
    }
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));
    uint64_t tbl2_nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->lookup(db_nodeid, FLAGS_pvfs_test_table2, &tbl2_nodeid, &st),
              0);
    DEFER(fs_->forget(tbl2_nodeid, 1));

    uint64_t dir_nodeid = 0;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, test_subdir_, 0755, 0, 0, 0, &dir_nodeid,
                         &st),
              0);
    DEFER({
      fs_->unlink(dir_nodeid, "f");
      fs_->rmdir(tbl_nodeid, test_subdir_);
      fs_->forget(dir_nodeid, 1);
    });
    uint64_t f_nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "f", "x", 1, f_nodeid), 1);
    DEFER(fs_->forget(f_nodeid, 1));

    ASSERT_EQ(fs_->rename(dir_nodeid, "f", tbl2_nodeid, "f", 0), -EXDEV);
    ASSERT_EQ(fs_->rename(tbl_nodeid, test_subdir_, tbl2_nodeid, test_subdir_,
                          0),
              -EXDEV);
    // The source is untouched.
    uint64_t again = 0;
    ASSERT_EQ(fs_->lookup(dir_nodeid, "f", &again, &st), 0);
    fs_->forget(again, 1);
  }

  // Every mutating operation is refused at the virtual levels, and none of
  // them is ENOSYS.
  void verify_virtual_level_ops_refused() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));
    std::string db = FLAGS_pvfs_test_database, tbl = FLAGS_pvfs_test_table;
    struct stat st;
    auto refused = [](int r) {
      return r == -EPERM || r == -EISDIR || r == -ENOTEMPTY;
    };
    EXPECT_TRUE(refused(fs_->unlink(root_nodeid_, db)));
    EXPECT_TRUE(refused(fs_->unlink(db_nodeid, tbl)));
    EXPECT_TRUE(refused(fs_->rmdir(root_nodeid_, db)));
    EXPECT_TRUE(refused(fs_->rmdir(db_nodeid, tbl)));
    memset(&st, 0, sizeof(st));
    EXPECT_EQ(fs_->setattr(db_nodeid, &st, FUSE_SET_ATTR_SIZE), -EISDIR);
    EXPECT_EQ(fs_->setattr(tbl_nodeid, &st, FUSE_SET_ATTR_SIZE), -EISDIR);
    EXPECT_EQ(fs_->rename(root_nodeid_, db, root_nodeid_, db + "_x", 0),
              -EPERM);
    EXPECT_EQ(fs_->rename(db_nodeid, tbl, db_nodeid, tbl + "_x", 0), -EPERM);
    // Nothing changed.
    uint64_t again = 0;
    ASSERT_EQ(fs_->lookup(db_nodeid, tbl, &again, &st), 0);
    ASSERT_EQ(again, tbl_nodeid);
    fs_->forget(again, 1);
  }

  PvfsFileSystem::PaimonRESTClientOptions rest_opts(const std::string& signing) {
    auto r = base_opts().rest;
    r.signing_algorithm = signing;
    return r;
  }

  // The server verifies signatures: the algorithm matching the endpoint
  // lists the catalog, the other one is rejected.
  void verify_signing_control_pair() {
    INIT_PHOTON();
    bool openapi = FLAGS_pvfs_endpoint.find("dlfnext") != std::string::npos;
    PvfsFileSystem::PaimonRESTClient good(rest_opts(openapi ? "openapi"
                                                            : "default"));
    ASSERT_EQ(good.init(), 0);
    std::vector<std::string> dbs;
    ASSERT_EQ(good.list_databases(dbs), 0);
    ASSERT_GT(dbs.size(), 0u);

    // The gateway answers a wrongly signed request with a 4xx (the public
    // one says 404); whatever it is, the mount must not come up.
    PvfsFileSystem::PaimonRESTClient bad(rest_opts(openapi ? "default"
                                                           : "openapi"));
    int r = bad.init();
    ASSERT_NE(r, 0);
    ASSERT_TRUE(r == -EACCES || r == -EINVAL || r == -ENOENT) << r;
    std::vector<std::string> none;
    ASSERT_NE(bad.list_databases(none), 0);
  }

  // The token contract PVFS builds on.
  void verify_token_contract() {
    INIT_PHOTON();
    PvfsFileSystem::PaimonRESTClient client(rest_opts("auto"));
    ASSERT_EQ(client.init(), 0);
    PvfsFileSystem::TableInfo info;
    ASSERT_EQ(client.get_table(FLAGS_pvfs_test_database, FLAGS_pvfs_test_table,
                               info),
              0);
    ASSERT_EQ(info.location.compare(0, 6, "oss://"), 0) << info.location;
    auto slash = info.location.find('/', 6);
    ASSERT_NE(slash, std::string::npos) << info.location;
    ASSERT_GT(slash, 6u);                           // bucket
    ASSERT_LT(slash + 1, info.location.size());  // prefix

    PvfsFileSystem::TableCredential cred;
    ASSERT_EQ(client.get_table_token(FLAGS_pvfs_test_database,
                                     FLAGS_pvfs_test_table, cred),
              0);
    ASSERT_FALSE(cred.access_key_id.empty());
    ASSERT_FALSE(cred.access_key_secret.empty());
    ASSERT_FALSE(cred.security_token.empty());
    time_t now = time(nullptr);
    ASSERT_GT(cred.expiration_sec, now);
    ASSERT_LT(cred.expiration_sec, now + 24 * 3600);
  }

  void verify_getattr_database() {
    INIT_PHOTON();
    init();
    std::vector<DirEntry> dbs;
    int r = list_dir(root_nodeid_, dbs);
    ASSERT_EQ(r, 0);
    ASSERT_GT(dbs.size(), 0UL);
    uint64_t db_nodeid = 0;
    struct stat st;
    r = fs_->lookup(root_nodeid_, dbs[0].name, &db_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER(fs_->forget(db_nodeid, 1));
    fs_->forget(dbs[0].nodeid, 1);
    // getattr on virtual level should succeed
    r = fs_->getattr(db_nodeid, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
  }

  void verify_forget_root_noop() {
    INIT_PHOTON();
    init();
    // Forgetting root should be a no-op
    int r = fs_->forget(root_nodeid_, 1);
    ASSERT_EQ(r, 0);
    // Root should still work
    struct stat st;
    r = fs_->getattr(root_nodeid_, &st);
    ASSERT_EQ(r, 0);
  }
};

TEST_F(PvfsEdgeTest, verify_symlink_not_supported) {
  verify_symlink_not_supported();
}
TEST_F(PvfsEdgeTest, verify_virtual_level_mkdir_fails) {
  verify_virtual_level_mkdir_fails();
}
TEST_F(PvfsEdgeTest, verify_virtual_level_creat_fails) {
  verify_virtual_level_creat_fails();
}
TEST_F(PvfsEdgeTest, verify_table_cache_eviction) {
  verify_table_cache_eviction();
}
TEST_F(PvfsEdgeTest, verify_background_refresh) { verify_background_refresh(); }
TEST_F(PvfsEdgeTest, verify_no_blocking_during_refresh) {
  verify_no_blocking_during_refresh();
}
TEST_F(PvfsEdgeTest, verify_failed_refresh_keeps_env) {
  verify_failed_refresh_keeps_env();
}
TEST_F(PvfsEdgeTest, verify_init_rejects_bad_config) {
  verify_init_rejects_bad_config();
}
TEST_F(PvfsEdgeTest, verify_missing_table_is_enoent) {
  verify_missing_table_is_enoent();
}
TEST_F(PvfsEdgeTest, verify_endpoint_scheme) { verify_endpoint_scheme(); }
TEST_F(PvfsEdgeTest, verify_point_lookups) { verify_point_lookups(); }
TEST_F(PvfsEdgeTest, verify_list_paging) { verify_list_paging(); }
TEST_F(PvfsEdgeTest, verify_virtual_level_ops_refused) {
  verify_virtual_level_ops_refused();
}
TEST_F(PvfsEdgeTest, verify_signing_control_pair) {
  verify_signing_control_pair();
}
TEST_F(PvfsEdgeTest, verify_token_contract) { verify_token_contract(); }
TEST_F(PvfsEdgeTest, verify_cross_table_rename_fails) {
  verify_cross_table_rename_fails();
}
TEST_F(PvfsEdgeTest, verify_getattr_database) { verify_getattr_database(); }
TEST_F(PvfsEdgeTest, verify_forget_root_noop) { verify_forget_root_noop(); }
