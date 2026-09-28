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

#include <photon/thread/thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "common/macros.h"
#include "common/utils.h"
#include "fs/fs.h"
#include "oss/oss_store.h"
#include "pvfs_test_suite.h"


DEFINE_string(bench_output, "", "CSV output file path for benchmark results");

using Clock = std::chrono::high_resolution_clock;

struct BenchConfig {
  std::string label;
  size_t file_size;
  int file_count;
  int concurrency;
};

struct BenchResult {
  double write_total_ms = 0;
  double read_total_ms = 0;
  double delete_total_ms = 0;
  double write_avg_ms = 0;
  double read_avg_ms = 0;
  double delete_avg_ms = 0;
  double write_throughput_mbps = 0;
  double read_throughput_mbps = 0;
  int errors = 0;
  // Sub-operation averages (ms)
  double w_creat_ms = 0;
  double w_pwrite_ms = 0;
  double w_release_ms = 0;
  double r_lookup_ms = 0;
  double r_open_ms = 0;
  double r_pread_ms = 0;
  double r_release_ms = 0;
  double d_lookup_ms = 0;
  double d_unlink_ms = 0;
};

static std::string gen_data(size_t size, int seed) {
  std::string data(size, '\0');
  for (size_t i = 0; i < size; i++) {
    data[i] = static_cast<char>((seed + i) & 0xFF);
  }
  return data;
}

