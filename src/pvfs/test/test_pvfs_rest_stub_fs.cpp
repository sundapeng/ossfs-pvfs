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
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "common/fuse.h"
#include "common/logger.h"
#include "fs/bg_vcpu_env.h"
#include "pvfs/paimon_rest_client.h"
#include "pvfs_rest_stub.h"
#include "pvfs_test_suite.h"

using PvfsFileSystem::PaimonRESTClient;
using PvfsFileSystem::PaimonRESTClientOptions;
using PvfsFileSystem::PvfsRuntimeOptions;
using PvfsFileSystem::TableCredential;
using PvfsFileSystem::TableInfo;

namespace {

// A whole mount (OssFs over PvfsObjStore) against the REST stub: every
// way the catalog can fail must surface as a bounded, meaningful errno
// and never wedge the mount.
class PvfsRestStubFsTest : public RestStubFixture {
 protected:
  static constexpr int64_t kBoundMs = 5000;
  using Instance = PvfsTestSuite::Instance;

  // Knobs read by the routes below.
  int token_status_ = 200;
  std::string token_override_;  // "<empty>" serves an empty 200 body
  int token_hold_ms_ = 0;
  std::string table_override_;  // "<empty>" serves an empty 200 body
  std::string creds_ =
      R"("fs.oss.accessKeyId":"ak","fs.oss.accessKeySecret":"sk",)"
      R"("fs.oss.securityToken":"tok","fs.oss.endpoint":"oss-cn-test.aliyuncs.com")";
  int64_t expires_ms_ = 0;  // 0: one hour from now
  bool down_ = false;       // 503 for everything
  std::set<std::string> tables_ = {"t", "t2"};
  std::string live_location_;  // real table location, see serve_live_table
  std::string live_suffix_;    // appended to it to simulate a move
  // External tables the stub describes Bennett-style: name -> {type, path}.
  std::map<std::string, std::pair<std::string, std::string>> externals_;
  bool user_credential_ = false;  // fs_opts sets a static OSS credential
  std::string user_ak_ = "user-ak", user_sk_ = "user-sk";
  std::string external_endpoint_ = "oss-cn-test.aliyuncs.com";

  void install_routes() {
    stub_.route = [this](const std::string& t,
                         const std::string&) -> std::pair<int, std::string> {
      if (down_) return {503, "{}"};
      if (t == "/v1/config") return {200, R"({"defaults":{"prefix":"pfx"}})"};
      const std::string dbs = "/v1/pfx/databases";
      if (t == dbs) return {200, R"({"databases":["db"]})"};
      if (t == dbs + "/db") return {200, "{}"};
      if (t == dbs + "/db/tables") return {200, R"({"tables":["t","t2"]})"};
      const std::string tp = dbs + "/db/tables/";
      if (t.compare(0, tp.size(), tp) != 0) return {404, "{}"};
      std::string name = t.substr(tp.size());
      bool token = false;
      if (name.size() > 6 && name.compare(name.size() - 6, 6, "/token") == 0) {
        token = true;
        name.resize(name.size() - 6);
      }
      if (!tables_.count(name) && !externals_.count(name)) return {404, "{}"};
      if (token) {
        if (token_hold_ms_ > 0) stub_.hold_ms(token_hold_ms_);
        if (token_status_ != 200) return {token_status_, "{}"};
        if (!token_override_.empty())
          return {200, token_override_ == "<empty>" ? "" : token_override_};
        int64_t exp = expires_ms_ ? expires_ms_ : (time(nullptr) + 3600) * 1000;
        return {200, "{\"token\":{" + creds_ +
                         "},\"expiresAtMillis\":" + std::to_string(exp) + "}"};
      }
      if (!table_override_.empty())
        return {200, table_override_ == "<empty>" ? "" : table_override_};
      auto ext = externals_.find(name);
      if (ext != externals_.end()) {
        const auto& [type, path] = ext->second;
        std::string options = "\"path\":\"" + path + "\",\"bucket\":\"-1\"";
        if (!type.empty()) options = "\"type\":\"" + type + "\"," + options;
        return {200, "{\"id\":\"tbl-1\",\"database\":\"db\",\"name\":\"" + name +
                         "\",\"path\":\"" + path +
                         "\",\"isExternal\":true,\"schemaId\":0,\"schema\":{"
                         "\"fields\":[],\"partitionKeys\":[],\"primaryKeys\":[],"
                         "\"options\":{" + options +
                         "},\"comment\":\"\"},\"owner\":\"1\",\"createdAt\":1}"};
      }
      std::string location = (!live_location_.empty() && name == "t")
                                 ? live_location_ + live_suffix_
                                 : "oss://bucket/warehouse/db.db/" + name;
      return {200, "{\"location\":\"" + location + "\"}"};
    };
  }

