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
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include "common/fuse.h"
#include "common/logger.h"
#include "pvfs_test_suite.h"

// Multi-part writes, FLUSH visibility and read-back with the READ patterns
// the kernel produces.
class PvfsLargeIoTest : public PvfsTestSuite {
 protected:
  static constexpr size_t kKiB = 1024;
  static constexpr size_t kMiB = 1024 * 1024;

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
  static size_t first_diff(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
      if (a[i] != b[i]) return i;
    }
    return a.size() == b.size() ? std::string::npos : n;
  }

  // Create `name` under `dir` and write `data` in `chunk`-sized pwrites.
  int write_file(uint64_t dir, const char* name, const std::string& data,
                 size_t chunk, uint64_t& nodeid, void** keep_fh = nullptr) {
    struct stat st;
    void* fh = nullptr;
    int r = fs_->creat(dir, name, O_CREAT | O_WRONLY, 0644, 0, 0, 0, &nodeid,
                       &st, &fh);
    if (r != 0) return r;
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    for (size_t off = 0; off < data.size(); off += chunk) {
      size_t n = std::min(chunk, data.size() - off);
      ssize_t w = handle->pwrite(data.data() + off, n, off);
      if (w != static_cast<ssize_t>(n)) return w < 0 ? w : -EIO;
    }
    if (keep_fh) {
      *keep_fh = fh;
      return 0;
    }
    return fs_->release(nodeid, fh);
  }

  // Sequential preads of `step` bytes over a fresh read-only handle.
  int read_sequential(uint64_t nodeid, size_t size, size_t step,
                      std::string& out) {
    void* fh = nullptr;
    bool keep = false;
    int r = fs_->open(nodeid, O_RDONLY, &fh, &keep);
    if (r != 0) return r;
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    out.assign(size, '\0');
    for (size_t off = 0; off < size; off += step) {
      size_t n = std::min(step, size - off);
      ssize_t rd = handle->pread(&out[off], n, off);
      if (rd != static_cast<ssize_t>(n)) {
        fs_->release(nodeid, fh);
        return rd < 0 ? rd : -EIO;
      }
    }
    return fs_->release(nodeid, fh);
  }

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
    fs_->rmdir(sc.tbl, test_subdir_);
    fs_->forget(sc.dir, 1);
    fs_->forget(sc.tbl, 1);
    fs_->forget(sc.db, 1);
  }

  // FLUSH on a multipart handle must leave the complete object visible to
  // a fresh open before RELEASE.
  void verify_multipart_flush_visible() {
    INIT_PHOTON();
    upload_part_size_ = 256 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string data = make_data(3 * 256 * kKiB + 100, 11);
    uint64_t nodeid = 0;
    void* wfh = nullptr;
    ASSERT_EQ(write_file(sc.dir, "mp_flush.bin", data, 64 * kKiB, nodeid, &wfh),
              0);
    sc.files.emplace_back(nodeid, "mp_flush.bin");
    ASSERT_EQ(fs_->flush(nodeid, wfh), 0);
    struct stat st;
    ASSERT_EQ(fs_->getattr(nodeid, &st), 0);
    ASSERT_EQ(static_cast<size_t>(st.st_size), data.size());
    std::string content;
    ASSERT_EQ(read_sequential(nodeid, data.size(), 128 * kKiB, content), 0);
    ASSERT_EQ(first_diff(content, data), std::string::npos);
    ASSERT_EQ(fs_->release(nodeid, wfh), 0);
    ASSERT_EQ(read_file(nodeid, content), static_cast<ssize_t>(data.size()));
    ASSERT_EQ(first_diff(content, data), std::string::npos);
  }

  // FLUSH, write more, FLUSH again: the object must always be the
  // concatenation.
  void verify_write_after_multipart_flush() {
    INIT_PHOTON();
    upload_part_size_ = 256 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string a = make_data(600 * kKiB, 21);
    std::string b = make_data(300 * kKiB, 22);
    std::string c = make_data(10, 23);
    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(write_file(sc.dir, "mp_resume.bin", a, 64 * kKiB, nodeid, &fh),
              0);
    sc.files.emplace_back(nodeid, "mp_resume.bin");
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ASSERT_EQ(fs_->flush(nodeid, fh), 0);
    ASSERT_EQ(handle->pwrite(b.data(), b.size(), a.size()),
              static_cast<ssize_t>(b.size()));
    ASSERT_EQ(fs_->flush(nodeid, fh), 0);
    ASSERT_EQ(handle->pwrite(c.data(), c.size(), a.size() + b.size()),
              static_cast<ssize_t>(c.size()));
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
    std::string content;
    std::string expected = a + b + c;
    ASSERT_EQ(read_file(nodeid, content),
              static_cast<ssize_t>(expected.size()));
    ASSERT_EQ(first_diff(content, expected), std::string::npos);
  }

  // O_APPEND on an object larger than one part.
  void verify_append_large_existing() {
    INIT_PHOTON();
    upload_part_size_ = 256 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string a = make_data(700 * kKiB, 31);
    std::string b = make_data(100 * kKiB, 32);
    uint64_t nodeid = 0;
    ASSERT_EQ(write_file(sc.dir, "append_large.bin", a, 64 * kKiB, nodeid), 0);
    sc.files.emplace_back(nodeid, "append_large.bin");
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_WRONLY | O_APPEND, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ASSERT_EQ(handle->pwrite(b.data(), b.size(), a.size()),
              static_cast<ssize_t>(b.size()));
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
    std::string content;
    std::string expected = a + b;
    ASSERT_EQ(read_file(nodeid, content),
              static_cast<ssize_t>(expected.size()));
    ASSERT_EQ(first_diff(content, expected), std::string::npos);
  }

  // A writable open of a non-empty object without O_TRUNC must append to
  // the existing content, for a single-object and for a multi-part file;
  // with O_TRUNC the object is emptied before the first write.
  void verify_write_open_appends() {
    INIT_PHOTON();
    upload_part_size_ = 256 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    struct Case {
      const char* name;
      size_t size;
    } cases[] = {{"wopen_small.bin", 100}, {"wopen_parts.bin", 3 * 256 * kKiB}};
    for (const auto& c : cases) {
      std::string a = make_data(c.size, 61);
      std::string b = make_data(10, 62);
      uint64_t nodeid = 0;
      ASSERT_EQ(write_file(sc.dir, c.name, a, 64 * kKiB, nodeid), 0) << c.name;
      sc.files.emplace_back(nodeid, c.name);
      void* fh = nullptr;
      bool keep = false;
      ASSERT_EQ(fs_->open(nodeid, O_WRONLY, &fh, &keep), 0) << c.name;
      auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
      ASSERT_EQ(handle->pwrite(b.data(), b.size(), a.size()),
                (ssize_t)b.size())
          << c.name;
      ASSERT_EQ(fs_->release(nodeid, fh), 0) << c.name;
      std::string content;
      std::string expected = a + b;
      ASSERT_EQ(read_file(nodeid, content), (ssize_t)expected.size()) << c.name;
      ASSERT_EQ(first_diff(content, expected), std::string::npos) << c.name;
    }

    std::string a = make_data(300 * kKiB, 63);
    std::string b = make_data(50, 64);
    uint64_t nodeid = 0;
    ASSERT_EQ(write_file(sc.dir, "wopen_trunc.bin", a, 64 * kKiB, nodeid), 0);
    sc.files.emplace_back(nodeid, "wopen_trunc.bin");
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_WRONLY | O_TRUNC, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ASSERT_EQ(handle->pwrite(b.data(), b.size(), 0), (ssize_t)b.size());
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
    std::string content;
    ASSERT_EQ(read_file(nodeid, content), (ssize_t)b.size());
    ASSERT_EQ(content, b);
  }

  // Replays the sequential READ sizes the kernel produces for different
  // syscall sizes over a 1 MiB object and compares byte-exact.
  void verify_sequential_read_sizes() {
    INIT_PHOTON();
    upload_part_size_ = 1536 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string data = make_data(kMiB, 41);
    uint64_t nodeid = 0;
    ASSERT_EQ(write_file(sc.dir, "seq_1m.bin", data, 2 * kKiB, nodeid), 0);
    sc.files.emplace_back(nodeid, "seq_1m.bin");
    for (size_t step : {4 * kKiB, 8 * kKiB, 64 * kKiB, 128 * kKiB, 256 * kKiB,
                        kMiB}) {
      std::string content;
      ASSERT_EQ(read_sequential(nodeid, data.size(), step, content), 0)
          << "read size " << step;
      ASSERT_EQ(first_diff(content, data), std::string::npos)
          << "read size " << step << " first diff at "
          << first_diff(content, data);
    }
  }

  // Interleaved 128 KiB READs from two threads on one handle, the way the
  // kernel issues readahead, plus 8 threads over a multipart object.
  void verify_concurrent_reads() {
    INIT_PHOTON();
    upload_part_size_ = 1536 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string small = make_data(kMiB, 51);
    std::string big = make_data(4 * kMiB, 52);
    uint64_t small_id = 0, big_id = 0;
    ASSERT_EQ(write_file(sc.dir, "par_1m.bin", small, 64 * kKiB, small_id), 0);
    sc.files.emplace_back(small_id, "par_1m.bin");
    ASSERT_EQ(write_file(sc.dir, "par_4m.bin", big, 200 * kKiB, big_id), 0);
    sc.files.emplace_back(big_id, "par_4m.bin");

    auto run = [&](uint64_t nodeid, const std::string& data, int threads,
                   int rounds) -> int {
      void* fh = nullptr;
      bool keep = false;
      int r = fs_->open(nodeid, O_RDONLY, &fh, &keep);
      if (r != 0) return r;
      auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
      const size_t slice = 128 * kKiB;
      const size_t slices = data.size() / slice;
      std::atomic<int> failures{0};
      for (int round = 0; round < rounds; round++) {
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; t++) {
          workers.emplace_back([&, t]() {
            INIT_PHOTON();
            std::string buf(slice, '\0');
            for (size_t i = t; i < slices; i += threads) {
              // odd rounds walk backwards so requests arrive out of order
              size_t s = (round % 2) ? slices - 1 - i : i;
              ssize_t rd = handle->pread(&buf[0], slice, s * slice);
              if (rd != static_cast<ssize_t>(slice) ||
                  memcmp(buf.data(), data.data() + s * slice, slice) != 0) {
                LOG_ERROR("slice ` (offset `) mismatch, rd=`", s, s * slice,
                          rd);
                failures.fetch_add(1);
              }
            }
          });
        }
        for (auto& w : workers) w.join();
      }
      fs_->release(nodeid, fh);
      return failures.load();
    };
    ASSERT_EQ(run(small_id, small, 2, 4), 0);
    ASSERT_EQ(run(big_id, big, 8, 3), 0);
  }

  // 16 threads over a 16 MiB multipart object, 64 KiB slices, both
  // directions.
  void verify_concurrent_reads_large() {
    INIT_PHOTON();
    upload_part_size_ = 1536 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string data = make_data(16 * kMiB, 71);
    uint64_t nodeid = 0;
    ASSERT_EQ(write_file(sc.dir, "par_16m.bin", data, 512 * kKiB, nodeid), 0);
    sc.files.emplace_back(nodeid, "par_16m.bin");
    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_RDONLY, &fh, &keep), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    const size_t slice = 64 * kKiB;
    const size_t slices = data.size() / slice;
    std::atomic<int> failures{0};
    for (int round = 0; round < 2; round++) {
      std::vector<std::thread> workers;
      for (int t = 0; t < 16; t++) {
        workers.emplace_back([&, t]() {
          INIT_PHOTON();
          std::string buf(slice, '\0');
          for (size_t i = t; i < slices; i += 16) {
            size_t s = round ? slices - 1 - i : i;
            ssize_t rd = handle->pread(&buf[0], slice, s * slice);
            if (rd != static_cast<ssize_t>(slice) ||
                memcmp(buf.data(), data.data() + s * slice, slice) != 0) {
              failures.fetch_add(1);
            }
          }
        });
      }
      for (auto& w : workers) w.join();
    }
    fs_->release(nodeid, fh);
    ASSERT_EQ(failures.load(), 0);
  }

  // A spliced write arrives as a pipe that a writer fills in 4 KiB
  // pieces, so read() returns short and the bufvec spans several parts.
  void verify_spliced_write_across_parts() {
    INIT_PHOTON();
    upload_part_size_ = 128 * kKiB;  // OSS parts must be at least 100 KiB
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string data = make_data(512 * kKiB, 72);
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    std::thread writer([&]() {
      for (size_t off = 0; off < data.size(); off += 4 * kKiB) {
        size_t n = std::min<size_t>(4 * kKiB, data.size() - off);
        const char* p = data.data() + off;
        while (n > 0) {
          ssize_t w = write(pipefd[1], p, n);
          if (w <= 0) return;
          p += w;
          n -= w;
        }
      }
      close(pipefd[1]);
    });
    DEFER(close(pipefd[0]));
    uint64_t nodeid = 0;
    struct stat st;
    void* fh = nullptr;
    ASSERT_EQ(fs_->creat(sc.dir, "spliced_parts.bin", O_CREAT | O_WRONLY, 0644,
                         0, 0, 0, &nodeid, &st, &fh),
              0);
    sc.files.emplace_back(nodeid, "spliced_parts.bin");
    struct fuse_bufvec bufv = FUSE_BUFVEC_INIT(data.size());
    bufv.buf[0].flags = FUSE_BUF_IS_FD;
    bufv.buf[0].fd = pipefd[0];
    ASSERT_EQ(fs_->write_buf(nodeid, fh, &bufv, 0), (ssize_t)data.size());
    writer.join();
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
    std::string content;
    ASSERT_EQ(read_file(nodeid, content), (ssize_t)data.size());
    ASSERT_EQ(first_diff(content, data), std::string::npos);
  }

  // Exactly two parts, flush completes with no tail, then a resume.
  void verify_multipart_edges() {
    INIT_PHOTON();
    upload_part_size_ = 256 * kKiB;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    std::string a = make_data(2 * 256 * kKiB, 81);
    std::string b = make_data(10, 82);
    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(write_file(sc.dir, "exact_parts.bin", a, 64 * kKiB, nodeid, &fh),
              0);
    sc.files.emplace_back(nodeid, "exact_parts.bin");
    ASSERT_EQ(fs_->flush(nodeid, fh), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    ASSERT_EQ(handle->pwrite(b.data(), b.size(), a.size()), (ssize_t)b.size());
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
    std::string content;
    ASSERT_EQ(read_file(nodeid, content), (ssize_t)(a.size() + b.size()));
    ASSERT_EQ(first_diff(content, a + b), std::string::npos);
  }
};

TEST_F(PvfsLargeIoTest, verify_multipart_flush_visible) {
  verify_multipart_flush_visible();
}
TEST_F(PvfsLargeIoTest, verify_write_after_multipart_flush) {
  verify_write_after_multipart_flush();
}
TEST_F(PvfsLargeIoTest, verify_append_large_existing) {
  verify_append_large_existing();
}
TEST_F(PvfsLargeIoTest, verify_write_open_appends) {
  verify_write_open_appends();
}
TEST_F(PvfsLargeIoTest, verify_sequential_read_sizes) {
  verify_sequential_read_sizes();
}
TEST_F(PvfsLargeIoTest, verify_concurrent_reads) { verify_concurrent_reads(); }
TEST_F(PvfsLargeIoTest, verify_concurrent_reads_large) {
  verify_concurrent_reads_large();
}
TEST_F(PvfsLargeIoTest, verify_spliced_write_across_parts) {
  verify_spliced_write_across_parts();
}
TEST_F(PvfsLargeIoTest, verify_multipart_edges) { verify_multipart_edges(); }
