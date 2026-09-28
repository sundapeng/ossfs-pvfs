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

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "common/logger.h"
#include "fs/bg_vcpu_env.h"
#include "options.h"
#include "pvfs/pvfs_obj_store.h"
#include "pvfs/pvfs_runtime.h"
#include "pvfs_rest_stub.h"
#include "pvfs_test_helper.h"
#include "pvfs_test_suite.h"

using OssFileSystem::IObjStore;
using OssFileSystem::ObjectList;
using PvfsFileSystem::PvfsRuntimeOptions;

namespace {

std::vector<std::string> names_of(const ObjectList& list) {
  std::vector<std::string> names;
  for (auto& e : list) names.emplace_back(e.name());
  std::sort(names.begin(), names.end());
  return names;
}

// One PvfsObjStore per executor over a stub catalog, driven the way OssFs
// drives its stores: every call performed on the next vCPU.
class PvfsObjStoreStubTest : public RestStubFixture {
 protected:
  int databases_status_ = 200;
  bool down_ = false;
  // The stub also plays OSS for the table stores (path-style URLs): the
  // token of the first /token call is refused, later ones are accepted.
  int oss_status_ = 200;
  int token_calls_ = 0;
  int token_hold_ms_ = 0;  // delay of every /token answer
  PvfsTestEnv env_;

  void install_routes() {
    stub_.route = [this](const std::string& t,
                         const std::string&) -> std::pair<int, std::string> {
      if (down_) return {503, "{}"};
      if (t.compare(0, 8, "/bucket/") == 0) {
        if (oss_status_ != 200) return {oss_status_, ""};
        return {stub_.last_security_token == "tok1" ? 403 : 200, ""};
      }
      if (t == "/v1/config") return {200, R"({"defaults":{"prefix":"pfx"}})"};
      const std::string dbs = "/v1/pfx/databases";
      if (t == dbs) {
        if (databases_status_ != 200) return {databases_status_, "{}"};
        return {200, R"({"databases":["db","db2"]})"};
      }
      if (t == dbs + "/db" || t == dbs + "/db2") return {200, "{}"};
      if (t == dbs + "/db/tables") return {200, R"({"tables":["t","t2"]})"};
      if (t == dbs + "/db2/tables") return {200, R"({"tables":[]})"};
      const std::string tp = dbs + "/db/tables/";
      if (t.compare(0, tp.size(), tp) != 0) return {404, "{}"};
      std::string name = t.substr(tp.size());
      bool token = false;
      if (name.size() > 6 && name.compare(name.size() - 6, 6, "/token") == 0) {
        token = true;
        name.resize(name.size() - 6);
      }
      if (name != "t" && name != "t2") return {404, "{}"};
      if (token) {
        if (token_hold_ms_ > 0) stub_.hold_ms(token_hold_ms_);
        int64_t exp = (time(nullptr) + 3600) * 1000;
        std::string tok = "tok" + std::to_string(++token_calls_);
        return {200,
                R"({"token":{"fs.oss.accessKeyId":"ak","fs.oss.accessKeySecret":"sk",)"
                R"("fs.oss.securityToken":")" +
                    tok +
                    R"(","fs.oss.endpoint":"oss-cn-test.aliyuncs.com"},)"
                    "\"expiresAtMillis\":" +
                    std::to_string(exp) + "}"};
      }
      return {200, "{\"location\":\"oss://bucket/warehouse/db.db/" + name +
                       "\"}"};
    };
  }

  PvfsRuntimeOptions opts(const std::string& prefix = "",
                          bool readonly = false) {
    PvfsRuntimeOptions o;
    o.rest.endpoint = endpoint();
    o.rest.region = "cn-test";
    o.rest.catalog = "cat";
    o.rest.access_key_id = "ak";
    o.rest.access_key_secret = "sk";
    o.rest.signing_algorithm = "default";
    o.rest.timeout_ms = 2000;
    o.cache.oss_endpoint = "oss-cn-test.aliyuncs.com";
    o.readonly = readonly;
    o.prefix = prefix;
    return o;
  }
  // Table stores talk to the stub instead of OSS.
  PvfsRuntimeOptions opts_with_stub_oss() {
    PvfsRuntimeOptions o = opts();
    o.cache.oss_endpoint = endpoint();
    o.cache.store_options.path_style = true;
    o.cache.store_options.retry_times = 0;
    return o;
  }

  void start_env(const PvfsRuntimeOptions& o, int vcpus = 2) {
    ASSERT_EQ(env_.init(o, vcpus), 0);
  }

  template <class F>
  auto on_next(F&& fn) {
    auto ctx = env_.env()->get_obj_store_env_next();
    return ctx.executor->perform([&]() { return fn(ctx.obj_store); });
  }

  int stat_of(const std::string& path, struct stat* st) {
    memset(st, 0, sizeof(*st));
    return on_next([&](IObjStore* s) { return s->stat(path, st, nullptr); });
  }

  int list_of(const std::string& path, std::vector<std::string>* names) {
    ObjectList list;
    std::string marker;
    int r = on_next(
        [&](IObjStore* s) { return s->list_dir(path, list, &marker); });
    if (r == 0) *names = names_of(list);
    EXPECT_TRUE(marker.empty());
    return r;
  }

  ssize_t put_empty(const std::string& path) {
    return on_next([&](IObjStore* s) -> ssize_t {
      iovec iov{nullptr, 0};
      return s->put_object(path, &iov, 1);
    });
  }

  // Called from the test body, after INIT_PHOTON: the stub needs a vCPU.
  void begin() {
    start();
    install_routes();
  }
  void finish() {
    env_.destroy();
    stop();
  }
};

