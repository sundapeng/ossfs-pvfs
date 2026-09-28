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

#include <string>
#include <vector>

#include "pvfs/paimon_rest_client.h"
#include "pvfs_rest_stub.h"

using PvfsFileSystem::PaimonRESTClient;
using PvfsFileSystem::PaimonRESTClientOptions;
using PvfsFileSystem::TableInfo;

namespace {

// The REST client alone against the stub: prefix, paging, status mapping.
class PvfsRestStubTest : public RestStubFixture {
 protected:
  PaimonRESTClientOptions opts() {
    PaimonRESTClientOptions o;
    o.endpoint = endpoint();
    o.region = "cn-test";
    o.catalog = "cat";
    o.access_key_id = "ak";
    o.access_key_secret = "sk";
    o.signing_algorithm = "default";
    o.list_page_size = 1;
    return o;
  }
};

TEST_F(PvfsRestStubTest, ConfigPrefixAndPaging) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  stub_.route = [](const std::string& t, const std::string& q) {
    if (t == "/v1/config") return json(200, R"({"defaults":{"prefix":"pfx"}})");
    if (t == "/v1/pfx/databases") {
      if (q.find("pageToken=") == std::string::npos)
        return json(200, R"({"databases":["a"],"nextPageToken":"a"})");
      return json(200, R"({"databases":[{"name":"b"}]})");
    }
    if (t == "/v1/pfx/databases/a/tables") {
      if (q.find("pageToken=") == std::string::npos)
        return json(200, R"({"tables":["t1"],"nextPageToken":"t1"})");
      if (q.find("pageToken=t1") != std::string::npos)
        return json(200, R"({"tables":["t2"],"nextPageToken":"t2"})");
      return json(200, R"({"tables":["t3"]})");
    }
    return json(404, "{}");
  };
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  std::vector<std::string> dbs;
  ASSERT_EQ(client.list_databases(dbs), 0);
  EXPECT_EQ(dbs, (std::vector<std::string>{"a", "b"}));
  std::vector<std::string> tables;
  ASSERT_EQ(client.list_tables("a", tables), 0);
  EXPECT_EQ(tables, (std::vector<std::string>{"t1", "t2", "t3"}));
  EXPECT_EQ(client.stats().list_tables, 3u);
  EXPECT_EQ(stub_.requests[0], "/v1/config?warehouse=cat");
  // Every call, the config probe included, identifies itself as ossfs2.
  std::string ua = PaimonRESTClient::user_agent();
  EXPECT_EQ(ua.compare(0, 11, "ossfs2-pvfs"), 0) << ua;
  EXPECT_NE(ua.find('/'), std::string::npos) << ua;
  ASSERT_EQ(stub_.user_agents.size(), stub_.requests.size());
  for (size_t i = 0; i < stub_.user_agents.size(); i++) {
    EXPECT_EQ(stub_.user_agents[i], ua) << stub_.requests[i];
  }
}

TEST_F(PvfsRestStubTest, EmptyJsonResponsesAreRejectedWithoutLeaks) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  stub_.route = [](const std::string &path, const std::string &) {
    if (path == "/v1/config")
      return json(200, R"({"defaults":{"prefix":"pfx"}})");
    return json(200, R"({"databases":[]})");
  };
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  stub_.route = [](const std::string &, const std::string &) {
    return json(200, "");
  };
  std::vector<std::string> names;
  TableInfo info;
  PvfsFileSystem::TableCredential credential;
  EXPECT_EQ(client.list_databases(names), -EIO);
  EXPECT_EQ(client.list_tables("db", names), -EIO);
  EXPECT_EQ(client.get_table("db", "t", info), -EIO);
  EXPECT_EQ(client.get_table_token("db", "t", credential), -EIO);
  // Existence checks need only the HTTP status, not a JSON document.
  EXPECT_EQ(client.get_database("db"), 0);
}

TEST_F(PvfsRestStubTest, ConfigFallbackOnlyOn404) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  int config_status = 404;
  stub_.route = [&](const std::string& t, const std::string&) {
    if (t == "/v1/config") return json(config_status, "{}");
    if (t == "/v1/catalogs/cat/databases")
      return json(200, R"({"databases":[]})");
    return json(404, "{}");
  };
  {
    PaimonRESTClient client(opts());
    EXPECT_EQ(client.init(), 0);  // 404: older server, fallback prefix
    EXPECT_EQ(stub_.requests.back(), "/v1/catalogs/cat/databases?maxResults=1");
  }
  for (int status : {403, 500, 400}) {
    stub_.requests.clear();
    config_status = status;
    PaimonRESTClient client(opts());
    EXPECT_EQ(client.init(), PaimonRESTClient::status_to_errno(status))
        << status;
    EXPECT_EQ(stub_.requests.size(), 1u) << status;  // no probe after that
  }
}

TEST_F(PvfsRestStubTest, StatusMappingAndEncoding) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  stub_.route = [](const std::string& t, const std::string&) {
    if (t == "/v1/config") return json(200, R"({"defaults":{"prefix":"pfx"}})");
    if (t == "/v1/pfx/databases") return json(200, R"({"databases":[]})");
    if (t == "/v1/pfx/databases/db/tables/missing") return json(404, "{}");
    if (t == "/v1/pfx/databases/db/tables/busy") return json(429, "{}");
    if (t == "/v1/pfx/databases/db/tables/broken") return json(500, "{}");
    if (t == "/v1/pfx/databases/db/tables/secret") return json(403, "{}");
    if (t == "/v1/pfx/databases/my%20db/tables/t%231")
      return json(200, R"({"location":"oss://b/p/t"})");
    return json(404, "{}");
  };
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  TableInfo info;
  EXPECT_EQ(client.get_table("db", "missing", info), -ENOENT);
  EXPECT_EQ(client.get_table("db", "busy", info), -EAGAIN);
  EXPECT_EQ(client.get_table("db", "broken", info), -EIO);
  EXPECT_EQ(client.get_table("db", "secret", info), -EACCES);
  EXPECT_EQ(client.get_database("nope"), -ENOENT);
  EXPECT_EQ(client.get_table("my db", "t#1", info), 0);
  EXPECT_EQ(info.location, "oss://b/p/t");
  EXPECT_EQ(stub_.requests.back(), "/v1/pfx/databases/my%20db/tables/t%231");
}

