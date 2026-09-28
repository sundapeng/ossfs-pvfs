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

#include "common/fault_injector.h"
#include "common/fuse.h"
#include "common/logger.h"
#include "pvfs_test_suite.h"

// Error paths driven by the OSS fault injector: every upload failure must
// leave the handle in a state that cannot crash or corrupt the object.
class PvfsFaultTest : public PvfsTestSuite {
 protected:
  static constexpr size_t kPart = 256 * 1024;
  // The OSS client retries twice and OssFs retries parts on top: a burst
  // this long fails the write for good, two hits are absorbed.
  static constexpr uint32_t kBurst = 50;
  static constexpr uint32_t kAbsorbed = 2;
  static constexpr int64_t kOssBoundMs = 5000;

  void SetUp() override {
    PvfsTestSuite::SetUp();
    if (!g_fault_injector) g_fault_injector.reset(new FaultInjector);
    g_fault_injector->clear_all_injections();
  }
  void TearDown() override {
    if (g_fault_injector) g_fault_injector->clear_all_injections();
    PvfsTestSuite::TearDown();
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

  // Create `name` and write `bytes` of filler through the returned handle.
  int create_and_fill(Scratch& sc, const char* name, size_t bytes,
                      uint64_t& nodeid, void*& fh) {
    struct stat st;
    int r = fs_->creat(sc.dir, name, O_CREAT | O_WRONLY, 0644, 0, 0, 0, &nodeid,
                       &st, &fh);
    if (r != 0) return r;
    sc.files.emplace_back(nodeid, name);
    std::string chunk(64 * 1024, 'p');
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    for (size_t off = 0; off < bytes; off += chunk.size()) {
      size_t n = std::min(chunk.size(), bytes - off);
      ssize_t w = handle->pwrite(chunk.data(), n, off);
      if (w != static_cast<ssize_t>(n)) return w < 0 ? w : -EIO;
    }
    return 0;
  }

  // Size of the object as another mount sees it: -ENOENT when the failed
  // upload never produced one, else its length.
  ssize_t remote_size(const char* name) {
    auto other = make_instance(base_opts());
    if (!other) return -EIO;
    FsSwap swap(*this, other->fs);
    uint64_t nodeid = 0;
    struct stat st;
    int r = lookup_path("/" + FLAGS_pvfs_test_database + "/" +
                            FLAGS_pvfs_test_table + "/" + test_subdir_ + "/" +
                            name,
                        nodeid, st);
    if (r != 0) return r;
    DEFER(fs_->forget(nodeid, 1));
    std::string buf;
    return read_file(nodeid, buf);
  }

  // Lay `bytes` of filler down as a complete object.
  void lay_down(Scratch& sc, const char* name, size_t bytes, uint64_t& nodeid) {
    void* fh = nullptr;
    ASSERT_EQ(create_and_fill(sc, name, bytes, nodeid, fh), 0);
    ASSERT_EQ(fs_->release(nodeid, fh), 0);
  }

  // A failing complete leaves no object behind and the handle done for:
  // every later write, flush and release fails, nothing is PUT instead.
  void verify_failed_complete_not_aborted() {
    INIT_PHOTON();
    upload_part_size_ = kPart;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    // Exactly two full parts: the flush only has to complete the upload.
    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(create_and_fill(sc, "complete_fail.bin", 2 * kPart, nodeid, fh),
              0);
    g_fault_injector->set_injection(FI_OssError_Failed_Without_Call,
                                    FaultInjection{kBurst});
    int err = fs_->flush(nodeid, fh);
    ASSERT_LT(err, 0);
    g_fault_injector->clear_all_injections();
    EXPECT_LT(fs_->flush(nodeid, fh), 0);
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    EXPECT_LT(handle->pwrite("x", 1, 2 * kPart), 0);
    EXPECT_LT(fs_->release(nodeid, fh), 0);
    EXPECT_LE(remote_size("complete_fail.bin"), 0);
  }

  // A failed upload_part must not be followed by a single-object PUT of
  // the tail on release.
  void verify_failed_upload_part_is_sticky() {
    INIT_PHOTON();
    upload_part_size_ = kPart;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    // One full part is uploaded during the writes, half a part is buffered.
    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(create_and_fill(sc, "part_fail.bin", kPart + kPart / 2, nodeid,
                              fh),
              0);
    g_fault_injector->set_injection(FI_OssError_Failed_Without_Call,
                                    FaultInjection{kBurst});
    int err = fs_->flush(nodeid, fh);
    ASSERT_LT(err, 0);
    g_fault_injector->clear_all_injections();
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    EXPECT_LT(handle->pwrite("x", 1, kPart + kPart / 2), 0);
    EXPECT_LT(fs_->flush(nodeid, fh), 0);
    EXPECT_LT(fs_->release(nodeid, fh), 0);
    EXPECT_LE(remote_size("part_fail.bin"), 0);
  }

  // A timed-out HEAD is reported as such, promptly, and does not stick.
  void verify_timeout_before_lookup() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    g_fault_injector->set_injection(FI_OssError_Call_Timeout, FaultInjection{1});
    uint64_t nodeid = 0;
    struct stat st;
    auto t0 = clock_now();
    EXPECT_EQ(fs_->lookup(sc.dir, "absent", &nodeid, &st), -ETIMEDOUT);
    EXPECT_LT(ms_since(t0), 1000);
    EXPECT_FALSE(g_fault_injector->is_injection_enabled(FI_OssError_Call_Timeout));
    EXPECT_EQ(fs_->lookup(sc.dir, "absent", &nodeid, &st), -ENOENT);
  }