TEST_F(PvfsObjStoreStubTest, VirtualLevelsListAndStat) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts());
  auto* store = env_.env()->obj_stores[0];
  EXPECT_EQ(store->get_backend_type(), IObjStore::StorageBackend::kOSS);
  EXPECT_EQ(store->get_options().bucket, "pvfs://cat");

  std::vector<std::string> names;
  ASSERT_EQ(list_of("/", &names), 0);
  EXPECT_EQ(names, (std::vector<std::string>{"db", "db2"}));
  ASSERT_EQ(list_of("/db/", &names), 0);
  EXPECT_EQ(names, (std::vector<std::string>{"t", "t2"}));
  ASSERT_EQ(list_of("/db2", &names), 0);
  EXPECT_TRUE(names.empty());
  // Gone databases and tables list empty, as a missing prefix does on OSS.
  ASSERT_EQ(list_of("/nope/", &names), 0);
  EXPECT_TRUE(names.empty());
  ASSERT_EQ(list_of("/db/nope/", &names), 0);
  EXPECT_TRUE(names.empty());

  struct stat st;
  ASSERT_EQ(stat_of("/", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  ASSERT_EQ(stat_of("/db", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(stat_of("/nope", &st), -ENOENT);
  ASSERT_EQ(stat_of("/db/t", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  ASSERT_EQ(stat_of("/db/t/", &st), 0);
  EXPECT_EQ(stat_of("/db/nope", &st), -ENOENT);
  EXPECT_EQ(stat_of("/nope/t", &st), -ENOENT);

  bool empty = true;
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->is_dir_empty("/", empty); }),
            0);
  EXPECT_FALSE(empty);
  EXPECT_EQ(
      on_next([&](IObjStore* s) { return s->is_dir_empty("/db2/", empty); }),
      0);
  EXPECT_TRUE(empty);
  empty = false;
  EXPECT_EQ(
      on_next([&](IObjStore* s) { return s->is_dir_empty("/db/nope/", empty); }),
      0);
  EXPECT_TRUE(empty);

  // Each table name was resolved once (t, nope, nope/t, then nope twice
  // more for the listing and the emptiness probe) and each database once
  // (db, nope); repeats are served from the caches.
  auto& stats = env_.runtime()->rest_client()->stats();
  EXPECT_EQ(stats.get_table.load(), 5u);
  EXPECT_EQ(stats.get_database.load(), 2u);
  ASSERT_EQ(stat_of("/db", &st), 0);
  ASSERT_EQ(stat_of("/db/t", &st), 0);
  ASSERT_EQ(stat_of("/db/t/", &st), 0);
  EXPECT_EQ(stats.get_table.load(), 5u);
  EXPECT_EQ(stats.get_database.load(), 2u);
}

TEST_F(PvfsObjStoreStubTest, ListPagingEndsWithEmptyContext) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts());
  // OssFs keeps calling while the context is non-empty: the second call
  // with a stale marker returns nothing and clears it.
  ObjectList list;
  std::string marker = "leftover";
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir("/", list, &marker); }),
            0);
  EXPECT_TRUE(list.empty());
  EXPECT_TRUE(marker.empty());
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir("/", list, nullptr); }),
            0);
  EXPECT_EQ(list.size(), 2u);
  EXPECT_TRUE(list[0].is_dir());
  EXPECT_EQ(list[0].size(), 0u);
}

TEST_F(PvfsObjStoreStubTest, VirtualLevelMutationsArePerm) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts());
  EXPECT_EQ(put_empty("/newdb/"), -EPERM);
  EXPECT_EQ(put_empty("/db/newtbl"), -EPERM);
  EXPECT_EQ(put_empty("/db/newtbl/"), -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->delete_object("/db/"); }),
            -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->delete_object("/db/t/"); }),
            -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->rename_object("/db/t", "/db/t3");
            }),
            -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->copy_object("/db/t/", "/db/t3/", true);
            }),
            -EPERM);
  std::vector<std::string> desc;
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->list_dir_descendants("/db/t/", desc);
            }),
            -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->delete_objects_under_dir("/db/t/", {"x"});
            }),
            -EPERM);
  void* ctx = nullptr;
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->init_multipart_upload("/db/x", &ctx);
            }),
            -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->truncate_object("/db/t", 0); }),
            -EPERM);
  // Across tables the object never leaves its table.
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->rename_object("/db/t/a", "/db/t2/a");
            }),
            -EXDEV);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->copy_object("/db/t/a", "/db/t2/a");
            }),
            -EXDEV);
  // Unsupported object kinds.
  std::string target;
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->get_symlink("/db/t/l", target); }),
            -ENOTSUP);
  EXPECT_EQ(on_next([&](IObjStore* s) -> ssize_t {
              iovec iov{nullptr, 0};
              return s->append_object("/db/t/a", &iov, 1, 0);
            }),
            -ENOTSUP);
  // No OSS request was needed for any of the refusals.
  EXPECT_EQ(env_.runtime()->rest_client()->stats().get_table_token.load(), 0u);
}

