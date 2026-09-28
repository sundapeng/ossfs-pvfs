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

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "options.h"
#include "pvfs_options.h"

using PvfsFileSystem::derive_region_from_endpoint;
using PvfsFileSystem::pvfs_mode_selected;
using PvfsFileSystem::pvfs_runtime_options_from_flags;
using PvfsFileSystem::PvfsRuntimeOptions;

namespace {

uint8_t find_mode(std::string_view name) {
  for (const auto& category : OptionsRegistry::get_all_options()) {
    for (const auto& option : category) {
      if (option.name == name) return option.modes;
    }
  }
  ADD_FAILURE() << "option not registered: " << name;
  return 0;
}

bool contains(const std::vector<std::string>& v, std::string_view name) {
  return std::find(v.begin(), v.end(), name) != v.end();
}

// Flags a mount would set; restored by the FlagSaver of each test.
void set_pvfs_flags() {
  FLAGS_pvfs_catalog = "cat";
  FLAGS_pvfs_endpoint = "dlfnext.cn-shanghai.aliyuncs.com";
  FLAGS_pvfs_access_key_id = "ak";
  FLAGS_pvfs_access_key_secret = "sk";
  // The live suite passes these on the command line; start from empty.
  FLAGS_pvfs_region.clear();
  FLAGS_pvfs_oss_endpoint.clear();
  FLAGS_pvfs_security_token.clear();
}

}  // namespace

TEST(PvfsOptions, ModeDetection) {
  gflags::FlagSaver saver;
  std::string catalog;
  FLAGS_pvfs_catalog.clear();
  FLAGS_oss_bucket.clear();
  EXPECT_FALSE(pvfs_mode_selected(&catalog));
  FLAGS_oss_bucket = "bucket";
  EXPECT_FALSE(pvfs_mode_selected(&catalog));
  FLAGS_oss_bucket = "pvfs://from_bucket";
  EXPECT_TRUE(pvfs_mode_selected(&catalog));
  EXPECT_EQ(catalog, "from_bucket");
  FLAGS_pvfs_catalog = "from_flag";
  EXPECT_TRUE(pvfs_mode_selected(&catalog));
  EXPECT_EQ(catalog, "from_flag");
}

TEST(PvfsOptions, OssTargetOnlyRequiredOutsidePvfs) {
  gflags::FlagSaver saver;
  // The requirement is installed by main for OSS/HDFS mounts only, so a
  // PVFS mount never sees it; here the check itself is exercised.
  FLAGS_oss_endpoint.clear();
  FLAGS_oss_bucket.clear();
  EXPECT_FALSE(OptionsRegistry::require_oss_target());
  FLAGS_oss_endpoint = "oss-cn-hangzhou.aliyuncs.com";
  FLAGS_oss_bucket = "bucket";
  EXPECT_TRUE(OptionsRegistry::require_oss_target());
}

TEST(PvfsOptions, SensitiveAndModes) {
  const auto& s = OptionsRegistry::kSensitiveOptions;
  EXPECT_TRUE(s.count("pvfs_access_key_id"));
  EXPECT_TRUE(s.count("pvfs_access_key_secret"));
  EXPECT_TRUE(s.count("pvfs_security_token"));

  EXPECT_EQ(find_mode("pvfs_catalog"), OptionsRegistry::kModePvfs);
  EXPECT_EQ(find_mode("pvfs_max_table_cache"), OptionsRegistry::kModePvfs);
  EXPECT_EQ(find_mode("oss_vcpu_count"), OptionsRegistry::kModeOssPvfs);
  EXPECT_EQ(find_mode("upload_buffer_size"), OptionsRegistry::kModeOssPvfs);
  EXPECT_EQ(find_mode("enable_ipv6"), OptionsRegistry::kModeOssPvfs);
  EXPECT_EQ(find_mode("ram_role"), OptionsRegistry::kModeOssHdfs);
  EXPECT_EQ(find_mode("attr_timeout"), OptionsRegistry::kModeAll);

  auto always_set = [](std::string_view) { return true; };
  auto ignored = OptionsRegistry::get_inapplicable_options(
      OptionsRegistry::kModePvfs, always_set);
  for (const char* name : {"ram_role", "credential_process",
                           "disk_data_cache_dir", "enable_crc64",
                           "memory_data_cache_size", "temp_dir"}) {
    EXPECT_TRUE(contains(ignored, name)) << name;
  }
  for (const char* name : {"pvfs_catalog", "oss_vcpu_count",
                           "upload_buffer_size", "enable_ipv6", "attr_timeout",
                           "close_to_open", "uid", "log_dir"}) {
    EXPECT_FALSE(contains(ignored, name)) << name;
  }
  auto oss_ignored = OptionsRegistry::get_inapplicable_options(
      OptionsRegistry::kModeOss, always_set);
  EXPECT_TRUE(contains(oss_ignored, "pvfs_catalog"));
  EXPECT_FALSE(contains(oss_ignored, "enable_crc64"));
  EXPECT_STREQ(OptionsRegistry::mode_name(OptionsRegistry::kModePvfs), "PVFS");
}