  PvfsRuntimeOptions fs_opts() {
    PvfsRuntimeOptions o;
    o.rest.endpoint = endpoint();
    o.rest.region = "cn-test";
    o.rest.catalog = "cat";
    o.rest.access_key_id = "ak";
    o.rest.access_key_secret = "sk";
    o.rest.signing_algorithm = "default";
    o.rest.timeout_ms = 2000;
    o.cache.oss_endpoint = FLAGS_pvfs_oss_endpoint;
    o.readonly = false;
    if (user_credential_) {
      o.cache.static_access_key_id = user_ak_;
      o.cache.static_access_key_secret = user_sk_;
      o.cache.external_oss_endpoint = external_endpoint_;
    }
    return o;
  }
  static OssFileSystem::OssFsOptions fs_options() {
    OssFileSystem::OssFsOptions fo;
    fo.enable_admin_server = false;
    fo.enable_appendable_object = false;
    fo.enable_symlink = false;
    return fo;
  }
  // A mount on the stub; nullptr with *err set when it cannot come up.
  std::unique_ptr<Instance> make(const PvfsRuntimeOptions& o,
                                 int* err = nullptr) {
    auto inst = std::make_unique<Instance>();
    int r = inst->init(o, fs_options(), 1);
    if (err) *err = r;
    if (r != 0) return nullptr;
    return inst;
  }

  // Simulation bucket for external tables, from the runner's environment.
  struct SimBucket {
    std::string ak, sk, bucket, endpoint;
    bool has_key() const { return !ak.empty() && !sk.empty(); }
    bool has_bucket() const { return !bucket.empty() && !endpoint.empty(); }
  };
  static SimBucket sim_bucket() {
    auto get = [](const char* n) {
      const char* v = getenv(n);
      return std::string(v ? v : "");
    };
    return {get("PVFS_TEST_EXTERNAL_AK"), get("PVFS_TEST_EXTERNAL_SK"),
            get("PVFS_TEST_EXTERNAL_BUCKET"),
            get("PVFS_TEST_EXTERNAL_ENDPOINT")};
  }