TEST_F(PvfsObjStoreStubTest, ReadOnlyIsErofsBelowTables) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts("", true));
  EXPECT_EQ(put_empty("/db/t/x"), -EROFS);
  EXPECT_EQ(put_empty("/db/t/d/"), -EROFS);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->delete_object("/db/t/x"); }),
            -EROFS);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->rename_object("/db/t/x", "/db/t/y");
            }),
            -EROFS);
  EXPECT_EQ(put_empty("/newdb/"), -EPERM);  // structural refusals come first
  struct stat st;
  EXPECT_EQ(stat_of("/db/t", &st), 0);  // reads are unaffected
}

TEST_F(PvfsObjStoreStubTest, ReservedDirectoriesArePerm) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts());
  auto* rt = env_.runtime();
  EXPECT_EQ(put_empty("/db/t/snapshot/LATEST"), -EPERM);
  EXPECT_EQ(put_empty("/db/t/snapshot/snapshot-1"), -EPERM);
  EXPECT_EQ(rt->reserved_write_refusals(), 1u);  // once per (table, name)
  EXPECT_EQ(put_empty("/db/t/bucket-0/f"), -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->delete_object("/db/t/manifest/m"); }),
            -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->rename_object("/db/t/data/f", "/db/t/schema/f");
            }),
            -EPERM);
  EXPECT_EQ(rt->reserved_write_refusals(), 4u);  // snapshot, bucket-0, manifest, schema
  EXPECT_EQ(rt->rest_client()->stats().get_table_token.load(), 0u);
  EXPECT_EQ(rt->check_mutable("db", "t", "bucketx/f"), 0);
  EXPECT_EQ(rt->check_mutable("db", "t", "tag"), -EPERM);
  PvfsRuntimeOptions allowed = opts();
  allowed.allow_metadata_write = true;
  PvfsFileSystem::PvfsRuntime other(allowed);
  EXPECT_EQ(other.check_mutable("db", "t", "snapshot/LATEST"), 0);
}

TEST_F(PvfsObjStoreStubTest, CheckBucketFailsTheMount) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts());
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), 0);
  databases_status_ = 403;  // OpenAPI disabled or no catalog permission
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), -EACCES);
  databases_status_ = 200;
  down_ = true;
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), -EIO);
}

TEST_F(PvfsObjStoreStubTest, CheckBucketNeedsThePrefix) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts("db/t"));
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), 0);
  env_.destroy();
  start_env(opts("db/nope"));
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), -ENOENT);
  env_.destroy();
  start_env(opts("nope"));
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), -ENOENT);
}

TEST_F(PvfsObjStoreStubTest, PrefixMountsASubTree) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts("db"));
  std::vector<std::string> names;
  ASSERT_EQ(list_of("/", &names), 0);
  EXPECT_EQ(names, (std::vector<std::string>{"t", "t2"}));
  struct stat st;
  ASSERT_EQ(stat_of("/", &st), 0);
  ASSERT_EQ(stat_of("/t", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(stat_of("/nope", &st), -ENOENT);
  EXPECT_EQ(put_empty("/t3/"), -EPERM);
  EXPECT_EQ(put_empty("/t/snapshot/x"), -EPERM);
  EXPECT_EQ(on_next([&](IObjStore* s) {
              return s->rename_object("/t/a", "/t2/a");
            }),
            -EXDEV);
  env_.destroy();
  start_env(opts("db/t"));
  ASSERT_EQ(stat_of("/", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->delete_object("/"); }),
            -EPERM);
  EXPECT_EQ(put_empty("/manifest/x"), -EPERM);
}

TEST_F(PvfsObjStoreStubTest, Oss403FetchesANewTokenOnce) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts_with_stub_oss(), 1);
  auto* cache = env_.runtime()->table_cache();
  auto& stats = env_.runtime()->rest_client()->stats();

  // tok1 is refused by "OSS": one new token, one retry, success.
  EXPECT_EQ(put_empty("/db/t/x"), 0);
  EXPECT_EQ(stats.get_table_token.load(), 2u);
  EXPECT_EQ(cache->forced_refreshes(), 1u);
  EXPECT_EQ(stub_.last_security_token, "tok2");
  uint64_t gen = 0;
  ASSERT_TRUE(cache->credential_generation("db", "t", &gen));
  EXPECT_EQ(gen, 2u);

  // Still refused after a refresh: the error stands and the guard keeps a
  // storm of refused writes from hammering /token.
  oss_status_ = 403;
  EXPECT_EQ(put_empty("/db/t/y"), -EACCES);
  EXPECT_EQ(put_empty("/db/t/z"), -EACCES);
  EXPECT_EQ(stats.get_table_token.load(), 2u);
  EXPECT_EQ(cache->forced_refreshes(), 1u);
  oss_status_ = 200;
  EXPECT_EQ(put_empty("/db/t/y"), 0);
}