  // A burst of 5xx beyond the retries fails the flush with a sticky error;
  // the object is either absent or complete, since the 5xx is injected on
  // the response and OSS may have taken the PUT. Two are absorbed.
  void verify_5xx_retry_budget() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(create_and_fill(sc, "5xx_three.bin", 100, nodeid, fh), 0);
    g_fault_injector->set_injection(FI_OssError_5xx, FaultInjection{kBurst});
    auto t0 = clock_now();
    int err = fs_->flush(nodeid, fh);
    EXPECT_LT(err, 0);
    EXPECT_LT(ms_since(t0), 30000);
    g_fault_injector->clear_all_injections();
    EXPECT_LT(fs_->flush(nodeid, fh), 0);
    EXPECT_LT(fs_->release(nodeid, fh), 0);
    ssize_t left = remote_size("5xx_three.bin");
    EXPECT_TRUE(left <= 0 || left == 100) << left;

    uint64_t n2 = 0;
    void* fh2 = nullptr;
    ASSERT_EQ(create_and_fill(sc, "5xx_two.bin", 100, n2, fh2), 0);
    g_fault_injector->set_injection(FI_OssError_5xx, FaultInjection{kAbsorbed});
    t0 = clock_now();
    EXPECT_EQ(fs_->flush(n2, fh2), 0);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    EXPECT_FALSE(g_fault_injector->is_injection_enabled(FI_OssError_5xx));
    EXPECT_EQ(fs_->release(n2, fh2), 0);
    EXPECT_EQ(remote_size("5xx_two.bin"), 100);
  }

  // A write that crosses into a failing part upload returns the error,
  // the handle stays broken and no object appears.
  void verify_part_failure_mid_write() {
    INIT_PHOTON();
    upload_part_size_ = kPart;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));

    uint64_t nodeid = 0;
    void* fh = nullptr;
    ASSERT_EQ(create_and_fill(sc, "mid_part.bin", 200 * 1024, nodeid, fh), 0);
    g_fault_injector->set_injection(FI_OssError_Failed_Without_Call,
                                    FaultInjection{kBurst});
    std::string chunk(100 * 1024, 'q');
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    auto t0 = clock_now();
    ssize_t w = handle->pwrite(chunk.data(), chunk.size(), 200 * 1024);
    if (w == static_cast<ssize_t>(chunk.size())) {
      // The part went out asynchronously; the flush reports the failure.
      EXPECT_LT(fs_->flush(nodeid, fh), 0);
    } else {
      EXPECT_LT(w, 0);
    }
    EXPECT_LT(ms_since(t0), 30000);
    g_fault_injector->clear_all_injections();
    EXPECT_LT(handle->pwrite("x", 1, 300 * 1024), 0);
    EXPECT_LT(fs_->flush(nodeid, fh), 0);
    EXPECT_LT(fs_->release(nodeid, fh), 0);
    EXPECT_LE(remote_size("mid_part.bin"), 0);
  }

  // Appending to a multipart object fails while the existing content is
  // being taken over: the write is refused and the object stays intact.
  void verify_resume_failure_keeps_object() {
    INIT_PHOTON();
    upload_part_size_ = kPart;
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t nodeid = 0;
    lay_down(sc, "resume.bin", kPart + 100, nodeid);

    void* fh = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(nodeid, O_WRONLY, &fh, &keep), 0);
    g_fault_injector->set_injection(FI_OssError_Failed_Without_Call,
                                    FaultInjection{kBurst});
    auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
    auto t0 = clock_now();
    ssize_t w = handle->pwrite("x", 1, kPart + 100);
    int err = w < 0 ? static_cast<int>(w) : fs_->flush(nodeid, fh);
    EXPECT_LT(err, 0);
    EXPECT_LT(ms_since(t0), 30000);
    g_fault_injector->clear_all_injections();
    // Nothing of the refused write may reach the object.
    fs_->flush(nodeid, fh);
    fs_->release(nodeid, fh);
    EXPECT_EQ(remote_size("resume.bin"), static_cast<ssize_t>(kPart + 100));
  }

  // The object shrinks or vanishes under an open reader: a short read or
  // an error, promptly, never a hang or stale bytes.
  void verify_object_changed_under_reader() {
    INIT_PHOTON();
    init();
    Scratch sc;
    setup_dir(sc);
    DEFER(cleanup_dir(sc));
    uint64_t shrink = 0, gone = 0;
    lay_down(sc, "shrink.bin", 100, shrink);
    lay_down(sc, "gone.bin", 100, gone);

    void *fh_shrink = nullptr, *fh_gone = nullptr;
    bool keep = false;
    ASSERT_EQ(fs_->open(shrink, O_RDONLY, &fh_shrink, &keep), 0);
    ASSERT_EQ(fs_->open(gone, O_RDONLY, &fh_gone, &keep), 0);

    auto other = make_instance(base_opts());
    ASSERT_NE(other, nullptr);
    std::string dir_path = "/" + FLAGS_pvfs_test_database + "/" +
                           FLAGS_pvfs_test_table + "/" + test_subdir_;
    {
      FsSwap swap(*this, other->fs);
      uint64_t odir = 0, oshrink = 0, ogone = 0;
      struct stat st;
      ASSERT_EQ(lookup_path(dir_path, odir, st), 0);
      ASSERT_EQ(fs_->lookup(odir, "shrink.bin", &oshrink, &st), 0);
      ASSERT_EQ(fs_->lookup(odir, "gone.bin", &ogone, &st), 0);
      memset(&st, 0, sizeof(st));
      ASSERT_EQ(fs_->setattr(oshrink, &st, FUSE_SET_ATTR_SIZE), 0);
      ASSERT_EQ(fs_->unlink(odir, "gone.bin"), 0);
      fs_->forget(ogone, 1);
      fs_->forget(oshrink, 1);
      fs_->forget(odir, 1);
    }

    char buf[100];
    auto* rs = reinterpret_cast<IFileHandleFuseLL*>(fh_shrink);
    auto t0 = clock_now();
    EXPECT_LE(rs->pread(buf, sizeof(buf), 0), 0);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    auto* rg = reinterpret_cast<IFileHandleFuseLL*>(fh_gone);
    t0 = clock_now();
    EXPECT_LT(rg->pread(buf, sizeof(buf), 0), 0);
    EXPECT_LT(ms_since(t0), kOssBoundMs);
    fs_->release(shrink, fh_shrink);
    fs_->release(gone, fh_gone);
  }
};

TEST_F(PvfsFaultTest, verify_failed_upload_part_is_sticky) {
  verify_failed_upload_part_is_sticky();
}
TEST_F(PvfsFaultTest, verify_failed_complete_not_aborted) {
  verify_failed_complete_not_aborted();
}
TEST_F(PvfsFaultTest, TimeoutBeforeLookup) { verify_timeout_before_lookup(); }
TEST_F(PvfsFaultTest, FiveXxRetryBudget) { verify_5xx_retry_budget(); }
TEST_F(PvfsFaultTest, PartFailureMidWrite) { verify_part_failure_mid_write(); }
TEST_F(PvfsFaultTest, ResumeFailureKeepsObject) {
  verify_resume_failure_keeps_object();
}
TEST_F(PvfsFaultTest, ObjectChangedUnderReader) {
  verify_object_changed_under_reader();
}