  // Names under `dir` through opendir/readdir, or a marker on failure.
  static std::vector<std::string> names_in(IFileSystemFuseLL& fs, uint64_t dir,
                                           int* err = nullptr) {
    struct fuse_file_info fi;
    memset(&fi, 0, sizeof(fi));
    int r = fs.opendir(dir, &fi);
    if (r != 0) {
      if (err) *err = r;
      return {"<opendir failed>"};
    }
    void* dh = reinterpret_cast<void*>(fi.fh);
    std::vector<PvfsTestSuite::DirEntry> entries;
    r = fs.readdir(dir, 0, dh, PvfsTestSuite::filler, &entries, nullptr, false,
                   nullptr);
    fs.releasedir(dir, dh);
    if (err) *err = r;
    if (r != 0) return {"<readdir failed>"};
    std::vector<std::string> names;
    for (auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    return names;
  }

  // Serve the live table's real location and token for "t" so OSS calls
  // through the stub-backed instance reach the real table. False without
  // the live flags.
  bool serve_live_table() {
    if (FLAGS_pvfs_test_table.empty() || FLAGS_pvfs_catalog.empty()) {
      return false;
    }
    PaimonRESTClientOptions ro;
    ro.endpoint = FLAGS_pvfs_endpoint;
    ro.region = FLAGS_pvfs_region;
    ro.catalog = FLAGS_pvfs_catalog;
    ro.access_key_id = FLAGS_pvfs_access_key_id;
    ro.access_key_secret = FLAGS_pvfs_access_key_secret;
    ro.security_token = FLAGS_pvfs_security_token;
    ro.signing_algorithm = FLAGS_pvfs_signing_algorithm;
    PaimonRESTClient client(ro);
    if (client.init() != 0) return false;
    TableInfo info;
    if (client.get_table(FLAGS_pvfs_test_database, FLAGS_pvfs_test_table,
                         info) != 0) {
      return false;
    }
    TableCredential cred;
    if (client.get_table_token(FLAGS_pvfs_test_database, FLAGS_pvfs_test_table,
                               cred) != 0) {
      return false;
    }
    live_location_ = info.location;
    creds_ = "\"fs.oss.accessKeyId\":\"" + cred.access_key_id +
             "\",\"fs.oss.accessKeySecret\":\"" + cred.access_key_secret +
             "\",\"fs.oss.securityToken\":\"" + cred.security_token +
             "\",\"fs.oss.endpoint\":\"" + cred.oss_endpoint + "\"";
    expires_ms_ = cred.expiration_sec * 1000;
    return true;
  }

  struct Counters {
    uint64_t list_databases, list_tables, get_database, get_table, token;
    bool operator==(const Counters& o) const {
      return list_databases == o.list_databases &&
             list_tables == o.list_tables && get_database == o.get_database &&
             get_table == o.get_table && token == o.token;
    }
  };
  static Counters counters(Instance& inst) {
    const auto& s = inst.runtime()->rest_client()->stats();
    return {s.list_databases.load(), s.list_tables.load(),
            s.get_database.load(), s.get_table.load(),
            s.get_table_token.load()};
  }
  static uint64_t tokens(Instance& inst) {
    return inst.runtime()->rest_client()->stats().get_table_token.load();
  }

  static int lookup(IFileSystemFuseLL& fs, uint64_t parent,
                    const std::string& name, uint64_t& nodeid) {
    struct stat st;
    return fs.lookup(parent, name, &nodeid, &st);
  }
  static int write_file(IFileSystemFuseLL& fs, uint64_t dir,
                        const std::string& name, const std::string& data,
                        uint64_t& nodeid, void** keep_fh = nullptr) {
    struct stat st;
    void* fh = nullptr;
    int r = fs.creat(dir, name, O_CREAT | O_WRONLY, 0644, 0, 0, 0, &nodeid,
                     &st, &fh);
    if (r != 0) return r;
    auto* h = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ssize_t w = h->pwrite(data.data(), data.size(), 0);
    if (w != static_cast<ssize_t>(data.size())) return w < 0 ? w : -EIO;
    if (keep_fh) {
      *keep_fh = fh;
      return 0;
    }
    return fs.release(nodeid, fh);
  }
  static std::string read_file(IFileSystemFuseLL& fs, uint64_t nodeid,
                               size_t size) {
    void* fh = nullptr;
    bool keep = false;
    if (fs.open(nodeid, O_RDONLY, &fh, &keep) != 0) return "<open failed>";
    std::string out(size, '\0');
    auto* h = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ssize_t rd = h->pread(out.data(), size, 0);
    fs.release(nodeid, fh);
    if (rd < 0) return "<read failed>";
    out.resize(rd);
    return out;
  }
  std::string scratch_name(const char* tag) {
    return std::string("pvfs_test_stub_") + tag + "_" + std::to_string(getpid());
  }

  // Every token failure is one REST call, maps to the status and never
  // poisons the entry: a later 200 recovers.
  void verify_token_errors_map_and_recover() {
    INIT_PHOTON();
    start();
    DEFER(stop());
    install_routes();
    auto inst = make(fs_opts());
    ASSERT_NE(inst, nullptr);
    auto* cache = inst->cache();
    struct Case {
      int status;
      const char* body;
      int err;
    };
    const Case cases[] = {{401, "", -EACCES}, {403, "", -EACCES},
                          {429, "", -EAGAIN}, {500, "", -EIO},
                          {200, "<empty>", -EIO}, {200, R"({"token":{}})", -EIO}};
    for (const auto& c : cases) {
      token_status_ = c.status;
      token_override_ = c.body;
      uint64_t before = tokens(*inst);
      int err = 0;
      auto t0 = std::chrono::steady_clock::now();
      EXPECT_EQ(cache->resolve("db", "t", &err), nullptr) << c.status;
      EXPECT_LT(ms_since(t0), kBoundMs);
      EXPECT_EQ(err, c.err) << c.status << " " << c.body;
      EXPECT_EQ(tokens(*inst) - before, 1u);
    }
    token_status_ = 200;
    token_override_.clear();
    EXPECT_NE(cache->resolve("db", "t"), nullptr);
  }

  // A token endpoint that never answers costs its caller the REST timeout
  // and nobody else anything: another cached table is served meanwhile.
  void verify_slow_token_times_out_without_blocking_others() {
    INIT_PHOTON();
    start();
    DEFER(stop());
    install_routes();
    auto inst = make(fs_opts());
    ASSERT_NE(inst, nullptr);
    auto* cache = inst->cache();
    ASSERT_NE(cache->resolve("db", "t2"), nullptr);

    token_hold_ms_ = 45 * 1000;
    std::atomic<int64_t> other_ms{-1};
    std::atomic<bool> other_ok{false};
    std::thread other([&]() {
      INIT_PHOTON();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      auto t0 = std::chrono::steady_clock::now();
      other_ok = cache->resolve("db", "t2") != nullptr;
      other_ms = ms_since(t0);
    });
    int err = 0;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(cache->resolve("db", "t", &err), nullptr);
    int64_t slow_ms = ms_since(t0);
    EXPECT_EQ(err, -ETIMEDOUT);
    EXPECT_LT(slow_ms, kBoundMs);
    EXPECT_GE(slow_ms, 1500);  // the 2 s REST timeout, not a fast failure
    other.join();
    EXPECT_TRUE(other_ok.load());
    EXPECT_LT(other_ms.load(), 1000);
    token_hold_ms_ = 0;
  }

  // A closed port or an unresolvable host fails the mount quickly.
  void verify_init_fails_fast_on_bad_endpoints() {
    INIT_PHOTON();
    {
      PvfsRuntimeOptions o = fs_opts();
      o.rest.endpoint = "http://127.0.0.1:1";
      auto t0 = std::chrono::steady_clock::now();
      EXPECT_EQ(make(o), nullptr);
      EXPECT_LT(ms_since(t0), 2000);
    }
    {
      PvfsRuntimeOptions o = fs_opts();
      o.rest.endpoint = "http://no-such-host.invalid";
      o.rest.timeout_ms = 3000;
      auto t0 = std::chrono::steady_clock::now();
      EXPECT_EQ(make(o), nullptr);
      EXPECT_LT(ms_since(t0), kBoundMs);
    }
  }

  // Garbage from the catalog is EIO, not a crash and not a hang.
  void verify_malformed_bodies_are_eio() {
    INIT_PHOTON();
    start();
    DEFER(stop());
    install_routes();
    auto inst = make(fs_opts());
    ASSERT_NE(inst, nullptr);
    auto* cache = inst->cache();
    int n = 0;
    for (const char* body : {"not json at all", "{}", "<empty>",
                             R"({"location":""})"}) {
      table_override_ = body;
      std::string name = "t" + std::to_string(10 + n++);
      tables_.insert(name);
      int err = 0;
      auto t0 = std::chrono::steady_clock::now();
      EXPECT_EQ(cache->resolve("db", name, &err), nullptr) << body;
      EXPECT_LT(ms_since(t0), kBoundMs);
      EXPECT_EQ(err, -EIO) << body;
    }
    table_override_.clear();
    for (const char* body :
         {"garbage", R"({"token":{"fs.oss.endpoint":"x"},"expiresAtMillis":1})",
          R"({"token":{"fs.oss.accessKeyId":"ak"}})"}) {
      token_override_ = body;
      std::string name = "t" + std::to_string(10 + n++);
      tables_.insert(name);
      int err = 0;
      auto t0 = std::chrono::steady_clock::now();
      EXPECT_EQ(cache->resolve("db", name, &err), nullptr) << body;
      EXPECT_LT(ms_since(t0), kBoundMs);
      EXPECT_EQ(err, -EIO) << body;
    }
    token_override_.clear();
    EXPECT_NE(cache->resolve("db", "t"), nullptr);
  }

  // A table that vanishes from the catalog is noticed when its token
  // expires and cannot be renewed: ENOENT, promptly.
  void verify_not_found_after_token_expiry() {
    INIT_PHOTON();
    start();
    DEFER(stop());
    install_routes();
    PvfsRuntimeOptions o = fs_opts();
    o.cache.credential_refresh_ahead_sec = 0;
    auto inst = make(o);
    ASSERT_NE(inst, nullptr);
    time_t expire = time(nullptr) + 2;
    expires_ms_ = static_cast<int64_t>(expire) * 1000;
    ASSERT_NE(inst->cache()->resolve("db", "t"), nullptr);
    token_status_ = 404;
    while (time(nullptr) < expire) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int err = 0;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(inst->cache()->resolve("db", "t", &err), nullptr);
    EXPECT_LT(ms_since(t0), kBoundMs);
    EXPECT_EQ(err, -ENOENT);
  }

  // Once a table is cached the catalog may go away: cached names resolve,
  // data flows, no REST call is made; only new names fail, and quickly.
  void verify_catalog_down_after_caching() {
    INIT_PHOTON();
    if (!serve_live_table()) GTEST_SKIP() << "live catalog flags not set";
    start();
    DEFER(stop());
    install_routes();
    auto inst = make(fs_opts());
    ASSERT_NE(inst, nullptr);
    auto& fs = *inst->fs;
    uint64_t db = 0, tbl = 0, dir = 0, file = 0;
    ASSERT_EQ(lookup(fs, 1, "db", db), 0);
    ASSERT_EQ(lookup(fs, db, "t", tbl), 0);
    struct stat st;
    std::string sub = scratch_name("down");
    ASSERT_EQ(fs.mkdir(tbl, sub, 0755, 0, 0, 0, &dir, &st), 0);
    ASSERT_EQ(write_file(fs, dir, "f", "cached-bytes", file), 0);
    DEFER({
      fs.unlink(dir, "f");
      fs.rmdir(tbl, sub);
      fs.forget(file, 1);
      fs.forget(dir, 1);
      fs.forget(tbl, 1);
      fs.forget(db, 1);
    });
    ASSERT_EQ(read_file(fs, file, 12), "cached-bytes");

    down_ = true;
    Counters before = counters(*inst);
    uint64_t again = 0;
    EXPECT_EQ(lookup(fs, 1, "db", again), 0);
    EXPECT_EQ(again, db);
    fs.forget(again, 1);
    EXPECT_EQ(lookup(fs, db, "t", again), 0);
    EXPECT_EQ(again, tbl);
    fs.forget(again, 1);
    EXPECT_NE(inst->cache()->resolve("db", "t"), nullptr);
    EXPECT_EQ(read_file(fs, file, 12), "cached-bytes");
    uint64_t f2 = 0;
    EXPECT_EQ(write_file(fs, dir, "g", "while-down", f2), 0);
    EXPECT_EQ(read_file(fs, f2, 10), "while-down");
    fs.unlink(dir, "g");
    fs.forget(f2, 1);
    EXPECT_TRUE(counters(*inst) == before);
    uint64_t other = 0;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(lookup(fs, 1, "other_db", other), -EIO);
    EXPECT_LT(ms_since(t0), kBoundMs);
    down_ = false;
  }

  // A table whose location moves gets a new generation and stores on the
  // new location; a write pending at the move lands where the table is
  // now.
  void verify_location_change_rebuilds_env() {
    INIT_PHOTON();
    if (!serve_live_table()) GTEST_SKIP() << "live catalog flags not set";
    start();
    DEFER(stop());
    install_routes();
    PvfsRuntimeOptions o = fs_opts();
    o.cache.location_cache_ttl_sec = 0;
    auto inst = make(o);
    ASSERT_NE(inst, nullptr);
    auto& fs = *inst->fs;
    auto* cache = inst->cache();
    uint64_t db = 0, tbl = 0, dir = 0, file = 0;
    ASSERT_EQ(lookup(fs, 1, "db", db), 0);
    ASSERT_EQ(lookup(fs, db, "t", tbl), 0);
    struct stat st;
    std::string sub = scratch_name("move");
    ASSERT_EQ(fs.mkdir(tbl, sub, 0755, 0, 0, 0, &dir, &st), 0);
    void* fh = nullptr;
    ASSERT_EQ(write_file(fs, dir, "f", "pending", file, &fh), 0);
    ASSERT_NE(cache->resolve("db", "t"), nullptr);
    uint64_t gen1 = 0;
    ASSERT_TRUE(cache->credential_generation("db", "t", &gen1));
    PvfsFileSystem::TableEntryInfo before, after;
    ASSERT_TRUE(cache->entry_info("db", "t", &before));

    live_suffix_ = "/" + sub;  // the catalog now says the table moved
    ASSERT_NE(cache->resolve("db", "t"), nullptr);
    uint64_t gen2 = 0;
    ASSERT_TRUE(cache->credential_generation("db", "t", &gen2));
    EXPECT_EQ(gen2, gen1 + 1);
    ASSERT_TRUE(cache->entry_info("db", "t", &after));
    EXPECT_EQ(after.prefix, before.prefix + "/" + sub);
    // The pending write completes on the new location.
    EXPECT_EQ(fs.flush(file, fh), 0);
    EXPECT_EQ(fs.release(file, fh), 0);
    EXPECT_EQ(read_file(fs, file, 7), "pending");
    fs.unlink(dir, "f");
    fs.rmdir(tbl, sub);

    live_suffix_.clear();
    ASSERT_NE(cache->resolve("db", "t"), nullptr);
    uint64_t gen3 = 0;
    ASSERT_TRUE(cache->credential_generation("db", "t", &gen3));
    EXPECT_EQ(gen3, gen2 + 1);
    ASSERT_TRUE(cache->entry_info("db", "t", &after));
    EXPECT_EQ(after.prefix, before.prefix);
    fs.rmdir(tbl, sub);  // the marker made before the move
    fs.forget(file, 1);
    fs.forget(dir, 1);
    fs.forget(tbl, 1);
    fs.forget(db, 1);
  }

  // isExternal picks the credential: the user's own when configured (no
  // token call, stores on the table's bucket), else the catalog token with
  // one warning per table; managed tables always take the token.
  void verify_external_table_credential_modes() {
    INIT_PHOTON();
    start();
    DEFER(stop());
    install_routes();
    externals_["objtbl"] = {"object-table", "oss://other-bucket/prefix/objtbl"};
    externals_["notype"] = {"", "oss://other-bucket/prefix/notype"};
    user_credential_ = true;
    {
      auto inst = make(fs_opts());
      ASSERT_NE(inst, nullptr);
      auto* cache = inst->cache();
      ASSERT_NE(cache->resolve("db", "objtbl"), nullptr);
      EXPECT_EQ(tokens(*inst), 0u);
      EXPECT_EQ(cache->external_token_warnings(), 0u);
      PvfsFileSystem::TableEntryInfo info;
      ASSERT_TRUE(cache->entry_info("db", "objtbl", &info));
      EXPECT_TRUE(info.is_external);
      EXPECT_EQ(info.table_type, "object-table");
      EXPECT_EQ(info.cred_mode, PvfsFileSystem::CredMode::kStatic);
      EXPECT_EQ(info.bucket, "other-bucket");
      EXPECT_EQ(info.prefix, "prefix/objtbl");
      EXPECT_EQ(info.endpoint, "https://" + external_endpoint_);

      ASSERT_NE(cache->resolve("db", "notype"), nullptr);
      ASSERT_TRUE(cache->entry_info("db", "notype", &info));
      EXPECT_EQ(info.table_type, "");
      EXPECT_EQ(info.cred_mode, PvfsFileSystem::CredMode::kStatic);

      ASSERT_NE(cache->resolve("db", "t"), nullptr);
      ASSERT_TRUE(cache->entry_info("db", "t", &info));
      EXPECT_FALSE(info.is_external);
      EXPECT_EQ(info.cred_mode, PvfsFileSystem::CredMode::kToken);
      EXPECT_EQ(tokens(*inst), 1u);
      EXPECT_EQ(cache->external_token_warnings(), 0u);
    }
    user_credential_ = false;
    {
      auto inst = make(fs_opts());
      ASSERT_NE(inst, nullptr);
      auto* cache = inst->cache();
      ASSERT_NE(cache->resolve("db", "objtbl"), nullptr);
      EXPECT_EQ(tokens(*inst), 1u);
      EXPECT_EQ(cache->external_token_warnings(), 1u);
      PvfsFileSystem::TableEntryInfo info;
      ASSERT_TRUE(cache->entry_info("db", "objtbl", &info));
      EXPECT_TRUE(info.is_external);
      EXPECT_EQ(info.cred_mode, PvfsFileSystem::CredMode::kToken);
      ASSERT_NE(cache->resolve("db", "objtbl"), nullptr);  // cached
      EXPECT_EQ(cache->external_token_warnings(), 1u);
      ASSERT_NE(cache->resolve("db", "t"), nullptr);
      EXPECT_EQ(cache->external_token_warnings(), 1u);
    }
  }

  // External object and format tables in a bucket of the user's own,
  // described by the stub and read with the user's credential.
  void verify_external_tables_on_real_bucket() {
    INIT_PHOTON();
    SimBucket sim = sim_bucket();
    if (!sim.has_key() || !sim.has_bucket()) {
      GTEST_SKIP() << "PVFS_TEST_EXTERNAL_AK/SK/BUCKET/ENDPOINT not set";
    }
    start();
    DEFER(stop());
    install_routes();
    token_status_ = 403;
    std::string base = "oss://" + sim.bucket + "/pvfs-sim/";
    externals_["objtbl_byoak"] = {"object-table", base + "objtbl_byoak"};
    externals_["fmttbl_byoak"] = {"format-table", base + "fmttbl_byoak"};
    user_credential_ = true;
    user_ak_ = sim.ak;
    user_sk_ = sim.sk;
    external_endpoint_ = sim.endpoint;
    auto inst = make(fs_opts());
    ASSERT_NE(inst, nullptr);
    auto& fs = *inst->fs;

    uint64_t db = 0, obj = 0;
    ASSERT_EQ(lookup(fs, 1, "db", db), 0);
    ASSERT_EQ(lookup(fs, db, "objtbl_byoak", obj), 0);
    EXPECT_EQ(names_in(fs, obj),
              (std::vector<std::string>{"README", "big.bin", "images"}));
    EXPECT_EQ(tokens(*inst), 0u);
    uint64_t images = 0, a = 0, big = 0;
    ASSERT_EQ(lookup(fs, obj, "images", images), 0);
    ASSERT_EQ(lookup(fs, images, "a.txt", a), 0);
    EXPECT_EQ(read_file(fs, a, 64), "object a\n");
    struct stat st;
    ASSERT_EQ(fs.lookup(obj, "big.bin", &big, &st), 0);
    ASSERT_EQ(st.st_size, 3 * 1024 * 1024);
    // Straight through the store on its vCPU, then through the mount.
    std::string direct(st.st_size, '\0');
    struct iovec iov = {direct.data(), direct.size()};
    ASSERT_EQ(PERFORM_BG_ENV_OBJ_REQUEST(inst->env.env(), get_object_range,
                                         "/db/objtbl_byoak/big.bin", &iov, 1,
                                         0),
              static_cast<ssize_t>(direct.size()));
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs.open(big, O_RDONLY, &fh, &keep), 0);
    auto* h = reinterpret_cast<IFileHandleFuseLL*>(fh);
    std::string via_pvfs(direct.size(), '\0');
    const size_t step = 128 * 1024;
    for (size_t off = 0; off < via_pvfs.size(); off += step) {
      ASSERT_EQ(h->pread(&via_pvfs[off], step, off), static_cast<ssize_t>(step))
          << off;
    }
    EXPECT_EQ(fs.release(big, fh), 0);
    EXPECT_TRUE(via_pvfs == direct);

    std::string sub = scratch_name("ext");
    uint64_t dir = 0, f = 0;
    ASSERT_EQ(fs.mkdir(obj, sub, 0755, 0, 0, 0, &dir, &st), 0);
    ASSERT_EQ(write_file(fs, dir, "written.txt", "user-ak-write", f), 0);
    EXPECT_EQ(read_file(fs, f, 64), "user-ak-write");
    EXPECT_EQ(fs.unlink(dir, "written.txt"), 0);
    EXPECT_EQ(fs.rmdir(obj, sub), 0);

    uint64_t fmt = 0;
    ASSERT_EQ(lookup(fs, db, "fmttbl_byoak", fmt), 0);
    EXPECT_EQ(names_in(fs, fmt), (std::vector<std::string>{"dt=2026-09-27"}));
    PvfsFileSystem::TableEntryInfo info;
    ASSERT_TRUE(inst->cache()->entry_info("db", "fmttbl_byoak", &info));
    EXPECT_EQ(info.table_type, "format-table");
    EXPECT_EQ(info.cred_mode, PvfsFileSystem::CredMode::kStatic);
    for (uint64_t n : {f, dir, fmt, big, a, images, obj, db}) fs.forget(n, 1);
  }