TEST_F(PvfsObjStoreStubTest, FailedRelocationRetriesBeforeServingTable) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts_with_stub_oss(), 1);
  auto *cache = env_.runtime()->table_cache();
  int err = 0;
  auto resolve = [&]() {
    return on_next(
        [&](IObjStore *) { return cache->resolve("db", "t", &err); });
  };
  auto entry = resolve();
  ASSERT_NE(entry, nullptr);
  auto original = std::atomic_load(&entry->snapshot);
  ASSERT_NE(original, nullptr);
  EXPECT_EQ(original->prefix, "warehouse/db.db/t");
  entry->location_expire_time = 0;

  auto route = stub_.route;
  int token_status = 503;
  stub_.route = [&](const std::string &path, const std::string &query) {
    if (path == "/v1/pfx/databases/db/tables/t") {
      return json(200, R"({"location":"oss://bucket/relocated/t"})");
    }
    if (path == "/v1/pfx/databases/db/tables/t/token" && token_status != 200) {
      return json(token_status, "{}");
    }
    return route(path, query);
  };

  // A failed token fetch must not mark the new location as ready or let
  // the next request silently use the old location through the fast path.
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_EQ(resolve(), nullptr);
    EXPECT_EQ(err, -EIO);
    EXPECT_EQ(std::atomic_load(&entry->snapshot), original);
    EXPECT_EQ(entry->location, "oss://bucket/warehouse/db.db/t");
    EXPECT_EQ(entry->location_expire_time.load(), 0);
  }

  token_status = 200;
  ASSERT_EQ(resolve(), entry);
  auto relocated = std::atomic_load(&entry->snapshot);
  ASSERT_NE(relocated, nullptr);
  EXPECT_EQ(relocated->prefix, "relocated/t");
  EXPECT_EQ(relocated->creds.securityToken, "tok2");
  EXPECT_EQ(relocated->generation, original->generation + 1);
  EXPECT_GT(entry->location_expire_time.load(), time(nullptr));
}

TEST_F(PvfsObjStoreStubTest, CachedLocationRefreshUsesFreshResponse) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  start_env(opts_with_stub_oss(), 1);
  auto *cache = env_.runtime()->table_cache();
  int err = 0;
  auto resolve = [&]() {
    return on_next(
        [&](IObjStore *) { return cache->resolve("db", "t", &err); });
  };
  auto entry = resolve();
  ASSERT_NE(entry, nullptr);
  auto route = stub_.route;
  std::string response =
      R"({"schema":{"options":{"path":"oss://bucket/moved/t"}}})";
  stub_.route = [&](const std::string &path, const std::string &query) {
    if (path == "/v1/pfx/databases/db/tables/t") return json(200, response);
    return route(path, query);
  };
  entry->location_expire_time = 0;
  ASSERT_EQ(resolve(), entry);
  auto moved = std::atomic_load(&entry->snapshot);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(moved->prefix, "moved/t");

  response = "{}";
  entry->location_expire_time = 0;
  EXPECT_EQ(resolve(), nullptr);
  EXPECT_EQ(err, -EIO);
  EXPECT_EQ(std::atomic_load(&entry->snapshot), moved);
  EXPECT_EQ(entry->location_expire_time.load(), 0);
}

TEST_F(PvfsObjStoreStubTest,
       RandomWriteCompleteFailureDoesNotAbortConsumedContext) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  auto route = stub_.route;
  int parts = 0, completes = 0, aborts = 0;
  stub_.etag = "\"part-etag\"";
  stub_.route = [&](const std::string &path, const std::string &query) {
    if (path.compare(0, 8, "/bucket/") != 0) return route(path, query);
    if (query == "uploads") {
      return json(200,
                  "<InitiateMultipartUploadResult>"
                  "<UploadId>upload-1</UploadId>"
                  "</InitiateMultipartUploadResult>");
    }
    if (query.find("partNumber=") != std::string::npos) {
      ++parts;
      return json(200, "");
    }
    if (query == "uploadId=upload-1") {
      if (stub_.verbs.back() == photon::net::http::Verb::DELETE) {
        ++aborts;
        return json(204, "");
      }
      ++completes;
      return json(503, "");
    }
    return json(404, "");
  };
  auto ro = opts_with_stub_oss();
  ro.prefix = "db/t";
  start_env(ro, 2);
  char staging[] = "/tmp/pvfs-random-complete-XXXXXX";
  ASSERT_NE(mkdtemp(staging), nullptr);
  DEFER(rmdir(staging));
  OssFileSystem::OssFsOptions fo;
  fo.enable_admin_server = false;
  fo.enable_symlink = false;
  fo.temp_dir = staging;
  fo.upload_buffer_size = 1024 * 1024;
  fo.random_write_chunk_size = 1024 * 1024;
  fo.enable_crc64 = false;
  OssFileSystem::BackgroundVCpuEnv bg;
  bg.bg_obj_store_env = env_.env();
  OssFileSystem::OssFs fs(fo, bg);
  ASSERT_EQ(fs.init(), 0);
  uint64_t nodeid = 0;
  struct stat st;
  void *fh = nullptr;
  ASSERT_EQ(fs.creat(1, "multipart", O_CREAT | O_RDWR, 0644, 0, 0, 0, &nodeid,
                     &st, &fh),
            0);
  DEFER(fs.forget(nodeid, 1));
  DEFER(fs.release(nodeid, fh));
  std::string data(2 * 1024 * 1024, 'x');
  ASSERT_EQ(
      static_cast<IFileHandleFuseLL *>(fh)->pwrite(data.data(), data.size(), 0),
      static_cast<ssize_t>(data.size()));
  EXPECT_LT(fs.flush(nodeid, fh), 0);
  EXPECT_EQ(parts, 2);
  EXPECT_EQ(completes, 1);
  EXPECT_EQ(aborts, 0);
}

