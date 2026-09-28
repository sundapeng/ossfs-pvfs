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
#include <sys/statvfs.h>

#include <chrono>
#include <thread>

#include "common/fuse.h"
#include "common/logger.h"

class PvfsFileOpsTest : public PvfsTestSuite {
 protected:
  void verify_readdir_table() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::vector<DirEntry> entries;
    int r = list_dir(tbl_nodeid, entries);
    ASSERT_EQ(r, 0);
    LOG_INFO("Table has ` entries", entries.size());
    for (const auto& e : entries) {
      LOG_INFO("  ` (dir=`, nodeid=`)", e.name, e.is_dir, e.nodeid);
      fs_->forget(e.nodeid, 1);
    }
  }

  void verify_mkdir_rmdir() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    // Create a test directory
    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    // Verify it exists via lookup
    uint64_t lookup_nodeid = 0;
    r = fs_->lookup(tbl_nodeid, dirname, &lookup_nodeid, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    fs_->forget(lookup_nodeid, 1);

    // Remove it
    r = fs_->rmdir(tbl_nodeid, dirname);
    ASSERT_EQ(r, 0);
    fs_->forget(dir_nodeid, 1);

    // Verify it's gone
    r = fs_->lookup(tbl_nodeid, dirname, &lookup_nodeid, &st);
    ASSERT_EQ(r, -ENOENT);
  }

  void verify_create_write_read() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    // Create test dir
    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "test_file.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    // Create and write a file
    std::string test_data = "Hello PVFS test! " + test_subdir_;
    uint64_t file_nodeid = 0;
    ssize_t written =
        create_and_write(dir_nodeid, "test_file.txt", test_data.data(),
                         test_data.size(), file_nodeid);
    ASSERT_EQ(written, (ssize_t)test_data.size());
    DEFER(fs_->forget(file_nodeid, 1));

    // Verify via lookup
    uint64_t lookup_nodeid = 0;
    r = fs_->lookup(dir_nodeid, "test_file.txt", &lookup_nodeid, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISREG(st.st_mode));
    fs_->forget(lookup_nodeid, 1);

    // Read back
    std::string content;
    ssize_t bytes_read = read_file(file_nodeid, content);
    ASSERT_EQ(bytes_read, (ssize_t)test_data.size());
    ASSERT_EQ(content, test_data);
  }

  // bash `>>` closes a dup'ed fd (a FUSE FLUSH) before the write lands; the
  // first write must survive that flush.
  void verify_flush_between_writes() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "flushed.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    uint64_t file_nodeid = 0;
    void* fh = nullptr;
    r = fs_->creat(dir_nodeid, "flushed.txt", O_CREAT | O_RDWR, 0644, 0, 0, 0,
                   &file_nodeid, &st, &fh);
    ASSERT_EQ(r, 0);
    DEFER(fs_->forget(file_nodeid, 1));
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    std::string first = "first-" + test_subdir_ + "\n";
    std::string second = "second\n";
    ASSERT_EQ(handle->pwrite(first.data(), first.size(), 0),
              (ssize_t)first.size());
    ASSERT_EQ(fs_->flush(file_nodeid, fh), 0);
    ASSERT_EQ(handle->pwrite(second.data(), second.size(), first.size()),
              (ssize_t)second.size());
    ASSERT_EQ(fs_->release(file_nodeid, fh), 0);

    std::string content;
    ASSERT_EQ(read_file(file_nodeid, content),
              (ssize_t)(first.size() + second.size()));
    ASSERT_EQ(content, first + second);
  }

  void verify_append_after_flush() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "append.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    std::string line1 = "line1\n";
    std::string line2 = "line2\n";
    uint64_t file_nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "append.txt", line1.data(),
                               line1.size(), file_nodeid),
              (ssize_t)line1.size());
    DEFER(fs_->forget(file_nodeid, 1));

    void* fh = nullptr;
    bool keep_page_cache = false;
    r = fs_->open(file_nodeid, O_WRONLY | O_APPEND, &fh, &keep_page_cache);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(fs_->flush(file_nodeid, fh), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ASSERT_EQ(handle->pwrite(line2.data(), line2.size(), line1.size()),
              (ssize_t)line2.size());
    ASSERT_EQ(fs_->release(file_nodeid, fh), 0);

    std::string content;
    ASSERT_EQ(read_file(file_nodeid, content),
              (ssize_t)(line1.size() + line2.size()));
    ASSERT_EQ(content, line1 + line2);
  }

  // libfuse delivers spliced writes as a pipe fd without FUSE_BUF_FD_SEEK;
  // the handle must read it forward instead of pread()ing it.
  void verify_write_buf_pipe() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "spliced.bin");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    std::string data(16 * 1024, '\0');
    for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<char>(i * 7);
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_EQ(write(pipefd[1], data.data(), data.size()), (ssize_t)data.size());
    close(pipefd[1]);
    DEFER(close(pipefd[0]));

    uint64_t file_nodeid = 0;
    void* fh = nullptr;
    r = fs_->creat(dir_nodeid, "spliced.bin", O_CREAT | O_WRONLY, 0644, 0, 0,
                   0, &file_nodeid, &st, &fh);
    ASSERT_EQ(r, 0);
    DEFER(fs_->forget(file_nodeid, 1));
    struct fuse_bufvec bufv = FUSE_BUFVEC_INIT(data.size());
    bufv.buf[0].flags = FUSE_BUF_IS_FD;
    bufv.buf[0].fd = pipefd[0];
    ASSERT_EQ(fs_->write_buf(file_nodeid, fh, &bufv, 0), (ssize_t)data.size());
    ASSERT_EQ(fs_->release(file_nodeid, fh), 0);

    std::string content;
    ASSERT_EQ(read_file(file_nodeid, content), (ssize_t)data.size());
    ASSERT_EQ(content, data);
  }

  // A refused create at the root must not poison later creates.
  void verify_creat_after_virtual_refusal() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    uint64_t nodeid = 0;
    struct stat st;
    void* fh = nullptr;
    int r = fs_->creat(root_nodeid_, "illegal_file.txt", O_CREAT | O_WRONLY,
                       0644, 0, 0, 0, &nodeid, &st, &fh);
    ASSERT_EQ(r, -EPERM);

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "m.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });
    std::string data = "first\n";
    uint64_t file_nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "m.txt", data.data(), data.size(),
                               file_nodeid),
              (ssize_t)data.size());
    DEFER(fs_->forget(file_nodeid, 1));
    std::string content;
    ASSERT_EQ(read_file(file_nodeid, content), (ssize_t)data.size());
    ASSERT_EQ(content, data);
  }

  // A negative lookup is cached; creating that name (file or dir) must
  // erase the entry, and the entry must be private to the table.
  void verify_negative_cache_scoping() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions&,
                     OssFileSystem::OssFsOptions& fo) {
      fo.oss_negative_cache_size = 10000;
      fo.oss_negative_cache_timeout = 5;
    };
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "later.txt");
      fs_->rmdir(dir_nodeid, "later_dir");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    uint64_t nodeid = 0;
    ASSERT_EQ(fs_->lookup(dir_nodeid, "later.txt", &nodeid, &st), -ENOENT);
    ASSERT_EQ(fs_->lookup(dir_nodeid, "later_dir", &nodeid, &st), -ENOENT);

    std::string data = "x";
    uint64_t file_nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "later.txt", data.data(),
                               data.size(), file_nodeid),
              (ssize_t)data.size());
    DEFER(fs_->forget(file_nodeid, 1));
    ASSERT_EQ(fs_->lookup(dir_nodeid, "later.txt", &nodeid, &st), 0);
    ASSERT_TRUE(S_ISREG(st.st_mode));
    fs_->forget(nodeid, 1);

    uint64_t sub_nodeid = 0;
    ASSERT_EQ(fs_->mkdir(dir_nodeid, "later_dir", 0755, 0, 0, 0, &sub_nodeid,
                         &st),
              0);
    DEFER(fs_->forget(sub_nodeid, 1));
    // Drop the inode created by mkdir so the lookup has to go to OSS.
    fs_->forget(sub_nodeid, 1);
    ASSERT_EQ(fs_->lookup(dir_nodeid, "later_dir", &nodeid, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    if (FLAGS_pvfs_test_table2.empty()) {
      GTEST_SKIP() << "--pvfs_test_table2 not set, cross-table part skipped";
    }
    uint64_t tbl2_nodeid = 0;
    ASSERT_EQ(fs_->lookup(db_nodeid, FLAGS_pvfs_test_table2, &tbl2_nodeid, &st),
              0);
    DEFER(fs_->forget(tbl2_nodeid, 1));
    std::string shared = "pvfs_neg_" + test_subdir_;
    // Missing in table 1 and cached there; present in table 2 must still
    // resolve.
    ASSERT_EQ(fs_->lookup(tbl_nodeid, shared, &nodeid, &st), -ENOENT);
    uint64_t t2_file = 0;
    ASSERT_EQ(create_and_write(tbl2_nodeid, shared, data.data(), data.size(),
                               t2_file),
              (ssize_t)data.size());
    DEFER({
      fs_->unlink(tbl2_nodeid, shared);
      fs_->forget(t2_file, 1);
    });
    fs_->forget(t2_file, 1);  // force the lookup to go remote
    ASSERT_EQ(fs_->lookup(tbl2_nodeid, shared, &nodeid, &st), 0);
    ASSERT_EQ(fs_->lookup(tbl_nodeid, shared, &nodeid, &st), -ENOENT);
  }

  // The kernel may keep its page cache only while the object is provably
  // the one it cached: a same-size rewrite by another client must be seen
  // once the attributes are refreshed.
  void verify_keep_page_cache() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions&,
                     OssFileSystem::OssFsOptions& fo) { fo.attr_timeout = 1; };
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st),
              0);
    DEFER({
      fs_->unlink(dir_nodeid, "cache.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });
    std::string v1(100, 'a'), v2(100, 'b');
    uint64_t nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "cache.txt", v1.data(), v1.size(),
                               nodeid),
              (ssize_t)v1.size());
    DEFER(fs_->forget(nodeid, 1));
    auto open_ro = [&](bool* keep) {
      void* fh = nullptr;
      int r = fs_->open(nodeid, O_RDONLY, &fh, keep);
      if (r == 0) fs_->release(nodeid, fh);
      return r;
    };
    bool keep = false;
    ASSERT_EQ(open_ro(&keep), 0);
    ASSERT_TRUE(keep);  // the ETag came back with the upload
    ASSERT_EQ(open_ro(&keep), 0);
    ASSERT_TRUE(keep);

    // Same-size rewrite through another mount: the ETag differs.
    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    {
      FsSwap swap(*this, other->fs);
      uint64_t of = 0;
      ASSERT_EQ(lookup_path("/" + FLAGS_pvfs_test_database + "/" +
                                FLAGS_pvfs_test_table + "/" + dirname +
                                "/cache.txt",
                            of, st),
                0);
      void* wfh = nullptr;
      bool wkeep = false;
      ASSERT_EQ(fs_->open(of, O_WRONLY | O_TRUNC, &wfh, &wkeep), 0);
      auto* handle = reinterpret_cast<IFileHandleFuseLL*>(wfh);
      ASSERT_EQ(handle->pwrite(v2.data(), v2.size(), 0), (ssize_t)v2.size());
      ASSERT_EQ(fs_->release(of, wfh), 0);
      fs_->forget(of, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    ASSERT_EQ(fs_->getattr(nodeid, &st), 0);  // the kernel revalidates
    ASSERT_EQ(open_ro(&keep), 0);
    ASSERT_FALSE(keep);
    std::string content;
    ASSERT_EQ(read_file(nodeid, content), (ssize_t)v2.size());
    ASSERT_EQ(content, v2);
    ASSERT_EQ(open_ro(&keep), 0);
    ASSERT_TRUE(keep);
  }

  // Renaming a directory must move the cached paths of everything under
  // it, so an already known child stays readable by nodeid and by name.
  void verify_rename_dir_descendants() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string d1 = test_subdir_ + "_d1", d2 = test_subdir_ + "_d2";
    uint64_t d1_nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, d1, 0755, 0, 0, 0, &d1_nodeid, &st), 0);
    uint64_t f_nodeid = 0;
    std::string data = "moved along";
    ASSERT_EQ(create_and_write(d1_nodeid, "f", data.data(), data.size(),
                               f_nodeid),
              (ssize_t)data.size());
    DEFER({
      fs_->unlink(d1_nodeid, "f");
      fs_->rmdir(tbl_nodeid, d2);
      fs_->rmdir(tbl_nodeid, d1);
      fs_->forget(f_nodeid, 1);
      fs_->forget(d1_nodeid, 1);
    });

    ASSERT_EQ(fs_->rename(tbl_nodeid, d1, tbl_nodeid, d2, 0), 0);
    // Through the nodeid the kernel still holds.
    std::string content;
    ASSERT_EQ(read_file(f_nodeid, content), (ssize_t)data.size());
    ASSERT_EQ(content, data);
    // Through a fresh lookup of the new path.
    uint64_t d2_nodeid = 0, f2_nodeid = 0;
    ASSERT_EQ(fs_->lookup(tbl_nodeid, d2, &d2_nodeid, &st), 0);
    ASSERT_EQ(d2_nodeid, d1_nodeid);
    ASSERT_EQ(fs_->lookup(d2_nodeid, "f", &f2_nodeid, &st), 0);
    ASSERT_EQ(f2_nodeid, f_nodeid);
    fs_->forget(d2_nodeid, 1);
    fs_->forget(f2_nodeid, 1);
    ASSERT_EQ(fs_->lookup(tbl_nodeid, d1, &d2_nodeid, &st), -ENOENT);
  }

  // Under O_APPEND the kernel's offset comes from its cached i_size; the
  // write must land at the real end even when that offset is stale.
  void verify_append_ignores_offset() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st),
              0);
    DEFER({
      fs_->unlink(dir_nodeid, "app.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });
    std::string line1 = "line1\n", line2 = "line2\n";
    uint64_t nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "app.txt", line1.data(),
                               line1.size(), nodeid),
              (ssize_t)line1.size());
    DEFER(fs_->forget(nodeid, 1));
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_WRONLY | O_APPEND, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ASSERT_EQ(handle->pwrite(line2.data(), line2.size(), 0),  // stale offset
              (ssize_t)line2.size());
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
    std::string content;
    ASSERT_EQ(read_file(nodeid, content), (ssize_t)(line1.size() + line2.size()));
    ASSERT_EQ(content, line1 + line2);
  }

  // A read-only open right after a lookup must not issue another HEAD,
  // unless close_to_open asks for one.
  void check_open_head(bool close_to_open) {
    INIT_PHOTON();
    if (close_to_open) {
      tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions&,
                       OssFileSystem::OssFsOptions& fo) {
        fo.close_to_open = true;
      };
    }
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st),
              0);
    DEFER({
      fs_->unlink(dir_nodeid, "head.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });
    uint64_t nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "head.txt", "abc", 3, nodeid), 3);
    fs_->forget(nodeid, 1);  // drop the inode: the next lookup goes remote
    ASSERT_EQ(fs_->lookup(dir_nodeid, "head.txt", &nodeid, &st), 0);
    DEFER(fs_->forget(nodeid, 1));
    uint64_t heads = runtime()->oss_stat_calls();
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_RDONLY, &fh, &keep), 0);
    ASSERT_EQ(runtime()->oss_stat_calls(), heads + (close_to_open ? 1 : 0));
    ASSERT_TRUE(keep);
    fs_->release(nodeid, fh);
  }
  void verify_open_skips_head_when_fresh() { check_open_head(false); }
  void verify_close_to_open_option() { check_open_head(true); }

  // A 0-byte object must answer a real pread with 0, not an error from a
  // ranged GET that OSS rejects.
  void verify_read_empty_object() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st),
              0);
    DEFER({
      fs_->unlink(dir_nodeid, "empty.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });
    uint64_t nodeid = 0;
    ASSERT_EQ(create_and_write(dir_nodeid, "empty.txt", "", 0, nodeid), 0);
    DEFER(fs_->forget(nodeid, 1));
    fs_->forget(nodeid, 1);  // the next lookup sees the object itself
    ASSERT_EQ(fs_->lookup(dir_nodeid, "empty.txt", &nodeid, &st), 0);
    ASSERT_EQ(st.st_size, 0);
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_RDONLY, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    char buf[4096];
    ASSERT_EQ(handle->pread(buf, sizeof(buf), 0), 0);
    ASSERT_EQ(handle->pread(buf, sizeof(buf), 4096), 0);
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
  }

  // stat fields the kernel relies on.
  void verify_stat_fields() {
    INIT_PHOTON();
    init();
    struct stat st;
    ASSERT_EQ(fs_->getattr(root_nodeid_, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(st.st_mode & 0777, 0755u);
    ASSERT_EQ(st.st_ino, root_nodeid_);

    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));
    ASSERT_EQ(fs_->getattr(db_nodeid, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(st.st_ino, db_nodeid);

    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    ASSERT_EQ(fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st),
              0);
    DEFER({
      fs_->unlink(dir_nodeid, "f.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });
    ASSERT_EQ(st.st_ino, dir_nodeid);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    uint64_t nodeid = 0;
    std::string data(17, 'z');
    ASSERT_EQ(create_and_write(dir_nodeid, "f.txt", data.data(), data.size(),
                               nodeid),
              (ssize_t)data.size());
    DEFER(fs_->forget(nodeid, 1));
    ASSERT_EQ(fs_->getattr(nodeid, &st), 0);
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_EQ(st.st_mode & 0777, 0644u);
    ASSERT_EQ(st.st_ino, nodeid);
    ASSERT_EQ((size_t)st.st_size, data.size());
    ASSERT_GT(st.st_mtime, 0);

    struct statvfs sv;
    ASSERT_EQ(fs_->statfs(&sv), 0);
    ASSERT_GT(sv.f_bsize, 0u);
    ASSERT_EQ(sv.f_frsize, sv.f_bsize);
    ASSERT_GT(sv.f_blocks, 0u);
    ASSERT_GT(sv.f_bavail, 0u);
    ASSERT_GT(sv.f_ffree, 0u);
    ASSERT_EQ(sv.f_namemax, 255u);
  }

  void verify_unlink() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    // Create test dir + file
    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    std::string data = "delete me";
    uint64_t file_nodeid = 0;
    ssize_t written = create_and_write(dir_nodeid, "to_delete.txt", data.data(),
                                       data.size(), file_nodeid);
    ASSERT_GT(written, 0);

    // Delete it (the kernel holds the looked-up child while it unlinks)
    r = fs_->unlink(dir_nodeid, "to_delete.txt");
    ASSERT_EQ(r, 0);
    fs_->forget(file_nodeid, 1);

    // Verify it's gone
    uint64_t lookup_nodeid = 0;
    r = fs_->lookup(dir_nodeid, "to_delete.txt", &lookup_nodeid, &st);
    ASSERT_EQ(r, -ENOENT);
  }

  void verify_rename() {
    INIT_PHOTON();
    init();
    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    ASSERT_EQ(find_table(db_nodeid, tbl_nodeid), 0);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    // Create test dir + file
    std::string dirname = test_subdir_;
    uint64_t dir_nodeid = 0;
    struct stat st;
    int r = fs_->mkdir(tbl_nodeid, dirname, 0755, 0, 0, 0, &dir_nodeid, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->unlink(dir_nodeid, "renamed.txt");
      fs_->rmdir(tbl_nodeid, dirname);
      fs_->forget(dir_nodeid, 1);
    });

    std::string data = "rename me";
    uint64_t file_nodeid = 0;
    ssize_t written = create_and_write(dir_nodeid, "original.txt", data.data(),
                                       data.size(), file_nodeid);
    ASSERT_GT(written, 0);
    DEFER(fs_->forget(file_nodeid, 1));

    // Rename within same directory
    r = fs_->rename(dir_nodeid, "original.txt", dir_nodeid, "renamed.txt", 0);
    ASSERT_EQ(r, 0);

    // Old name should be gone
    uint64_t lookup_nodeid = 0;
    r = fs_->lookup(dir_nodeid, "original.txt", &lookup_nodeid, &st);
    ASSERT_EQ(r, -ENOENT);

    // New name should exist
    r = fs_->lookup(dir_nodeid, "renamed.txt", &lookup_nodeid, &st);
    ASSERT_EQ(r, 0);
    ASSERT_TRUE(S_ISREG(st.st_mode));
    fs_->forget(lookup_nodeid, 1);
  }
};

TEST_F(PvfsFileOpsTest, verify_readdir_table) { verify_readdir_table(); }
TEST_F(PvfsFileOpsTest, verify_mkdir_rmdir) { verify_mkdir_rmdir(); }
TEST_F(PvfsFileOpsTest, verify_create_write_read) {
  verify_create_write_read();
}
TEST_F(PvfsFileOpsTest, verify_flush_between_writes) {
  verify_flush_between_writes();
}
TEST_F(PvfsFileOpsTest, verify_append_after_flush) {
  verify_append_after_flush();
}
TEST_F(PvfsFileOpsTest, verify_write_buf_pipe) { verify_write_buf_pipe(); }
TEST_F(PvfsFileOpsTest, verify_creat_after_virtual_refusal) {
  verify_creat_after_virtual_refusal();
}
TEST_F(PvfsFileOpsTest, verify_negative_cache_scoping) {
  verify_negative_cache_scoping();
}
TEST_F(PvfsFileOpsTest, verify_keep_page_cache) { verify_keep_page_cache(); }
TEST_F(PvfsFileOpsTest, verify_rename_dir_descendants) {
  verify_rename_dir_descendants();
}
TEST_F(PvfsFileOpsTest, verify_append_ignores_offset) {
  verify_append_ignores_offset();
}
TEST_F(PvfsFileOpsTest, verify_open_skips_head_when_fresh) {
  verify_open_skips_head_when_fresh();
}
TEST_F(PvfsFileOpsTest, verify_close_to_open_option) {
  verify_close_to_open_option();
}
TEST_F(PvfsFileOpsTest, verify_read_empty_object) { verify_read_empty_object(); }
TEST_F(PvfsFileOpsTest, verify_stat_fields) { verify_stat_fields(); }
TEST_F(PvfsFileOpsTest, verify_unlink) { verify_unlink(); }
TEST_F(PvfsFileOpsTest, verify_rename) { verify_rename(); }