  // No user credential: the catalog token is tried on a bucket it does not
  // cover, OSS refuses it, the token is fetched once more and the mount
  // reports EACCES, quickly, with no further token calls.
  void verify_external_table_without_user_key_is_eacces() {
    INIT_PHOTON();
    SimBucket sim = sim_bucket();
    if (!sim.has_bucket()) {
      GTEST_SKIP() << "PVFS_TEST_EXTERNAL_BUCKET/ENDPOINT not set";
    }
    start();
    DEFER(stop());
    install_routes();
    externals_["objtbl_byoak"] = {
        "object-table", "oss://" + sim.bucket + "/pvfs-sim/objtbl_byoak"};
    creds_ = R"("fs.oss.accessKeyId":"STS.notreal","fs.oss.accessKeySecret":"notreal",)"
             R"("fs.oss.securityToken":"notreal","fs.oss.endpoint":")" +
             sim.endpoint + "\"";
    user_credential_ = false;
    PvfsRuntimeOptions o = fs_opts();
    o.cache.oss_endpoint = sim.endpoint;
    auto inst = make(o);
    ASSERT_NE(inst, nullptr);
    auto& fs = *inst->fs;
    auto* cache = inst->cache();
    uint64_t db = 0, obj = 0;
    ASSERT_EQ(lookup(fs, 1, "db", db), 0);
    ASSERT_EQ(lookup(fs, db, "objtbl_byoak", obj), 0);  // resolves the token
    EXPECT_EQ(cache->external_token_warnings(), 1u);
    EXPECT_EQ(tokens(*inst), 1u);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t readme = 0;
    EXPECT_EQ(lookup(fs, obj, "README", readme), -EACCES);
    EXPECT_LT(ms_since(t0), kBoundMs);
    EXPECT_EQ(cache->external_token_warnings(), 1u);
    EXPECT_EQ(tokens(*inst), 2u);  // the token, then one forced refresh
    EXPECT_EQ(cache->forced_refreshes(), 1u);
    int err = 0;
    t0 = std::chrono::steady_clock::now();
    names_in(fs, obj, &err);
    EXPECT_EQ(err, -EACCES);
    EXPECT_LT(ms_since(t0), kBoundMs);
    EXPECT_EQ(cache->external_token_warnings(), 1u);
    EXPECT_EQ(tokens(*inst), 2u);
    fs.forget(obj, 1);
    fs.forget(db, 1);
  }
};