static double ms_since(Clock::time_point start) {
  auto end = Clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

static void run_concurrent(int concurrency, std::function<void()> fn) {
  if (concurrency <= 1) {
    fn();
    return;
  }
  std::vector<std::thread> threads;
  for (int c = 0; c < concurrency; c++) {
    threads.emplace_back([&fn]() {
      // Each OS thread needs its own photon env for coroutine-based fs calls
      photon::init(OSSFS_EVENT_ENGINE, photon::INIT_IO_NONE);
      fn();
      photon::fini();
    });
  }
  for (auto& t : threads) t.join();
}

// ---- Generic filesystem benchmark (OssFs on OSS and on PVFS) ----
static BenchResult bench_fs(IFileSystemFuseLL* fs, const BenchConfig& cfg,
                            uint64_t parent_nodeid) {
  BenchResult result;
  std::vector<std::string> names;
  for (int i = 0; i < cfg.file_count; i++) {
    names.push_back("bench_" + std::to_string(i));
  }

  std::vector<std::string> datas;
  for (int i = 0; i < cfg.file_count; i++) {
    datas.push_back(gen_data(cfg.file_size, i));
  }

  // --- Write (creat + pwrite + release) ---
  {
    std::atomic<int> idx{0};
    std::atomic<int> errors{0};
    std::mutex mu;
    std::vector<double> latencies;
    std::vector<double> creat_lats, pwrite_lats, release_lats;

    auto writer = [&]() {
      while (true) {
        int i = idx.fetch_add(1);
        if (i >= cfg.file_count) break;
        auto t0 = Clock::now();
        uint64_t nodeid = 0;
        struct stat st;
        void* fh = nullptr;
        int r = fs->creat(parent_nodeid, names[i], O_CREAT | O_RDWR, 0644, 0,
                          0, 0, &nodeid, &st, &fh);
        double t_creat = ms_since(t0);
        if (r != 0) {
          errors++;
          std::lock_guard<std::mutex> lock(mu);
          latencies.push_back(t_creat);
          creat_lats.push_back(t_creat);
          pwrite_lats.push_back(0);
          release_lats.push_back(0);
          continue;
        }
        auto t1 = Clock::now();
        auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
        ssize_t w = handle->pwrite(datas[i].data(), datas[i].size(), 0);
        double t_pwrite = ms_since(t1);
        if (w < 0) errors++;
        auto t2 = Clock::now();
        fs->release(nodeid, fh);
        double t_release = ms_since(t2);
        fs->forget(nodeid, 1);
        double lat = ms_since(t0);
        std::lock_guard<std::mutex> lock(mu);
        latencies.push_back(lat);
        creat_lats.push_back(t_creat);
        pwrite_lats.push_back(t_pwrite);
        release_lats.push_back(t_release);
      }
    };

    auto t0 = Clock::now();
    run_concurrent(cfg.concurrency, writer);
    result.write_total_ms = ms_since(t0);
    result.write_avg_ms =
        std::accumulate(latencies.begin(), latencies.end(), 0.0) /
        latencies.size();
    result.w_creat_ms =
        std::accumulate(creat_lats.begin(), creat_lats.end(), 0.0) /
        creat_lats.size();
    result.w_pwrite_ms =
        std::accumulate(pwrite_lats.begin(), pwrite_lats.end(), 0.0) /
        pwrite_lats.size();
    result.w_release_ms =
        std::accumulate(release_lats.begin(), release_lats.end(), 0.0) /
        release_lats.size();
    result.errors += errors.load();
  }

  // --- Read (lookup + open + pread + release) ---
  {
    std::atomic<int> idx{0};
    std::atomic<int> errors{0};
    std::mutex mu;
    std::vector<double> latencies;
    std::vector<double> lookup_lats, open_lats, pread_lats, release_lats;

    auto reader = [&]() {
      while (true) {
        int i = idx.fetch_add(1);
        if (i >= cfg.file_count) break;
        auto t0 = Clock::now();
        uint64_t nodeid = 0;
        struct stat st;
        int r = fs->lookup(parent_nodeid, names[i], &nodeid, &st);
        double t_lookup = ms_since(t0);
        if (r != 0) {
          errors++;
          std::lock_guard<std::mutex> lock(mu);
          latencies.push_back(t_lookup);
          lookup_lats.push_back(t_lookup);
          open_lats.push_back(0);
          pread_lats.push_back(0);
          release_lats.push_back(0);
          continue;
        }
        auto t1 = Clock::now();
        void* fh = nullptr;
        bool keep = false;
        r = fs->open(nodeid, O_RDONLY, &fh, &keep);
        double t_open = ms_since(t1);
        if (r != 0) {
          errors++;
          fs->forget(nodeid, 1);
          std::lock_guard<std::mutex> lock(mu);
          latencies.push_back(ms_since(t0));
          lookup_lats.push_back(t_lookup);
          open_lats.push_back(t_open);
          pread_lats.push_back(0);
          release_lats.push_back(0);
          continue;
        }
        auto t2 = Clock::now();
        std::string buf(cfg.file_size, '\0');
        auto* handle = reinterpret_cast<IFileHandleFuseLL*>(fh);
        ssize_t rd = handle->pread(buf.data(), buf.size(), 0);
        double t_pread = ms_since(t2);
        if (rd < 0) errors++;
        auto t3 = Clock::now();
        fs->release(nodeid, fh);
        double t_release = ms_since(t3);
        fs->forget(nodeid, 1);
        double lat = ms_since(t0);
        std::lock_guard<std::mutex> lock(mu);
        latencies.push_back(lat);
        lookup_lats.push_back(t_lookup);
        open_lats.push_back(t_open);
        pread_lats.push_back(t_pread);
        release_lats.push_back(t_release);
      }
    };

    auto t0 = Clock::now();
    run_concurrent(cfg.concurrency, reader);
    result.read_total_ms = ms_since(t0);
    result.read_avg_ms =
        std::accumulate(latencies.begin(), latencies.end(), 0.0) /
        latencies.size();
    result.r_lookup_ms =
        std::accumulate(lookup_lats.begin(), lookup_lats.end(), 0.0) /
        lookup_lats.size();
    result.r_open_ms =
        std::accumulate(open_lats.begin(), open_lats.end(), 0.0) /
        open_lats.size();
    result.r_pread_ms =
        std::accumulate(pread_lats.begin(), pread_lats.end(), 0.0) /
        pread_lats.size();
    result.r_release_ms =
        std::accumulate(release_lats.begin(), release_lats.end(), 0.0) /
        release_lats.size();
    result.errors += errors.load();
  }

  // --- Delete (lookup + unlink) ---
  {
    std::atomic<int> idx{0};
    std::mutex mu;
    std::vector<double> latencies;
    std::vector<double> lookup_lats, unlink_lats;

    auto deleter = [&]() {
      while (true) {
        int i = idx.fetch_add(1);
        if (i >= cfg.file_count) break;
        auto t0 = Clock::now();
        uint64_t nodeid = 0;
        struct stat st;
        int r = fs->lookup(parent_nodeid, names[i], &nodeid, &st);
        double t_lookup = ms_since(t0);
        if (r == 0) {
          auto t1 = Clock::now();
          fs->unlink(parent_nodeid, names[i]);
          double t_unlink = ms_since(t1);
          fs->forget(nodeid, 1);
          double lat = ms_since(t0);
          std::lock_guard<std::mutex> lock(mu);
          latencies.push_back(lat);
          lookup_lats.push_back(t_lookup);
          unlink_lats.push_back(t_unlink);
        } else {
          std::lock_guard<std::mutex> lock(mu);
          latencies.push_back(t_lookup);
          lookup_lats.push_back(t_lookup);
          unlink_lats.push_back(0);
        }
      }
    };

    auto t0 = Clock::now();
    run_concurrent(cfg.concurrency, deleter);
    result.delete_total_ms = ms_since(t0);
    result.delete_avg_ms =
        std::accumulate(latencies.begin(), latencies.end(), 0.0) /
        latencies.size();
    result.d_lookup_ms =
        std::accumulate(lookup_lats.begin(), lookup_lats.end(), 0.0) /
        lookup_lats.size();
    result.d_unlink_ms =
        std::accumulate(unlink_lats.begin(), unlink_lats.end(), 0.0) /
        unlink_lats.size();
  }

  double total_bytes = (double)cfg.file_size * cfg.file_count;
  result.write_throughput_mbps =
      (total_bytes / (1024.0 * 1024.0)) / (result.write_total_ms / 1000.0);
  result.read_throughput_mbps =
      (total_bytes / (1024.0 * 1024.0)) / (result.read_total_ms / 1000.0);

  return result;
}

// ---- Test fixture ----
class PvfsBenchTest : public PvfsTestSuite {
 protected:
  void SetUp() override {
    PvfsTestSuite::SetUp();
    if (FLAGS_oss_bucket.empty()) {
      GTEST_SKIP() << "benchmark needs --oss_endpoint/--oss_bucket";
    }
  }

  // Initialize OssFs with BackgroundVCpuEnv (same as Ossfs2TestSuite::do_init)
  OssFileSystem::OssFs* init_ossfs() {
    using namespace OssFileSystem;

    std::string endpoint = FLAGS_oss_endpoint;
    std::string bucket = FLAGS_oss_bucket;
    std::string prefix = FLAGS_oss_bucket_prefix;
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    auto* bg_env = new BGVCpuObjStoreEnv;

    ScopedBlockAllSignal block_signals;

    auto* executor = new photon::Executor(
        OSSFS_EVENT_ENGINE, photon::INIT_IO_NONE, {}, {16, 1024});
    auto* store = executor->perform([&]() {
      ObjStoreOptions options;
      options.endpoint = endpoint;
      options.bucket = bucket;
      options.prefix = prefix;
      options.user_agent = "ossfs2-bench";
      options.request_timeout_us = FLAGS_oss_request_timeout_ms * 1000;
      options.ip_version = ip_version_for(false);
      return new_oss_store(FLAGS_oss_access_key_id.c_str(),
                           FLAGS_oss_access_key_secret.c_str(), options);
    });
    bg_env->add_obj_store_env(executor, store);

    BackgroundVCpuEnv bg_vcpu_env;
    bg_vcpu_env.bg_obj_store_env = bg_env;

    OssFsOptions fs_opts;
    auto* oss_fs = new OssFs(fs_opts, bg_vcpu_env);
    return oss_fs;
  }

  void run_all_benchmarks() {
    INIT_PHOTON();

    // --- Initialize PVFS ---
    init();
    ASSERT_NE(fs_, nullptr);

    // Find a test table for PVFS
    std::vector<DirEntry> dbs;
    ASSERT_EQ(list_dir(root_nodeid_, dbs), 0);
    ASSERT_GT(dbs.size(), 0UL);

    uint64_t db_nodeid = 0, tbl_nodeid = 0;
    struct stat st;
    bool found_table = false;
    for (const auto& db : dbs) {
      int r = fs_->lookup(root_nodeid_, db.name, &db_nodeid, &st);
      if (r != 0) continue;
      std::vector<DirEntry> tables;
      r = list_dir(db_nodeid, tables);
      if (r != 0 || tables.empty()) {
        fs_->forget(db_nodeid, 1);
        continue;
      }
      r = fs_->lookup(db_nodeid, tables[0].name, &tbl_nodeid, &st);
      if (r == 0) {
        found_table = true;
        LOG_INFO("PVFS bench table: `/`", db.name, tables[0].name);
        break;
      }
      fs_->forget(db_nodeid, 1);
    }
    ASSERT_TRUE(found_table);
    DEFER(fs_->forget(db_nodeid, 1));
    DEFER(fs_->forget(tbl_nodeid, 1));

    // Create bench dir in PVFS table
    uint64_t pvfs_bench_dir = 0;
    std::string bench_dirname = "perf_bench_" + std::to_string(time(nullptr));
    int r = fs_->mkdir(tbl_nodeid, bench_dirname, 0755, 0, 0, 0,
                       &pvfs_bench_dir, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      fs_->rmdir(tbl_nodeid, bench_dirname);
      fs_->forget(pvfs_bench_dir, 1);
    });

    // --- Initialize OssFs ---
    auto* oss_fs = init_ossfs();
    ASSERT_NE(oss_fs, nullptr);
    DEFER(delete oss_fs);

    uint64_t oss_root = 1;  // OssFs root nodeid

    // Create bench dir in OssFs
    uint64_t oss_bench_dir = 0;
    r = oss_fs->mkdir(oss_root, bench_dirname.c_str(), 0755, 0, 0, 0,
                      &oss_bench_dir, &st);
    ASSERT_EQ(r, 0);
    DEFER({
      oss_fs->rmdir(oss_root, bench_dirname.c_str());
      oss_fs->forget(oss_bench_dir, 1);
    });

    // --- Benchmark matrix ---
    std::vector<BenchConfig> configs = {
        {"1KB", 1024, 10, 1},
        {"1KB", 1024, 10, 5},
        {"1KB", 1024, 10, 10},
        {"100KB", 100 * 1024, 5, 1},
        {"100KB", 100 * 1024, 5, 5},
        {"100KB", 100 * 1024, 5, 10},
        {"1MB", 1024 * 1024, 3, 1},
        {"1MB", 1024 * 1024, 3, 5},
        {"1MB", 1024 * 1024, 3, 10},
    };

    // --- Output header ---
    printf("\n");
    printf("%-8s %-6s | %-10s %-10s %-10s %-10s | %-10s %-10s %-10s %-10s\n",
           "Size", "Conc", "OSS-W(ms)", "OSS-R(ms)", "OSS-D(ms)",
           "OSS-TP(MB/s)", "PVFS-W(ms)", "PVFS-R(ms)", "PVFS-D(ms)",
           "PVFS-TP(MB/s)");
    printf("%-8s %-6s | %-10s %-10s %-10s %-10s | %-10s %-10s %-10s %-10s\n",
           "", "", "crt/pw/rel", "lk/op/rd/rl", "lk/unl", "",
           "crt/pw/rel", "lk/op/rd/rl", "lk/unl", "");
    printf("%.*s\n", 120,
           "--------------------------------------------------------------"
           "--------------------------------------------------------------");

    // CSV output
    FILE* csv = nullptr;
    std::string csv_path = FLAGS_bench_output;
    if (csv_path.empty()) {
      const char* work_dir = getenv("WORK_DIR");
      if (work_dir) {
        csv_path = std::string(work_dir) + "/ci-logs/bench-results.csv";
      }
    }
    if (!csv_path.empty()) {
      csv = fopen(csv_path.c_str(), "w");
      if (csv) {
        fprintf(csv,
                "size,concurrency,file_count,"
                "oss_write_avg_ms,oss_read_avg_ms,oss_delete_avg_ms,"
                "oss_write_total_ms,oss_read_total_ms,oss_delete_total_ms,"
                "oss_write_tp_mbps,oss_read_tp_mbps,oss_errors,"
                "oss_w_creat,oss_w_pwrite,oss_w_release,"
                "oss_r_lookup,oss_r_open,oss_r_pread,oss_r_release,"
                "oss_d_lookup,oss_d_unlink,"
                "pvfs_write_avg_ms,pvfs_read_avg_ms,pvfs_delete_avg_ms,"
                "pvfs_write_total_ms,pvfs_read_total_ms,pvfs_delete_total_ms,"
                "pvfs_write_tp_mbps,pvfs_read_tp_mbps,pvfs_errors,"
                "pvfs_w_creat,pvfs_w_pwrite,pvfs_w_release,"
                "pvfs_r_lookup,pvfs_r_open,pvfs_r_pread,pvfs_r_release,"
                "pvfs_d_lookup,pvfs_d_unlink\n");
      }
    }
    DEFER(if (csv) fclose(csv));

    for (const auto& cfg : configs) {
      LOG_INFO("=== Bench: ` x` concurrency=` ===", cfg.label, cfg.file_count,
               cfg.concurrency);

      // Run OSS benchmark (via OssFs filesystem path)
      auto oss_result = bench_fs(oss_fs, cfg, oss_bench_dir);

      // Run PVFS benchmark (OssFs over PvfsObjStore)
      auto pvfs_result = bench_fs(fs_, cfg, pvfs_bench_dir);

      // Print results - totals
      printf("%-8s %-6d | %8.1f %8.1f %8.1f %8.3f | %8.1f %8.1f "
             "%8.1f %8.3f\n",
             cfg.label.c_str(), cfg.concurrency, oss_result.write_avg_ms,
             oss_result.read_avg_ms, oss_result.delete_avg_ms,
             oss_result.read_throughput_mbps, pvfs_result.write_avg_ms,
             pvfs_result.read_avg_ms, pvfs_result.delete_avg_ms,
             pvfs_result.read_throughput_mbps);
      // Print sub-op breakdown
      printf("%-8s %-6s | %3.0f/%3.0f/%3.0f %2.0f/%2.0f/%2.0f/%2.0f %3.0f/%3.0f %8s | "
             "%3.0f/%3.0f/%3.0f %2.0f/%2.0f/%2.0f/%2.0f %3.0f/%3.0f\n",
             "", "",
             oss_result.w_creat_ms, oss_result.w_pwrite_ms, oss_result.w_release_ms,
             oss_result.r_lookup_ms, oss_result.r_open_ms, oss_result.r_pread_ms, oss_result.r_release_ms,
             oss_result.d_lookup_ms, oss_result.d_unlink_ms, "",
             pvfs_result.w_creat_ms, pvfs_result.w_pwrite_ms, pvfs_result.w_release_ms,
             pvfs_result.r_lookup_ms, pvfs_result.r_open_ms, pvfs_result.r_pread_ms, pvfs_result.r_release_ms,
             pvfs_result.d_lookup_ms, pvfs_result.d_unlink_ms);

      if (oss_result.errors > 0 || pvfs_result.errors > 0) {
        printf("  [WARN] errors: OSS=%d, PVFS=%d\n", oss_result.errors,
               pvfs_result.errors);
      }

      // CSV row
      if (csv) {
        fprintf(csv,
                "%s,%d,%d,"
                "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%d,"
                "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
                "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%d,"
                "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                cfg.label.c_str(), cfg.concurrency, cfg.file_count,
                oss_result.write_avg_ms, oss_result.read_avg_ms,
                oss_result.delete_avg_ms, oss_result.write_total_ms,
                oss_result.read_total_ms, oss_result.delete_total_ms,
                oss_result.write_throughput_mbps,
                oss_result.read_throughput_mbps, oss_result.errors,
                oss_result.w_creat_ms, oss_result.w_pwrite_ms, oss_result.w_release_ms,
                oss_result.r_lookup_ms, oss_result.r_open_ms, oss_result.r_pread_ms, oss_result.r_release_ms,
                oss_result.d_lookup_ms, oss_result.d_unlink_ms,
                pvfs_result.write_avg_ms, pvfs_result.read_avg_ms,
                pvfs_result.delete_avg_ms, pvfs_result.write_total_ms,
                pvfs_result.read_total_ms, pvfs_result.delete_total_ms,
                pvfs_result.write_throughput_mbps,
                pvfs_result.read_throughput_mbps, pvfs_result.errors,
                pvfs_result.w_creat_ms, pvfs_result.w_pwrite_ms, pvfs_result.w_release_ms,
                pvfs_result.r_lookup_ms, pvfs_result.r_open_ms, pvfs_result.r_pread_ms, pvfs_result.r_release_ms,
                pvfs_result.d_lookup_ms, pvfs_result.d_unlink_ms);
        fflush(csv);
      }
    }

    printf("\n");
    if (csv) {
      LOG_INFO("Benchmark results written to: `", csv_path);
    }
  }
};

TEST_F(PvfsBenchTest, run_all_benchmarks) { run_all_benchmarks(); }