TEST_F(PvfsObjStoreStubTest, MultipartCompleteCleansUpAfterCatalogFailure) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  auto route = stub_.route;
  bool catalog_down = false;
  int aborts = 0, completes = 0;
  stub_.route = [&](const std::string &path, const std::string &query) {
    if (path.compare(0, 8, "/bucket/") == 0) {
      if (query == "uploads") {
        return json(200,
                    "<InitiateMultipartUploadResult>"
                    "<UploadId>upload-1</UploadId>"
                    "</InitiateMultipartUploadResult>");
      }
      if (query == "uploadId=upload-1") {
        if (stub_.verbs.back() == photon::net::http::Verb::DELETE)
          ++aborts;
        else
          ++completes;
        return json(200, "");
      }
    }
    if (catalog_down) return json(503, "{}");
    return route(path, query);
  };
  start_env(opts_with_stub_oss(), 2);
  void *upload = nullptr;
  ASSERT_EQ(on_next([&](IObjStore *s) {
              return s->init_multipart_upload("/db/t/multipart", &upload);
            }),
            0);
  auto *cache = env_.runtime()->table_cache();
  auto entry = cache->resolve("db", "t");
  ASSERT_NE(entry, nullptr);
  entry->location_expire_time = 0;
  catalog_down = true;
  EXPECT_EQ(on_next([&](IObjStore *s) {
              return s->complete_multipart_upload(upload, nullptr);
            }),
            -EIO);
  EXPECT_EQ(completes, 0);
  EXPECT_EQ(aborts, 1);
}

TEST_F(PvfsObjStoreStubTest,
       MultipartRejectsRelocationAndAbortsOriginalTarget) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  auto route = stub_.route;
  bool moved = false;
  std::vector<std::string> data_requests;
  stub_.etag = "\"part-etag\"";
  stub_.route = [&](const std::string &path, const std::string &query) {
    if (moved && path == "/v1/pfx/databases/db/tables/t") {
      return json(200, R"({"location":"oss://other/moved/t"})");
    }
    if (path.compare(0, 8, "/bucket/") == 0 ||
        path.compare(0, 7, "/other/") == 0) {
      data_requests.push_back(path + "?" + query);
      if (query == "uploads") {
        return json(200,
                    "<InitiateMultipartUploadResult>"
                    "<UploadId>upload-1</UploadId>"
                    "</InitiateMultipartUploadResult>");
      }
      return json(200,
                  "<CopyPartResult><ETag>part-etag</ETag>"
                  "</CopyPartResult>");
    }
    return route(path, query);
  };
  start_env(opts_with_stub_oss(), 2);
  void *upload = nullptr;
  ASSERT_EQ(on_next([&](IObjStore *s) {
              return s->init_multipart_upload("/db/t/multipart", &upload);
            }),
            0);
  auto entry = env_.runtime()->table_cache()->resolve("db", "t");
  ASSERT_NE(entry, nullptr);
  moved = true;
  entry->location_expire_time = 0;
  char data = 'x';
  iovec iov{&data, 1};
  EXPECT_EQ(
      on_next([&](IObjStore *s) { return s->upload_part(upload, &iov, 1, 1); }),
      -ESTALE);
  FILE *file = tmpfile();
  ASSERT_NE(file, nullptr);
  DEFER(fclose(file));
  ASSERT_EQ(fwrite(&data, 1, 1, file), 1u);
  ASSERT_EQ(fflush(file), 0);
  EXPECT_EQ(on_next([&](IObjStore *s) {
              return s->upload_part_from_fd(upload, fileno(file), 0, 1, 2);
            }),
            -ESTALE);
  EXPECT_EQ(on_next([&](IObjStore *s) {
              return s->upload_part_copy(upload, 0, 1, 3);
            }),
            -ESTALE);
  EXPECT_EQ(on_next([&](IObjStore *s) {
              return s->complete_multipart_upload(upload, nullptr);
            }),
            -ESTALE);
  EXPECT_EQ(
      data_requests,
      (std::vector<std::string>{
          "/bucket/warehouse%2Fdb.db%2Ft%2Fmultipart?uploads",
          "/bucket/warehouse%2Fdb.db%2Ft%2Fmultipart?uploadId=upload-1"}));
  EXPECT_EQ(stub_.verbs.back(), photon::net::http::Verb::DELETE);
}

