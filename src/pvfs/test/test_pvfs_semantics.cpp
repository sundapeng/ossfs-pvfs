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
#include <string.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>

#include "common/fuse.h"
#include "common/logger.h"
#include "pvfs_test_suite.h"

// POSIX semantics that need more than one client, or a second look at the
// object after the operation: create flags, rename edge cases, eviction.
class PvfsSemanticsTest : public PvfsTestSuite {
 protected:
  static constexpr int64_t kOssBoundMs = 5000;
  static constexpr size_t kPart = 256 * 1024;

  struct Scratch {
    uint64_t db = 0, tbl = 0, dir = 0;
    std::vector<std::pair<uint64_t, std::string>> files;
  };
  void setup_dir(Scratch& sc) {
    ASSERT_EQ(find_table(sc.db, sc.tbl), 0);
    struct stat st;
    ASSERT_EQ(fs_->mkdir(sc.tbl, test_subdir_, 0755, 0, 0, 0, &sc.dir, &st), 0);
  }
  void cleanup_dir(Scratch& sc) {
    for (auto& f : sc.files) {
      fs_->unlink(sc.dir, f.second);
      fs_->forget(f.first, 1);
    }
    remove_tree(sc.tbl, test_subdir_);
    fs_->forget(sc.dir, 1);
    fs_->forget(sc.tbl, 1);
    fs_->forget(sc.db, 1);
  }

  std::string table_path() {
    return "/" + FLAGS_pvfs_test_database + "/" + FLAGS_pvfs_test_table;
  }
  std::string subdir_path() { return table_path() + "/" + test_subdir_; }

  // Content of `name` under `dir` through a fresh read-only open.
  std::string read_back(uint64_t dir, const std::string& name) {
    uint64_t nodeid = 0;
    struct stat st;
    if (fs_->lookup(dir, name, &nodeid, &st) != 0) return "<lookup failed>";
    DEFER(fs_->forget(nodeid, 1));
    std::string content;
    if (read_file(nodeid, content) < 0) return "<read failed>";
    return content;
  }