TEST(PvfsOptions, Validators) {
  gflags::FlagSaver saver;
  EXPECT_TRUE(gflags::SetCommandLineOption("pvfs_signing_algorithm", "bogus")
                  .empty());
  EXPECT_FALSE(gflags::SetCommandLineOption("pvfs_signing_algorithm", "openapi")
                   .empty());
  EXPECT_FALSE(gflags::SetCommandLineOption("pvfs_signing_algorithm", "default")
                   .empty());
  EXPECT_TRUE(
      gflags::SetCommandLineOption("pvfs_credential_refresh_ahead", "1800")
          .empty());
  EXPECT_TRUE(
      gflags::SetCommandLineOption("pvfs_credential_refresh_ahead", "-1")
          .empty());
  EXPECT_FALSE(
      gflags::SetCommandLineOption("pvfs_credential_refresh_ahead", "120")
          .empty());
}

TEST(PvfsOptions, RegionDerivation) {
  EXPECT_EQ(derive_region_from_endpoint("dlfnext.cn-hangzhou.aliyuncs.com"),
            "cn-hangzhou");
  EXPECT_EQ(derive_region_from_endpoint(
                "https://dlfnext.ap-southeast-1.aliyuncs.com"),
            "ap-southeast-1");
  EXPECT_EQ(derive_region_from_endpoint(
                "http://dlf-regres-test-cn-hangzhou-vpc.example.com"),
            "cn-hangzhou");
  EXPECT_EQ(derive_region_from_endpoint("dlf-prod-us-east-1-vpc.example.com"),
            "us-east-1");
  EXPECT_EQ(derive_region_from_endpoint("dlf.internal.example.com"), "");
  EXPECT_EQ(derive_region_from_endpoint("127.0.0.1:8080"), "");
}

TEST(PvfsOptions, FlagWiringDefaults) {
  gflags::FlagSaver saver;
  set_pvfs_flags();
  PvfsRuntimeOptions o;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_EQ(o.rest.catalog, "cat");
  EXPECT_EQ(o.rest.region, "cn-shanghai");  // derived from the endpoint
  EXPECT_EQ(o.rest.access_key_id, "ak");
  EXPECT_EQ(o.cache.max_table_cache, 50);
  EXPECT_FALSE(o.rest.enable_ipv6);
  EXPECT_FALSE(o.cache.enable_ipv6);
  EXPECT_TRUE(o.prefix.empty());
  EXPECT_TRUE(o.readonly);
  EXPECT_EQ(o.cache.store_options.max_list_ret_cnt, 100);
}

TEST(PvfsOptions, FlagWiringExplicit) {
  gflags::FlagSaver saver;
  set_pvfs_flags();
  FLAGS_pvfs_region = "cn-beijing";
  FLAGS_pvfs_max_table_cache = 7;
  FLAGS_oss_bucket_prefix = "/db/tbl/";
  FLAGS_max_list_ret_count = 500;
  FLAGS_pvfs_location_cache_ttl = 9;
  FLAGS_pvfs_credential_refresh_ahead = 11;
  // These count as "given" only when set the way the command line does.
  ASSERT_FALSE(gflags::SetCommandLineOption("enable_ipv6", "true").empty());
  PvfsRuntimeOptions o;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_EQ(o.rest.region, "cn-beijing");  // the flag overrides derivation
  EXPECT_EQ(o.cache.max_table_cache, 7);
  EXPECT_EQ(o.cache.location_cache_ttl_sec, 9);
  EXPECT_EQ(o.cache.credential_refresh_ahead_sec, 11);
  EXPECT_TRUE(o.rest.enable_ipv6);
  EXPECT_TRUE(o.cache.enable_ipv6);
  EXPECT_EQ(o.prefix, "db/tbl");  // slashes trimmed, as OSS mode does
  EXPECT_EQ(o.cache.store_options.max_list_ret_cnt, 500);
}

