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

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "pvfs/dlf4_signer.h"
#include "pvfs/pvfs_log_throttle.h"
#include "pvfs/pvfs_path.h"

using PvfsFileSystem::DlfSigner;
using PvfsFileSystem::PvfsPath;

TEST(PvfsPathParse, Levels) {
  struct Case {
    const char* in;
    PvfsPath::Level level;
    const char* db;
    const char* tbl;
    const char* sub;
  } cases[] = {
      {"/", PvfsPath::ROOT, "", "", ""},
      {"", PvfsPath::ROOT, "", "", ""},
      {"//", PvfsPath::ROOT, "", "", ""},
      {"/db", PvfsPath::DATABASE, "db", "", ""},
      {"//db//", PvfsPath::DATABASE, "db", "", ""},
      {"/db/tbl", PvfsPath::TABLE, "db", "tbl", ""},
      {"/db/tbl/", PvfsPath::TABLE, "db", "tbl", ""},
      {"/db/tbl/a", PvfsPath::SUBPATH, "db", "tbl", "a"},
      {"/db/tbl/a/b", PvfsPath::SUBPATH, "db", "tbl", "a/b"},
      {"/db/tbl/a/b/", PvfsPath::SUBPATH, "db", "tbl", "a/b"},
  };
  for (const auto& c : cases) {
    auto p = PvfsPath::parse(c.in);
    EXPECT_EQ(p.level, c.level) << c.in;
    EXPECT_EQ(p.database, c.db) << c.in;
    EXPECT_EQ(p.table, c.tbl) << c.in;
    EXPECT_EQ(p.subpath, c.sub) << c.in;
    EXPECT_EQ(p.is_virtual(), c.level == PvfsPath::ROOT ||
                                   c.level == PvfsPath::DATABASE)
        << c.in;
  }
  EXPECT_EQ(PvfsPath::parse("/db/tbl/x").cache_key(), "db/tbl");
}

// Fixed inputs: 2026-01-02T03:04:05Z, AKIDTEST/SECRETTEST, cn-hangzhou.
// The expected strings come from this implementation after comparing it
// with pypaimon's DLFDefaultSigner and DLFOpenApiSigner on the same
// inputs; see the commit message for the one known difference.
static constexpr time_t kNow = 1767323045;  // 2026-01-02T03:04:05Z

TEST(DlfSignerGolden, Dlf4Get) {
  DlfSigner signer("cn-hangzhou", "AKIDTEST", "SECRETTEST", "", false);
  signer.set_clock_for_test(kNow);
  std::map<std::string, std::string> h;
  signer.sign("GET", "/v1/config", "warehouse=c", "", h);
  EXPECT_EQ(h["x-dlf-date"], "20260102T030405Z");
  EXPECT_EQ(h["x-dlf-version"], "v1");
  EXPECT_EQ(h["x-dlf-content-sha256"], "UNSIGNED-PAYLOAD");
  EXPECT_EQ(h.count("content-md5"), 0u);
  EXPECT_EQ(h["authorization"],
            "DLF4-HMAC-SHA256 Credential=AKIDTEST/20260102/cn-hangzhou/DlfNext/"
            "aliyun_v4_request,Signature="
            "ad3b552699ad47d43e17a7964bc4c180bbb5d31e2d807714766afade43b666ac");
}

TEST(DlfSignerGolden, Dlf4Post) {
  DlfSigner signer("cn-hangzhou", "AKIDTEST", "SECRETTEST", "", false);
  signer.set_clock_for_test(kNow);
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  signer.sign("POST", "/v1/clg-x/databases", "", "{\"name\":\"db\"}", h);
  EXPECT_EQ(h["content-md5"], "6ZF45M/6TJ2FOC248EOPDg==");
  EXPECT_EQ(h["authorization"],
            "DLF4-HMAC-SHA256 Credential=AKIDTEST/20260102/cn-hangzhou/DlfNext/"
            "aliyun_v4_request,Signature="
            "7fdb0fccbbfbec9221b68fa6a30dcff482dce71f2689f977043fe0241af2338c");
}

TEST(DlfSignerGolden, Dlf4SecurityToken) {
  DlfSigner signer("cn-hangzhou", "AKIDTEST", "SECRETTEST", "STSTOKEN", false);
  signer.set_clock_for_test(kNow);
  std::map<std::string, std::string> h;
  signer.sign("GET", "/v1/config", "warehouse=c", "", h);
  EXPECT_EQ(h["x-dlf-security-token"], "STSTOKEN");
  EXPECT_NE(h["authorization"].find("Signature="), std::string::npos);
}