  static std::string make_data(size_t n, uint32_t seed) {
    std::string d(n, '\0');
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      d[i] = static_cast<char>(x);
    }
    return d;
  }

  // Open `nodeid` for writing on `fs` and return the handle.
  static void* open_writer(IFileSystemFuseLL& fs, uint64_t nodeid) {
    void* fh = nullptr;
    bool keep = false;
    if (fs.open(nodeid, O_WRONLY, &fh, &keep) != 0) return nullptr;
    return fh;
  }
  uint64_t child_on(Instance& other, const std::string& name) {
    FsSwap swap(*this, other.fs);
    uint64_t nodeid = 0;
    struct stat st;
    if (lookup_path(subdir_path() + "/" + name, nodeid, st) != 0) return 0;
    return nodeid;
  }

  // Another client created the name after this one looked it up: creat
  // sees it exist (the kernel then opens instead), O_TRUNC through open
  // empties it.
  void verify_create_honours_excl_and_trunc() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t nodeid = 0;
    struct stat st;
    EXPECT_EQ(fs_->lookup(sc.dir, "f", &nodeid, &st), -ENOENT);

    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    {
      FsSwap swap(*this, other->fs);
      uint64_t odir = 0, of = 0;
      ASSERT_EQ(lookup_path(subdir_path(), odir, st), 0);
      ASSERT_EQ(create_and_write(odir, "f", "abcd", 4, of), 4);
      fs_->forget(of, 1);
      fs_->forget(odir, 1);
    }

    void* fh = nullptr;
    auto t0 = clock_now();
    EXPECT_EQ(fs_->creat(sc.dir, "f", O_CREAT | O_EXCL | O_WRONLY, 0644, 0, 0,
                         0, &nodeid, &st, &fh),
              -EEXIST);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    EXPECT_EQ(read_back(sc.dir, "f"), "abcd");
    EXPECT_EQ(fs_->creat(sc.dir, "f", O_CREAT | O_WRONLY, 0644, 0, 0, 0,
                         &nodeid, &st, &fh),
              -EEXIST);
    EXPECT_EQ(read_back(sc.dir, "f"), "abcd");

    ASSERT_EQ(fs_->lookup(sc.dir, "f", &nodeid, &st), 0);
    sc.files.emplace_back(nodeid, "f");
    EXPECT_EQ(st.st_size, 4);
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_WRONLY | O_TRUNC, &fh, &keep), 0);
    EXPECT_EQ(fs_->release(nodeid, fh), 0);
    EXPECT_EQ(read_back(sc.dir, "f"), "");
  }

  // Over the limit nothing is written: no destination marker, sources intact.
  void verify_rename_dir_limit_before_marker() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions&,
                     OssFileSystem::OssFsOptions& fo) {
      fo.rename_dir_limit = 2;
    };
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    for (const char* n : {"a", "b", "c"}) {
      uint64_t f = 0;
      ASSERT_EQ(create_and_write(sc.dir, n, n, 1, f), 1);
      sc.files.emplace_back(f, n);
    }
    std::string dst = test_subdir_ + "_moved";
    auto t0 = clock_now();
    EXPECT_EQ(fs_->rename(sc.tbl, test_subdir_, sc.tbl, dst, 0), -E2BIG);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    uint64_t nodeid = 0;
    struct stat st;
    EXPECT_EQ(fs_->lookup(sc.tbl, dst, &nodeid, &st), -ENOENT);
    std::vector<DirEntry> entries;
    ASSERT_EQ(list_dir(sc.dir, entries), 0);
    EXPECT_EQ(entries.size(), 3u);
    for (auto& e : entries) fs_->forget(e.nodeid, 1);
    EXPECT_EQ(read_back(sc.dir, "c"), "c");
  }

  // Two clients append to the same object: whoever flushes last defines
  // the content, since each handle rewrites the whole object.
  void verify_two_writers_last_flush_wins() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t f = 0;
    std::string base(100, 'b');
    ASSERT_EQ(create_and_write(sc.dir, "w", base.data(), base.size(), f), 100);
    sc.files.emplace_back(f, "w");

    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    uint64_t of = child_on(*other, "w");
    ASSERT_NE(of, 0u);
    DEFER(other->fs->forget(of, 1));
    void* fa = open_writer(*fs_, f);
    void* fb = open_writer(*other->fs, of);
    ASSERT_NE(fa, nullptr);
    ASSERT_NE(fb, nullptr);
    auto* ha = reinterpret_cast<IFileHandleFuseLL*>(fa);
    auto* hb = reinterpret_cast<IFileHandleFuseLL*>(fb);
    EXPECT_EQ(ha->pwrite("AAAA", 4, 100), 4);
    EXPECT_EQ(hb->pwrite("BBBBBBBB", 8, 100), 8);
    EXPECT_EQ(fs_->flush(f, fa), 0);
    EXPECT_EQ(read_back(sc.dir, "w"), base + "AAAA");
    EXPECT_EQ(other->fs->flush(of, fb), 0);
    EXPECT_EQ(fs_->release(f, fa), 0);
    EXPECT_EQ(other->fs->release(of, fb), 0);
    {
      FsSwap swap(*this, other->fs);
      std::string content;
      ASSERT_EQ(read_file(of, content), 108);
      EXPECT_EQ(content, base + "BBBBBBBB");
    }
  }

  // A reader opened before an append keeps its size bound; a fresh open
  // after the writer's flush sees everything.
  void verify_reader_bound_vs_fresh_open() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t f = 0;
    std::string base(100, 'r');
    ASSERT_EQ(create_and_write(sc.dir, "r", base.data(), base.size(), f), 100);
    sc.files.emplace_back(f, "r");
    void* fr = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(f, O_RDONLY, &fr, &keep), 0);

    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    uint64_t of = child_on(*other, "r");
    ASSERT_NE(of, 0u);
    DEFER(other->fs->forget(of, 1));
    void* fw = open_writer(*other->fs, of);
    ASSERT_NE(fw, nullptr);
    std::string more(50, 'm');
    EXPECT_EQ(reinterpret_cast<IFileHandleFuseLL*>(fw)->pwrite(more.data(), 50,
                                                               100),
              50);
    EXPECT_EQ(other->fs->flush(of, fw), 0);
    EXPECT_EQ(other->fs->release(of, fw), 0);

    std::string buf(1000, '\0');
    auto* hr = reinterpret_cast<IFileHandleFuseLL*>(fr);
    EXPECT_EQ(hr->pread(buf.data(), 100, 0), 100);
    EXPECT_EQ(fs_->release(f, fr), 0);
    {
      FsSwap swap(*this, other->fs);
      std::string content;
      ASSERT_EQ(read_file(of, content), 150);
      EXPECT_EQ(content, base + more);
    }
  }

  // The store behind an open handle outlives its table cache entry.
  void verify_handle_survives_table_eviction() {
    INIT_PHOTON();
    max_table_cache_ = 1;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t f = 0;
    struct stat st;
    void* fh = nullptr;
    ASSERT_EQ(fs_->creat(sc.dir, "e", O_CREAT | O_WRONLY, 0644, 0, 0, 0, &f,
                         &st, &fh),
              0);
    sc.files.emplace_back(f, "e");
    auto* h = reinterpret_cast<IFileHandleFuseLL*>(fh);
    EXPECT_EQ(h->pwrite("0123456789", 10, 0), 10);
    for (int i = 0; i < 3; i++) {
      int err = 0;
      EXPECT_EQ(cache()->resolve(FLAGS_pvfs_test_database,
                                 "no_such_table_" + std::to_string(i), &err),
                nullptr);
    }
    EXPECT_EQ(fs_->flush(f, fh), 0);
    EXPECT_EQ(fs_->release(f, fh), 0);
    EXPECT_EQ(read_back(sc.dir, "e"), "0123456789");
  }

  // rename onto an existing file replaces it; a missing source is ENOENT.
  void verify_rename_file_cases() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t s = 0, d = 0;
    ASSERT_EQ(create_and_write(sc.dir, "src", "S", 1, s), 1);
    ASSERT_EQ(create_and_write(sc.dir, "dst", "D", 1, d), 1);
    sc.files.emplace_back(s, "src");
    sc.files.emplace_back(d, "dst");
    EXPECT_EQ(fs_->rename(sc.dir, "src", sc.dir, "dst", 0), 0);
    EXPECT_EQ(read_back(sc.dir, "dst"), "S");
    // A missing source stops at the lookup the kernel issues before RENAME
    // (OssFs itself answers ESTALE to a rename of a name it never saw).
    uint64_t nodeid = 0;
    struct stat st;
    auto t0 = clock_now();
    EXPECT_EQ(fs_->lookup(sc.dir, "missing", &nodeid, &st), -ENOENT);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    // What is on OSS, seen from another mount (this one answers ESTALE for
    // entries it keeps until the kernel forgets them, as upstream does).
    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    FsSwap swap(*this, other->fs);
    EXPECT_EQ(lookup_path(subdir_path() + "/src", nodeid, st), -ENOENT);
    EXPECT_EQ(lookup_path(subdir_path() + "/dst", nodeid, st), 0);
    fs_->forget(nodeid, 1);
  }

  // A writable open of exactly one part is prefilled, one byte more is
  // resumed through a HEAD; both take a one-byte append.
  void verify_writable_open_at_part_boundaries() {
    INIT_PHOTON();
    upload_part_size_ = kPart;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    const std::pair<const char*, size_t> cases[] = {{"exact", kPart},
                                                    {"plus1", kPart + 1}};
    int n = 0;
    for (const auto& [name, size] : cases) {
      std::string data = make_data(size, 40 + n++);
      uint64_t f = 0;
      ASSERT_EQ(create_and_write(sc.dir, name, data.data(), data.size(), f),
                static_cast<ssize_t>(size));
      sc.files.emplace_back(f, name);
      void* fh = open_writer(*fs_, f);
      ASSERT_NE(fh, nullptr) << name;
      auto* h = reinterpret_cast<IFileHandleFuseLL*>(fh);
      EXPECT_EQ(h->pwrite("Z", 1, size), 1) << name;
      EXPECT_EQ(fs_->release(f, fh), 0) << name;
      std::string content;
      ASSERT_EQ(read_file(f, content), static_cast<ssize_t>(size + 1)) << name;
      EXPECT_EQ(content, data + "Z") << name;
    }
  }

  // Once the attr TTL is over, getattr asks OSS again: a file removed by
  // another client is ENOENT, and the inode stays usable for a new object.
  void verify_getattr_ttl_expiry() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions&,
                     OssFileSystem::OssFsOptions& fo) { fo.attr_timeout = 1; };
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t f = 0;
    ASSERT_EQ(create_and_write(sc.dir, "ttl", "old", 3, f), 3);
    sc.files.emplace_back(f, "ttl");

    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    uint64_t odir = 0;
    struct stat st;
    {
      FsSwap swap(*this, other->fs);
      ASSERT_EQ(lookup_path(subdir_path(), odir, st), 0);
      uint64_t ottl = 0;
      ASSERT_EQ(fs_->lookup(odir, "ttl", &ottl, &st), 0);
      ASSERT_EQ(fs_->unlink(odir, "ttl"), 0);
      fs_->forget(ottl, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto t0 = clock_now();
    EXPECT_EQ(fs_->getattr(f, &st), -ENOENT);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    {
      FsSwap swap(*this, other->fs);
      uint64_t of = 0;
      ASSERT_EQ(create_and_write(odir, "ttl", "newer", 5, of), 5);
      fs_->forget(of, 1);
      fs_->forget(odir, 1);
    }
    uint64_t again = 0;
    EXPECT_EQ(fs_->lookup(sc.dir, "ttl", &again, &st), 0);
    EXPECT_EQ(st.st_size, 5);
    fs_->forget(again, 1);
  }

  // An empty directory lists nothing; a directory beyond one OSS listing
  // page lists every name exactly once.
  void verify_readdir_empty_and_large() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    std::vector<DirEntry> entries;
    ASSERT_EQ(list_dir(sc.dir, entries), 0);
    EXPECT_EQ(entries.size(), 0u);
    if (FLAGS_pvfs_test_fast) GTEST_SKIP() << "--pvfs_test_fast: 1200 files";

    const int kFiles = 1200, kThreads = 8;
    std::atomic<int> failures{0};
    std::vector<uint64_t> ids(kFiles, 0);
    auto in_parallel = [&](const std::function<int(int)>& op) {
      std::vector<std::thread> workers;
      for (int t = 0; t < kThreads; t++) {
        workers.emplace_back([&, t]() {
          INIT_PHOTON();
          for (int i = t; i < kFiles; i += kThreads) {
            if (op(i) != 0) failures.fetch_add(1);
          }
        });
      }
      for (auto& w : workers) w.join();
    };
    in_parallel([&](int i) {
      int r = create_and_write(sc.dir, "n" + std::to_string(i), "1", 1, ids[i]);
      return r == 1 ? 0 : r;
    });
    ASSERT_EQ(failures.load(), 0);
    ASSERT_EQ(list_dir(sc.dir, entries), 0);
    std::set<std::string> names;
    for (auto& e : entries) {
      names.insert(e.name);
      fs_->forget(e.nodeid, 1);
    }
    EXPECT_EQ(entries.size(), static_cast<size_t>(kFiles));
    EXPECT_EQ(names.size(), static_cast<size_t>(kFiles));
    in_parallel([&](int i) {
      int r = fs_->unlink(sc.dir, "n" + std::to_string(i));
      fs_->forget(ids[i], 1);
      return r;
    });
    EXPECT_EQ(failures.load(), 0);
  }

  // The default options make a read-only mount: creates fail at once,
  // every other mutation is EROFS from the store, reads work.
  void verify_read_only_by_default() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions& ro,
                     OssFileSystem::OssFsOptions&) {
      ro.readonly = PvfsFileSystem::PvfsRuntimeOptions().readonly;
      ASSERT_TRUE(ro.readonly);
    };
    init();
    uint64_t db = 0, tbl = 0;
    ASSERT_EQ(find_table(db, tbl), 0);
    DEFER(fs_->forget(db, 1));
    DEFER(fs_->forget(tbl, 1));

    // A file another (writable) client made is readable, not writable.
    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    uint64_t odir = 0, of = 0, otbl = 0;
    struct stat st;
    {
      FsSwap swap(*this, other->fs);
      ASSERT_EQ(lookup_path(table_path(), otbl, st), 0);
      ASSERT_EQ(fs_->mkdir(otbl, test_subdir_, 0755, 0, 0, 0, &odir, &st), 0);
      ASSERT_EQ(create_and_write(odir, "f", "ro", 2, of), 2);
    }
    DEFER({
      FsSwap swap(*this, other->fs);
      fs_->unlink(odir, "f");
      fs_->forget(of, 1);
      fs_->rmdir(otbl, test_subdir_);
      fs_->forget(odir, 1);
      fs_->forget(otbl, 1);
    });

    uint64_t dir = 0, f = 0, nodeid = 0;
    ASSERT_EQ(fs_->lookup(tbl, test_subdir_, &dir, &st), 0);
    DEFER(fs_->forget(dir, 1));
    ASSERT_EQ(fs_->lookup(dir, "f", &f, &st), 0);
    DEFER(fs_->forget(f, 1));
    EXPECT_EQ(read_back(dir, "f"), "ro");
    uint64_t heads = runtime()->oss_stat_calls();
    void* fh = nullptr;
    EXPECT_EQ(fs_->creat(dir, "ro_f", O_CREAT | O_WRONLY, 0644, 0, 0, 0,
                         &nodeid, &st, &fh),
              -EROFS);
    EXPECT_EQ(fs_->mkdir(dir, "ro_d", 0755, 0, 0, 0, &nodeid, &st), -EROFS);
    EXPECT_EQ(runtime()->oss_stat_calls(), heads);  // refused before a probe
    EXPECT_EQ(fs_->unlink(dir, "f"), -EROFS);
    EXPECT_EQ(fs_->rename(dir, "f", dir, "g", 0), -EROFS);
    EXPECT_EQ(fs_->rmdir(tbl, test_subdir_), -ENOTEMPTY);
    memset(&st, 0, sizeof(st));
    EXPECT_EQ(fs_->setattr(f, &st, FUSE_SET_ATTR_SIZE), -EROFS);
    // A write is buffered and refused when it reaches the store.
    bool keep = false;
    ASSERT_EQ(fs_->open(f, O_WRONLY, &fh, &keep), 0);
    auto* h = reinterpret_cast<IFileHandleFuseLL*>(fh);
    EXPECT_EQ(h->pwrite("!", 1, 2), 1);
    EXPECT_EQ(fs_->flush(f, fh), -EROFS);
    EXPECT_LT(fs_->release(f, fh), 0);
    EXPECT_EQ(read_back(dir, "f"), "ro");
    EXPECT_EQ(rest_stats().get_table_token.load(), 1u);
  }

  // Node of the table's snapshot directory on this instance, made by
  // `meta` (an instance allowed to write metadata) when the table has none
  // yet. The probe runs on `meta` so this instance never caches the
  // name's absence first.
  uint64_t snapshot_dir(uint64_t tbl, Instance& meta, bool* created) {
    *created = false;
    struct stat st;
    uint64_t mtbl = 0, mnode = 0;
    {
      FsSwap swap(*this, meta.fs);
      if (lookup_path(table_path(), mtbl, st) != 0) return 0;
      DEFER(fs_->forget(mtbl, 1));
      if (fs_->lookup(mtbl, "snapshot", &mnode, &st) != 0) {
        if (fs_->mkdir(mtbl, "snapshot", 0755, 0, 0, 0, &mnode, &st) != 0) {
          return 0;
        }
        *created = true;
      }
      fs_->forget(mnode, 1);
    }
    uint64_t nodeid = 0;
    return fs_->lookup(tbl, "snapshot", &nodeid, &st) == 0 ? nodeid : 0;
  }
  void drop_snapshot_dir(Instance& meta) {
    struct stat st;
    uint64_t mtbl = 0;
    FsSwap swap(*this, meta.fs);
    if (lookup_path(table_path(), mtbl, st) == 0) {
      fs_->rmdir(mtbl, "snapshot");
      fs_->forget(mtbl, 1);
    }
  }

  // Writable mount: mutations under Paimon's directories are EPERM, the
  // creates without a probe, once-logged per name; ordinary directories
  // work.
  void verify_reserved_names_guarded() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    auto meta_opts = base_opts();
    meta_opts.allow_metadata_write = true;
    auto meta = make_instance(meta_opts);
    ASSERT_NE(meta, nullptr);
    bool created = false;
    uint64_t snap = snapshot_dir(sc.tbl, *meta, &created);
    ASSERT_NE(snap, 0u);
    DEFER(fs_->forget(snap, 1));
    DEFER(if (created) drop_snapshot_dir(*meta));

    uint64_t nodeid = 0;
    struct stat st;
    void* fh = nullptr;
    uint64_t heads = runtime()->oss_stat_calls();
    auto t0 = clock_now();
    EXPECT_EQ(fs_->creat(snap, "x", O_CREAT | O_WRONLY, 0644, 0, 0, 0, &nodeid,
                         &st, &fh),
              -EPERM);
    EXPECT_EQ(fs_->creat(snap, "y", O_CREAT | O_WRONLY, 0644, 0, 0, 0, &nodeid,
                         &st, &fh),
              -EPERM);
    EXPECT_EQ(fs_->mkdir(snap, "d", 0755, 0, 0, 0, &nodeid, &st), -EPERM);
    EXPECT_EQ(fs_->mkdir(sc.tbl, "manifest", 0755, 0, 0, 0, &nodeid, &st),
              -EPERM);
    EXPECT_EQ(fs_->mkdir(sc.tbl, "bucket-0", 0755, 0, 0, 0, &nodeid, &st),
              -EPERM);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    EXPECT_EQ(runtime()->oss_stat_calls(), heads);
    EXPECT_EQ(runtime()->reserved_write_refusals(), 3u);  // snapshot, manifest, bucket-0
    EXPECT_EQ(fs_->rename(sc.tbl, "snapshot", sc.tbl, "snap2", 0), -EPERM);
    int r = fs_->rmdir(sc.tbl, "snapshot");
    EXPECT_TRUE(r == -EPERM || r == -ENOTEMPTY) << r;

    uint64_t f = 0;
    ASSERT_EQ(create_and_write(sc.dir, "x", "1", 1, f), 1);
    sc.files.emplace_back(f, "x");
    EXPECT_EQ(fs_->rename(sc.dir, "x", sc.tbl, "manifest", 0), -EPERM);
    EXPECT_EQ(fs_->rename(sc.dir, "x", snap, "x", 0), -EPERM);
    EXPECT_EQ(read_back(sc.dir, "x"), "1");
    ASSERT_EQ(fs_->mkdir(sc.tbl, "bucketx_" + test_subdir_, 0755, 0, 0, 0,
                         &nodeid, &st),
              0);
    EXPECT_EQ(fs_->rmdir(sc.tbl, "bucketx_" + test_subdir_), 0);
    fs_->forget(nodeid, 1);
    EXPECT_EQ(runtime()->reserved_write_refusals(), 3u);
  }

  // With pvfs_allow_metadata_write a file under snapshot/ can be made and
  // removed again.
  void verify_metadata_write_allowed() {
    INIT_PHOTON();
    tweak_opts_ = [](PvfsFileSystem::PvfsRuntimeOptions& ro,
                     OssFileSystem::OssFsOptions&) {
      ro.allow_metadata_write = true;
    };
    init();
    uint64_t db = 0, tbl = 0;
    ASSERT_EQ(find_table(db, tbl), 0);
    DEFER(fs_->forget(db, 1));
    DEFER(fs_->forget(tbl, 1));
    bool created = false;
    uint64_t snap = snapshot_dir(tbl, *inst_, &created);
    ASSERT_NE(snap, 0u);
    DEFER(fs_->forget(snap, 1));
    DEFER(if (created) fs_->rmdir(tbl, "snapshot"));
    uint64_t f = 0;
    ASSERT_EQ(create_and_write(snap, test_subdir_, "meta", 4, f), 4);
    EXPECT_EQ(read_back(snap, test_subdir_), "meta");
    EXPECT_EQ(fs_->unlink(snap, test_subdir_), 0);
    fs_->forget(f, 1);
    EXPECT_EQ(runtime()->reserved_write_refusals(), 0u);
  }
};

