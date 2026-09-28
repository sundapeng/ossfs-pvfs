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

#include <sys/statvfs.h>

#include "common/logger.h"

class PvfsBasicTest : public PvfsTestSuite {
 protected:
  void verify_init() {
    INIT_PHOTON();
    init();
    ASSERT_NE(fs_, nullptr);
  }

  void verify_statfs() {
    INIT_PHOTON();
    init();

    struct statvfs stbuf;
    int r = fs_->statfs(&stbuf);
    ASSERT_EQ(r, 0);
    ASSERT_GT(stbuf.f_bsize, 0UL);
    ASSERT_GT(stbuf.f_blocks, 0UL);
    ASSERT_EQ(stbuf.f_namemax, 255UL);
  }

  void verify_list_databases() {
    INIT_PHOTON();
    init();

    std::vector<DirEntry> entries;
    int r = list_dir(root_nodeid_, entries);
    ASSERT_EQ(r, 0);
    ASSERT_GT(entries.size(), 0UL);

    LOG_INFO("Found ` databases", entries.size());
    for (const auto& e : entries) {
      LOG_INFO("  database: ` (nodeid=`)", e.name, e.nodeid);
      ASSERT_TRUE(e.is_dir);
    }
  }

  void verify_lookup_database() {
    INIT_PHOTON();
    init();

    // List databases first to get a valid name
    std::vector<DirEntry> dbs;
    int r = list_dir(root_nodeid_, dbs);
    ASSERT_EQ(r, 0);
    ASSERT_GT(dbs.size(), 0UL);

    // Lookup the first database
    uint64_t nodeid = 0;
    struct stat st;
    r = fs_->lookup(root_nodeid_, dbs[0].name, &nodeid, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    DEFER(fs_->forget(nodeid, 1));

    // Lookup non-existent database
    uint64_t bad_nodeid = 0;
    r = fs_->lookup(root_nodeid_, "nonexistent_db_12345", &bad_nodeid, &st);
    ASSERT_EQ(r, -ENOENT);
  }

  void verify_list_tables() {
    INIT_PHOTON();
    init();

    // Get first database
    std::vector<DirEntry> dbs;
    int r = list_dir(root_nodeid_, dbs);
    ASSERT_EQ(r, 0);
    ASSERT_GT(dbs.size(), 0UL);

    // Find a database with tables
    for (const auto& db : dbs) {
      uint64_t db_nodeid = 0;
      struct stat st;
      r = fs_->lookup(root_nodeid_, db.name, &db_nodeid, &st);
      if (r != 0) continue;
      DEFER(fs_->forget(db_nodeid, 1));

      std::vector<DirEntry> tables;
      r = list_dir(db_nodeid, tables);
      ASSERT_EQ(r, 0);

      if (tables.empty()) {
        LOG_INFO("Database ` has no tables, skipping", db.name);
        continue;
      }

      LOG_INFO("Database ` has ` tables", db.name, tables.size());
      for (const auto& t : tables) {
        LOG_INFO("  table: ` (nodeid=`)", t.name, t.nodeid);
        ASSERT_TRUE(t.is_dir);
      }
      return;  // found at least one database with tables
    }

    LOG_WARN("No database with tables found, test inconclusive");
  }

  void verify_lookup_table() {
    INIT_PHOTON();
    init();

    // Get first database
    std::vector<DirEntry> dbs;
    int r = list_dir(root_nodeid_, dbs);
    ASSERT_EQ(r, 0);
    ASSERT_GT(dbs.size(), 0UL);

    for (const auto& db : dbs) {
      uint64_t db_nodeid = 0;
      struct stat st;
      r = fs_->lookup(root_nodeid_, db.name, &db_nodeid, &st);
      if (r != 0) continue;
      DEFER(fs_->forget(db_nodeid, 1));

      std::vector<DirEntry> tables;
      r = list_dir(db_nodeid, tables);
      if (r != 0 || tables.empty()) continue;

      // Lookup the first table
      uint64_t tbl_nodeid = 0;
      r = fs_->lookup(db_nodeid, tables[0].name, &tbl_nodeid, &st);
      ASSERT_EQ(r, 0);
      ASSERT_TRUE(S_ISDIR(st.st_mode));
      fs_->forget(tbl_nodeid, 1);

      // Lookup non-existent table
      uint64_t bad_nodeid = 0;
      r = fs_->lookup(db_nodeid, "nonexistent_table_12345", &bad_nodeid, &st);
      ASSERT_EQ(r, -ENOENT);
      return;
    }

    LOG_WARN("No database with tables found");
  }

  void verify_getattr_root() {
    INIT_PHOTON();
    init();

    struct stat st;
    int r = fs_->getattr(root_nodeid_, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
  }
};

TEST_F(PvfsBasicTest, verify_init) { verify_init(); }

TEST_F(PvfsBasicTest, verify_statfs) { verify_statfs(); }

TEST_F(PvfsBasicTest, verify_getattr_root) { verify_getattr_root(); }

TEST_F(PvfsBasicTest, verify_list_databases) { verify_list_databases(); }

TEST_F(PvfsBasicTest, verify_lookup_database) { verify_lookup_database(); }

TEST_F(PvfsBasicTest, verify_list_tables) { verify_list_tables(); }

TEST_F(PvfsBasicTest, verify_lookup_table) { verify_lookup_table(); }