TEST(DlfSignerGolden, RoaPost) {
  DlfSigner signer("cn-hangzhou", "AKIDTEST", "SECRETTEST", "", true);
  signer.set_clock_for_test(kNow);
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  signer.sign("POST", "/v1/clg-x/databases", "", "{\"name\":\"db\"}", h);
  EXPECT_EQ(h["date"], "Fri, 02 Jan 2026 03:04:05 GMT");
  EXPECT_EQ(h["x-acs-version"], "2026-01-18");
  EXPECT_EQ(h["x-acs-signature-method"], "HMAC-SHA1");
  EXPECT_EQ(h["x-acs-signature-version"], "1.0");
  EXPECT_EQ(h["content-md5"], "6ZF45M/6TJ2FOC248EOPDg==");
  EXPECT_EQ(h["authorization"], "acs AKIDTEST:TRZxyW8BEmp0h0XYK9yUaz8/cm4=");
}

TEST(DlfSignerGolden, RoaGet) {
  DlfSigner signer("cn-hangzhou", "AKIDTEST", "SECRETTEST", "", true);
  signer.set_clock_for_test(kNow);
  std::map<std::string, std::string> h;
  signer.sign("GET", "/v1/config", "warehouse=c", "", h);
  EXPECT_EQ(h["date"], "Fri, 02 Jan 2026 03:04:05 GMT");
  EXPECT_EQ(h.count("content-md5"), 0u);
  // GET requests carry Content-Type: application/json and sign it, so this
  // differs from the Python signer, which omits the header without a body.
  EXPECT_EQ(h["content-type"], "application/json");
  EXPECT_EQ(h["authorization"], "acs AKIDTEST:9zBrSlhbfe8KeM4sTVem0H44z+4=");
}

// One ERROR per key and minute; a full table drops stale keys, then resets.
TEST(PvfsLogThrottle, OnePerKeyPerWindow) {
  PvfsFileSystem::PvfsLogThrottle t(60, 3);
  time_t now = 1000;
  EXPECT_TRUE(t.allow("list|/db|403", now));
  EXPECT_FALSE(t.allow("list|/db|403", now));
  EXPECT_FALSE(t.allow("list|/db|403", now + 59));
  EXPECT_TRUE(t.allow("list|/db|404", now + 59));  // another status
  EXPECT_TRUE(t.allow("list|/db|403", now + 60));  // window over
  EXPECT_FALSE(t.allow("list|/db|403", now + 61));
}

TEST(PvfsLogThrottle, FullTableDropsStaleKeysThenResets) {
  PvfsFileSystem::PvfsLogThrottle stale(60, 2);
  EXPECT_TRUE(stale.allow("a", 1000));
  EXPECT_TRUE(stale.allow("b", 1010));
  EXPECT_EQ(stale.size(), 2u);
  EXPECT_TRUE(stale.allow("c", 1060));  // "a" is stale and makes room
  EXPECT_EQ(stale.size(), 2u);
  EXPECT_FALSE(stale.allow("b", 1060));
  EXPECT_FALSE(stale.allow("c", 1061));

  PvfsFileSystem::PvfsLogThrottle live(60, 2);
  EXPECT_TRUE(live.allow("a", 1000));
  EXPECT_TRUE(live.allow("b", 1000));
  EXPECT_TRUE(live.allow("c", 1001));  // full of live keys: reset
  EXPECT_EQ(live.size(), 1u);
  EXPECT_TRUE(live.allow("a", 1001));
}

// Paimon's metadata directories by their exact first component.
TEST(PvfsPathParse, ReservedFirstComponent) {
  using PvfsFileSystem::is_reserved_first_component;
  for (const char* name : {"snapshot", "manifest", "schema", "index",
                           "changelog", "statistics", "tag", "branch",
                           "consumer", "bucket-0", "bucket-", "bucket-12/x",
                           "snapshot/x", "/snapshot", "manifest/a/b"}) {
    EXPECT_TRUE(is_reserved_first_component(name)) << name;
  }
  for (const char* name : {"", "/", "data", "data/snapshot", "bucketx",
                           "bucket", "Snapshot", "snapshots", "tags",
                           "index.html", "consumer2"}) {
    EXPECT_FALSE(is_reserved_first_component(name)) << name;
  }
}