TEST_F(PvfsSemanticsTest, CreateHonoursExclAndTrunc) {
  verify_create_honours_excl_and_trunc();
}
TEST_F(PvfsSemanticsTest, RenameDirLimitBeforeMarker) {
  verify_rename_dir_limit_before_marker();
}
TEST_F(PvfsSemanticsTest, TwoWritersLastFlushWins) {
  verify_two_writers_last_flush_wins();
}
TEST_F(PvfsSemanticsTest, ReaderBoundVsFreshOpen) {
  verify_reader_bound_vs_fresh_open();
}
TEST_F(PvfsSemanticsTest, HandleSurvivesTableEviction) {
  verify_handle_survives_table_eviction();
}
TEST_F(PvfsSemanticsTest, RenameFileCases) { verify_rename_file_cases(); }
TEST_F(PvfsSemanticsTest, WritableOpenAtPartBoundaries) {
  verify_writable_open_at_part_boundaries();
}
TEST_F(PvfsSemanticsTest, GetattrTtlExpiry) { verify_getattr_ttl_expiry(); }
TEST_F(PvfsSemanticsTest, ReaddirEmptyAndLarge) {
  verify_readdir_empty_and_large();
}
TEST_F(PvfsSemanticsTest, ReadOnlyByDefault) { verify_read_only_by_default(); }
TEST_F(PvfsSemanticsTest, ReservedNamesGuarded) {
  verify_reserved_names_guarded();
}
TEST_F(PvfsSemanticsTest, MetadataWriteAllowed) {
  verify_metadata_write_allowed();
}