TEST_F(PvfsObjStoreStubTest, EvictionReapsStoresOnTheirVcpu) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  auto o = opts_with_stub_oss();
  o.cache.max_table_cache = 1;
  oss_status_ = 404;
  start_env(o, 1);
  auto* cache = env_.runtime()->table_cache();
  struct stat st;
  EXPECT_EQ(stat_of("/db/t/x", &st), -ENOENT);  // builds t's store
  EXPECT_EQ(cache->stores_reaped(), 0u);
  EXPECT_EQ(stat_of("/db/t2/x", &st), -ENOENT);  // evicts t
  cache->reap_retired_stores();
  EXPECT_EQ(cache->stores_reaped(), 1u);
  EXPECT_EQ(stat_of("/db/t/x", &st), -ENOENT);  // t comes back, t2 goes
  cache->reap_retired_stores();
  EXPECT_EQ(cache->stores_reaped(), 2u);
  EXPECT_EQ(env_.runtime()->rest_client()->stats().get_table.load(), 3u);
}

// Eight requests reach one vCPU before it has a store for the table (the
// way concurrent creates or prefetches do): exactly one store may be
// built, or the others leak their timers and the vCPU never finishes.
TEST_F(PvfsObjStoreStubTest, ConcurrentFirstUseBuildsOneStorePerVcpu) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  oss_status_ = 404;
  auto o = opts_with_stub_oss();
  // set_credentials restarts the signature cache's timer: the builder yields
  // there, and a second caller used to build a store of its own.
  o.cache.store_options.use_auth_cache = true;
  start_env(o, 2);
  auto* cache = env_.runtime()->table_cache();
  for (int round = 0; round < 3; round++) {
    auto ctx = env_.env()->get_obj_store_env(round % 2);
    // Eight tasks queued at once on the vCPU; the test thread keeps
    // serving the stub while it waits for them.
    std::atomic<int> failures{0}, done{0};
    for (int i = 0; i < 8; i++) {
      ctx.executor->async_perform(new auto([&]() {
        struct stat st;
        int r = ctx.obj_store->stat("/db/t/first_use", &st, nullptr);
        if (r != -ENOENT) failures.fetch_add(1);
        done.fetch_add(1);
      }));
    }
    for (int i = 0; i < 1000 && done.load() < 8; i++) {
      photon::thread_usleep(10 * 1000);
    }
    ASSERT_EQ(done.load(), 8) << round;
    EXPECT_EQ(failures.load(), 0) << round;
    // Two vCPUs, one table: at most one store each, however many callers.
    EXPECT_LE(cache->stores_created(), static_cast<uint64_t>(std::min(round + 1, 2)))
        << round;
    EXPECT_EQ(cache->live_stores(), cache->stores_created()) << round;
  }
  env_.destroy();  // must not wait for a leaked store's timers
}

// Fifty mounts come and go in one process, each with stores on three
// vCPUs and a background token refresh that is often in flight when the
// runtime is torn down; teardown must always complete, and quickly.
TEST_F(PvfsObjStoreStubTest, RuntimeTeardownFiftyTimes) {
  INIT_PHOTON();
  begin();
  DEFER(finish());
  token_hold_ms_ = 100;  // a refresh takes a while: teardown overlaps it
  oss_status_ = 404;     // the stub plays an empty bucket
  auto o = opts_with_stub_oss();
  o.cache.store_options.use_auth_cache = true;
  o.cache.credential_refresh_ahead_sec = 10 * 365 * 24 * 3600;  // always due
  o.cache.cred_refresh_interval_sec = 1;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 50; i++) {
    ASSERT_EQ(env_.init(o, 3), 0) << i;
    struct stat st;
    for (int v = 0; v < 3; v++) {  // one store on each vCPU
      EXPECT_EQ(stat_of("/db/t/x", &st), -ENOENT) << i;
    }
    if (i % 5 == 0) {
      // A forced token fetch racing the background one.
      uint64_t gen = 0;
      ASSERT_TRUE(env_.runtime()->table_cache()->credential_generation(
          "db", "t", &gen));
      env_.runtime()->table_cache()->force_refresh("db", "t", gen);
    }
    photon::thread_usleep((i % 7) * 50 * 1000);  // 0..300 ms into the loop
    env_.destroy();
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
  EXPECT_LT(ms, 60000) << "50 mounts took " << ms << " ms";
}

// Against the real catalog: the pinned table through the store, on the
// vCPUs of a small env.
class PvfsObjStoreLiveTest : public ::testing::Test {
 protected:
  PvfsTestEnv env_;
  std::string dir_;  // table-relative scratch directory, with slashes