// A server that hands back the token it was just given, or a fresh one
// forever, must not keep the client walking: EIO after a bounded walk.
TEST_F(PvfsRestStubTest, PagingLoopIsBounded) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  enum { kNormal, kRepeat, kEndless } mode = kNormal;
  int served = 0;
  stub_.route = [&](const std::string& t, const std::string& q) {
    if (t == "/v1/config") return json(200, R"({"defaults":{"prefix":"pfx"}})");
    if (t != "/v1/pfx/databases") return json(404, "{}");
    served++;
    if (mode == kNormal) return json(200, R"({"databases":["a"]})");
    if (mode == kRepeat)
      return json(200, R"({"databases":["a"],"nextPageToken":"a"})");
    return json(200, R"({"databases":["a"],"nextPageToken":"p)" +
                          std::to_string(served) + R"("})");
  };
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  std::vector<std::string> dbs;

  mode = kRepeat;
  served = 0;
  auto start_ts = std::chrono::steady_clock::now();
  EXPECT_EQ(client.list_databases(dbs), -EIO);
  EXPECT_LT(ms_since(start_ts), 5000);
  EXPECT_EQ(served, 2);  // the repeated token is spotted on the second page

  mode = kEndless;
  served = 0;
  start_ts = std::chrono::steady_clock::now();
  EXPECT_EQ(client.list_databases(dbs), -EIO);
  EXPECT_LT(ms_since(start_ts), 5000);
  EXPECT_EQ(served, PaimonRESTClient::kMaxListPages);

  // The client is still usable afterwards.
  mode = kNormal;
  served = 0;
  EXPECT_EQ(client.list_databases(dbs), 0);
  EXPECT_EQ(dbs, std::vector<std::string>{"a"});
}

// GetTable as Bennett sends it: the path is top-level, the type sits in
// schema.options, and isExternal marks a user-managed location.
TEST_F(PvfsRestStubTest, GetTableBennettShape) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  const std::string bennett = R"({"id":"tbl-a7525909","database":"db","name":"objtbl",)"
      R"("path":"oss://other-bucket/prefix/objtbl","isExternal":true,"schemaId":0,)"
      R"("schema":{"fields":[{"id":0,"name":"f","type":"INT"}],"partitionKeys":[],)"
      R"("primaryKeys":[],"options":{"type":"object-table",)"
      R"("path":"oss://other-bucket/prefix/objtbl","bucket":"-1"},"comment":"c"},)"
      R"("owner":"1045689747920334","createdAt":1790000000000,"createdBy":"u",)"
      R"("updatedAt":1790000000000,"updatedBy":"u"})";
  stub_.route = [&](const std::string& t, const std::string&) {
    if (t == "/v1/config") return json(200, R"({"defaults":{"prefix":"pfx"}})");
    if (t == "/v1/pfx/databases") return json(200, R"({"databases":[]})");
    if (t == "/v1/pfx/databases/db/tables/objtbl") return json(200, bennett);
    if (t == "/v1/pfx/databases/db/tables/plain")
      return json(200, R"({"id":"tbl-2","name":"plain","path":"oss://managed/db/plain",)"
                       R"("isExternal":false,"schema":{"options":{"bucket":"-1"}}})");
    if (t == "/v1/pfx/databases/db/tables/legacy")
      return json(200, R"({"location":"oss://managed/db/legacy"})");
    if (t == "/v1/pfx/databases/db/tables/optpath")
      return json(200, R"({"name":"optpath","schema":{"options":{"type":"format-table",)"
                       R"("path":"oss://managed/db/optpath"}}})");
    return json(404, "{}");
  };
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);

  TableInfo info;
  ASSERT_EQ(client.get_table("db", "objtbl", info), 0);
  EXPECT_EQ(info.location, "oss://other-bucket/prefix/objtbl");
  EXPECT_TRUE(info.is_external);
  EXPECT_EQ(info.table_type, "object-table");
  EXPECT_EQ(info.options.at("path"), "oss://other-bucket/prefix/objtbl");
  EXPECT_EQ(info.options.at("bucket"), "-1");
  EXPECT_EQ(info.options.size(), 3u);

  info = TableInfo();
  ASSERT_EQ(client.get_table("db", "plain", info), 0);
  EXPECT_EQ(info.location, "oss://managed/db/plain");
  EXPECT_FALSE(info.is_external);
  EXPECT_EQ(info.table_type, "");
  EXPECT_EQ(info.options.size(), 1u);

  info = TableInfo();
  ASSERT_EQ(client.get_table("db", "legacy", info), 0);
  EXPECT_EQ(info.location, "oss://managed/db/legacy");
  EXPECT_FALSE(info.is_external);

  info = TableInfo();
  ASSERT_EQ(client.get_table("db", "optpath", info), 0);
  EXPECT_EQ(info.location, "oss://managed/db/optpath");
  EXPECT_EQ(info.table_type, "format-table");
}

}  // namespace