TEST_F(PvfsRestStubFsTest, ExternalTableCredentialModes) {
  verify_external_table_credential_modes();
}
TEST_F(PvfsRestStubFsTest, ExternalTablesOnRealBucket) {
  verify_external_tables_on_real_bucket();
}
TEST_F(PvfsRestStubFsTest, ExternalTableWithoutUserKeyIsEacces) {
  verify_external_table_without_user_key_is_eacces();
}
TEST_F(PvfsRestStubFsTest, TokenErrorsMapAndRecover) {
  verify_token_errors_map_and_recover();
}
TEST_F(PvfsRestStubFsTest, SlowTokenTimesOutWithoutBlockingOthers) {
  verify_slow_token_times_out_without_blocking_others();
}
TEST_F(PvfsRestStubFsTest, InitFailsFastOnBadEndpoints) {
  verify_init_fails_fast_on_bad_endpoints();
}
TEST_F(PvfsRestStubFsTest, MalformedBodiesAreEio) {
  verify_malformed_bodies_are_eio();
}
TEST_F(PvfsRestStubFsTest, NotFoundAfterTokenExpiry) {
  verify_not_found_after_token_expiry();
}
TEST_F(PvfsRestStubFsTest, CatalogDownAfterCaching) {
  verify_catalog_down_after_caching();
}
TEST_F(PvfsRestStubFsTest, LocationChangeRebuildsEnv) {
  verify_location_change_rebuilds_env();
}

}  // namespace