  void start_env(int vcpus = 3) {
    if (FLAGS_pvfs_catalog.empty() || FLAGS_pvfs_test_database.empty() ||
        FLAGS_pvfs_test_table.empty()) {
      GTEST_SKIP() << "live catalog flags not set";
    }
    PvfsRuntimeOptions opts;
    ASSERT_EQ(pvfs_test_runtime_options(&opts, ""), 0);
    ASSERT_EQ(env_.init(opts, vcpus), 0);
    dir_ = "/" + FLAGS_pvfs_test_database + "/" + FLAGS_pvfs_test_table +
           "/objstore_test_" + std::to_string(getpid()) + "_" +
           std::to_string(time(nullptr)) + "/";
  }

  template <class F>
  auto on_next(F&& fn) {
    auto ctx = env_.env()->get_obj_store_env_next();
    return ctx.executor->perform([&]() { return fn(ctx.obj_store); });
  }

  ssize_t put(const std::string& path, const std::string& data) {
    return on_next([&](IObjStore* s) -> ssize_t {
      iovec iov{const_cast<char*>(data.data()), data.size()};
      return s->put_object(path, &iov, 1);
    });
  }

  std::string get(const std::string& path, size_t size) {
    std::string buf(size, '\0');
    ssize_t r = on_next([&](IObjStore* s) -> ssize_t {
      iovec iov{&buf[0], size};
      return s->get_object_range(path, &iov, 1, 0);
    });
    if (r != static_cast<ssize_t>(size)) return "<" + std::to_string(r) + ">";
    return buf;
  }

  void cleanup() {
    if (dir_.empty()) return;
    on_next([&](IObjStore* s) {
      std::vector<std::string> names;
      if (s->list_dir_descendants(dir_, names) == 0 && !names.empty()) {
        std::vector<std::string_view> batch(names.begin(), names.end());
        s->delete_objects_under_dir(dir_, batch);
      }
      s->delete_object(dir_);
      return 0;
    });
  }
};

TEST_F(PvfsObjStoreLiveTest, VirtualLevelsOnTheCatalog) {
  INIT_PHOTON();
  start_env();
  DEFER(env_.destroy());
  ObjectList list;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir("/", list); }), 0);
  auto dbs = names_of(list);
  EXPECT_TRUE(std::binary_search(dbs.begin(), dbs.end(),
                                 FLAGS_pvfs_test_database));
  list.clear();
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->list_dir("/" + FLAGS_pvfs_test_database + "/", list);
            }),
            0);
  auto tables = names_of(list);
  EXPECT_TRUE(std::binary_search(tables.begin(), tables.end(),
                                 FLAGS_pvfs_test_table));
  struct stat st;
  memset(&st, 0, sizeof(st));
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->stat("/" + FLAGS_pvfs_test_database + "/" +
                                 FLAGS_pvfs_test_table,
                             &st, nullptr);
            }),
            0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->check_bucket(); }), 0);
}

TEST_F(PvfsObjStoreLiveTest, DelegationThroughTheTable) {
  INIT_PHOTON();
  start_env();
  DEFER(env_.destroy());
  DEFER(cleanup());

  const std::string a = dir_ + "a.txt", b = dir_ + "b.txt", c = dir_ + "c.txt";
  ASSERT_EQ(put(a, "hello"), 5);
  struct stat st;
  memset(&st, 0, sizeof(st));
  std::string etag;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->stat(a, &st, &etag); }), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 5);
  EXPECT_FALSE(etag.empty());
  OssFileSystem::ObjHeaderMeta meta;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->head_object(a, meta); }), 0);
  EXPECT_EQ(meta.size, 5u);
  EXPECT_EQ(get(a, 5), "hello");

  // The scratch directory exists by virtue of its child; listing and
  // emptiness see the file.
  memset(&st, 0, sizeof(st));
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->stat(dir_, &st, nullptr); }),
            0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  ObjectList list;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir(dir_, list); }), 0);
  EXPECT_EQ(names_of(list), (std::vector<std::string>{"a.txt"}));
  bool empty = true;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->is_dir_empty(dir_, empty); }),
            0);
  EXPECT_FALSE(empty);

  ASSERT_EQ(on_next([&](IObjStore* s) { return s->rename_object(a, b); }), 0);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->stat(a, &st, nullptr); }),
            -ENOENT);
  EXPECT_EQ(get(b, 5), "hello");
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->copy_object(b, c, true); }),
            0);
  EXPECT_EQ(get(c, 5), "hello");
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->truncate_object(c, 0); }), 0);
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->stat(c, &st, nullptr); }), 0);
  EXPECT_EQ(st.st_size, 0);

  // Multipart: the context travels across vCPUs like it does in OssFs.
  const std::string mp = dir_ + "mp.bin";
  std::string part1(200 * 1024, 'x'), part2(10, 'y');
  void* ctx = nullptr;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->init_multipart_upload(mp, &ctx); }),
            0);
  ASSERT_NE(ctx, nullptr);
  for (int i = 0; i < 2; i++) {
    std::string& part = i == 0 ? part1 : part2;
    ASSERT_EQ(on_next([&](IObjStore* s) -> ssize_t {
                iovec iov{&part[0], part.size()};
                return s->upload_part(ctx, &iov, 1, i + 1);
              }),
              static_cast<ssize_t>(part.size()));
  }
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->complete_multipart_upload(ctx, nullptr);
            }),
            0);
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->stat(mp, &st, nullptr); }), 0);
  EXPECT_EQ(st.st_size, static_cast<off_t>(part1.size() + part2.size()));
  EXPECT_EQ(get(mp, 12).substr(0, 12), "xxxxxxxxxxxx");
  ctx = nullptr;
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->init_multipart_upload(dir_ + "aborted.bin", &ctx);
            }),
            0);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->abort_multipart_upload(ctx); }),
            0);

  std::vector<std::string> names;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir_descendants(dir_, names); }),
            0);
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"b.txt", "c.txt", "mp.bin"}));
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->delete_objects_under_dir(dir_, {"b.txt", "c.txt"});
            }),
            0);
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->delete_object(mp); }), 0);
  list.clear();
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir(dir_, list); }), 0);
  EXPECT_TRUE(list.empty());

  // Every vCPU built its own store for the table; the REST catalog was
  // asked for the table and its token once.
  auto& stats = env_.runtime()->rest_client()->stats();
  EXPECT_EQ(stats.get_table.load(), 1u);
  EXPECT_EQ(stats.get_table_token.load(), 1u);
}