TEST(PvfsOptions, FlagWiringRejects) {
  gflags::FlagSaver saver;
  set_pvfs_flags();
  PvfsRuntimeOptions o;
  FLAGS_pvfs_endpoint = "dlf.internal.example.com";  // no region derivable
  EXPECT_EQ(pvfs_runtime_options_from_flags("cat", &o), -EINVAL);
  FLAGS_pvfs_region = "cn-hangzhou";
  EXPECT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  FLAGS_pvfs_access_key_secret.clear();
  unsetenv("PVFS_ACCESS_KEY_SECRET");
  EXPECT_EQ(pvfs_runtime_options_from_flags("cat", &o), -EINVAL);
  setenv("PVFS_ACCESS_KEY_SECRET", "from_env", 1);
  EXPECT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_EQ(o.rest.access_key_secret, "from_env");
  unsetenv("PVFS_ACCESS_KEY_SECRET");
}

// External tables: the user's OSS credential is taken the way OSS mode
// takes it, and the external endpoint defaults to the PVFS one.
TEST(PvfsOptions, ExternalTableCredentialAndEndpoint) {
  gflags::FlagSaver saver;
  set_pvfs_flags();
  EXPECT_EQ(find_mode("pvfs_external_oss_endpoint"), OptionsRegistry::kModePvfs);
  EXPECT_EQ(find_mode("oss_access_key_id"), OptionsRegistry::kModeAll);
  auto always_set = [](std::string_view) { return true; };
  auto ignored = OptionsRegistry::get_inapplicable_options(
      OptionsRegistry::kModePvfs, always_set);
  EXPECT_FALSE(contains(ignored, "oss_access_key_id"));
  EXPECT_FALSE(contains(ignored, "oss_access_key_secret"));
  EXPECT_FALSE(contains(ignored, "pvfs_external_oss_endpoint"));

  unsetenv("OSS_ACCESS_KEY_ID");
  unsetenv("OSS_ACCESS_KEY_SECRET");
  FLAGS_pvfs_oss_endpoint = "oss-cn-shanghai.aliyuncs.com";
  PvfsRuntimeOptions o;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_EQ(o.cache.oss_endpoint, "oss-cn-shanghai.aliyuncs.com");
  EXPECT_EQ(o.cache.external_oss_endpoint, "oss-cn-shanghai.aliyuncs.com");
  EXPECT_TRUE(o.cache.static_access_key_id.empty());
  EXPECT_TRUE(o.cache.static_access_key_secret.empty());

  setenv("OSS_ACCESS_KEY_ID", "env-id", 1);
  setenv("OSS_ACCESS_KEY_SECRET", "env-secret", 1);
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_EQ(o.cache.static_access_key_id, "env-id");
  EXPECT_EQ(o.cache.static_access_key_secret, "env-secret");

  for (auto [name, value] :
       {std::pair{"oss_access_key_id", "flag-id"},
        std::pair{"oss_access_key_secret", "flag-secret"},
        std::pair{"pvfs_external_oss_endpoint", "oss-cn-beijing.aliyuncs.com"}}) {
    ASSERT_FALSE(gflags::SetCommandLineOption(name, value).empty()) << name;
  }
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_EQ(o.cache.static_access_key_id, "flag-id");
  EXPECT_EQ(o.cache.static_access_key_secret, "flag-secret");
  EXPECT_EQ(o.cache.external_oss_endpoint, "oss-cn-beijing.aliyuncs.com");
  unsetenv("OSS_ACCESS_KEY_ID");
  unsetenv("OSS_ACCESS_KEY_SECRET");
}

// Writes are opt-in: read-only by default, --pvfs_allow_write enables
// them, --ro still wins; metadata writes are a second opt-in.
TEST(PvfsOptions, ReadOnlyByDefaultAndMetadataWrites) {
  gflags::FlagSaver saver;
  set_pvfs_flags();
  EXPECT_TRUE(PvfsRuntimeOptions().readonly);
  EXPECT_EQ(find_mode("pvfs_allow_write"), OptionsRegistry::kModePvfs);
  EXPECT_EQ(find_mode("pvfs_allow_metadata_write"), OptionsRegistry::kModePvfs);

  PvfsRuntimeOptions o;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_TRUE(o.readonly);
  EXPECT_FALSE(o.allow_metadata_write);

  FLAGS_pvfs_allow_write = true;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_FALSE(o.readonly);

  FLAGS_ro = true;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_TRUE(o.readonly);

  FLAGS_ro = false;
  FLAGS_pvfs_allow_metadata_write = true;
  ASSERT_EQ(pvfs_runtime_options_from_flags("cat", &o), 0);
  EXPECT_FALSE(o.readonly);
  EXPECT_TRUE(o.allow_metadata_write);
}