// A RAM user granted SELECT only gets a read-only token: a write is
// refused by OSS, the store fetches the token once more and gives up with
// EACCES; further writes are EACCES without another token fetch, and
// reads keep working.
TEST_F(PvfsObjStoreLiveTest, ReadOnlyTokenWritesAreEaccesWithoutStorm) {
  INIT_PHOTON();
  const char* ak = getenv("PVFS_TEST_LIMITED_AK");
  const char* sk = getenv("PVFS_TEST_LIMITED_SK");
  if (!ak || !*ak || !sk || !*sk) {
    GTEST_SKIP() << "PVFS_TEST_LIMITED_AK/PVFS_TEST_LIMITED_SK not set";
  }
  if (FLAGS_pvfs_catalog.empty() || FLAGS_pvfs_test_table.empty()) {
    GTEST_SKIP() << "live catalog flags not set";
  }
  PvfsRuntimeOptions opts;
  ASSERT_EQ(pvfs_test_runtime_options(&opts, ""), 0);
  opts.rest.access_key_id = ak;
  opts.rest.access_key_secret = sk;
  opts.rest.security_token.clear();
  ASSERT_EQ(env_.init(opts, 2), 0);
  DEFER(env_.destroy());
  auto* cache = env_.runtime()->table_cache();
  auto& stats = env_.runtime()->rest_client()->stats();
  const std::string tbl = "/" + FLAGS_pvfs_test_database + "/" +
                          FLAGS_pvfs_test_table + "/";
  const std::string probe = tbl + "objstore_perm_" + std::to_string(getpid());

  ObjectList list;
  ASSERT_EQ(on_next([&](IObjStore* s) { return s->list_dir(tbl, list); }), 0);
  EXPECT_FALSE(list.empty());
  EXPECT_EQ(stats.get_table_token.load(), 1u);

  auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(put(probe, "denied"), -EACCES);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count(),
            10000);
  EXPECT_EQ(stats.get_table_token.load(), 2u);  // the one forced refresh
  EXPECT_EQ(cache->forced_refreshes(), 1u);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(put(probe, "denied"), -EACCES) << i;
    EXPECT_EQ(on_next([&](IObjStore* s) { return s->delete_object(probe); }),
              -EACCES)
        << i;
  }
  EXPECT_EQ(stats.get_table_token.load(), 2u);
  EXPECT_EQ(cache->forced_refreshes(), 1u);
  struct stat st;
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->stat(probe, &st, nullptr); }),
            -ENOENT);
  // Reads on the same token stay fine.
  std::vector<std::string> names;
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->list_dir_descendants(tbl + "schema/", names);
            }),
            0);
  ASSERT_FALSE(names.empty());
  memset(&st, 0, sizeof(st));
  ASSERT_EQ(on_next([&](IObjStore* s) {
              return s->stat(tbl + "schema/" + names[0], &st, nullptr);
            }),
            0);
  EXPECT_GT(st.st_size, 0);
  EXPECT_EQ(get(tbl + "schema/" + names[0], 1).size(), 1u);
  EXPECT_EQ(stats.get_table_token.load(), 2u);
}

TEST_F(PvfsObjStoreLiveTest, CrossTableRenameIsExdev) {
  INIT_PHOTON();
  start_env();
  DEFER(env_.destroy());
  if (FLAGS_pvfs_test_table2.empty()) {
    GTEST_SKIP() << "--pvfs_test_table2 not set";
  }
  DEFER(cleanup());
  const std::string a = dir_ + "a.txt";
  ASSERT_EQ(put(a, "hello"), 5);
  std::string other = "/" + FLAGS_pvfs_test_database + "/" +
                      FLAGS_pvfs_test_table2 + "/objstore_test_" +
                      std::to_string(getpid()) + ".txt";
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->rename_object(a, other); }),
            -EXDEV);
  EXPECT_EQ(on_next([&](IObjStore* s) { return s->copy_object(a, other); }),
            -EXDEV);
  EXPECT_EQ(get(a, 5), "hello");
}

}  // namespace
