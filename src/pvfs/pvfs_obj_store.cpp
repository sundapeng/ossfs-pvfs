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

#include "pvfs_obj_store.h"

#include <sys/stat.h>

#include "common/logger.h"

namespace PvfsFileSystem {

using OssFileSystem::ObjDirent;

static constexpr mode_t kPvfsDirMode = (S_IFDIR | 0755);

// Keep the original store alive so cleanup can run even if the table is
// evicted, moved or no longer available from the catalog.
struct PvfsObjStore::UploadContext {
  std::string database;
  std::string table;
  void *inner;
  std::shared_ptr<IObjStore> origin;
  photon::Executor *executor;

  bool same_location(IObjStore *store) const {
    const auto &before = origin->get_options();
    const auto &now = store->get_options();
    return before.endpoint == now.endpoint && before.bucket == now.bucket &&
           before.prefix == now.prefix;
  }

  int abort() {
    return executor->perform([&]() {
      void *pending = inner;
      inner = nullptr;
      return origin->abort_multipart_upload(pending);
    });
  }
};

PvfsObjStore::PvfsObjStore(PvfsRuntime *runtime, int vcpu,
                           photon::Executor *executor)
    : runtime_(runtime),
      vcpu_(vcpu),
      executor_(executor),
      prefix_(runtime->options().prefix) {
  // Only for logging and validate_creds(): the catalog stands in for the
  // bucket, the data-plane options live in the table cache.
  opts_.endpoint = runtime->options().rest.endpoint;
  opts_.bucket = "pvfs://" + runtime->options().rest.catalog;
  opts_.prefix = prefix_;
  opts_.user_agent = runtime->options().cache.store_options.user_agent;
  opts_.max_list_ret_cnt = runtime->options().cache.store_options.max_list_ret_cnt;
}

std::string PvfsObjStore::Target::table_path() const {
  std::string p = "/";
  p += pp.subpath;
  if (!pp.subpath.empty() && full.back() == '/') p += '/';
  return p;
}

PvfsObjStore::Target PvfsObjStore::target(std::string_view path) const {
  Target t;
  if (prefix_.empty()) {
    t.full.assign(path.data(), path.size());
  } else {
    t.full = "/" + prefix_;
    if (path.empty() || path.front() != '/') t.full += '/';
    t.full.append(path.data(), path.size());
  }
  if (t.full.empty()) t.full = "/";
  t.pp = PvfsPath::parse(t.full);
  return t;
}

int PvfsObjStore::write_guard(const Target& t) const {
  if (t.pp.level != PvfsPath::SUBPATH) return -EPERM;
  if (runtime_->options().readonly) return -EROFS;
  return runtime_->check_mutable(t.pp.database, t.pp.table, t.pp.subpath);
}

void PvfsObjStore::fill_dir_stat(struct stat* buf) {
  if (!buf) return;
  buf->st_mode = kPvfsDirMode;
  buf->st_size = 0;
}

int PvfsObjStore::table_store(const std::string& database,
                              const std::string& table,
                              std::shared_ptr<IObjStore>* store) {
  int err = 0;
  auto entry = runtime_->table_cache()->resolve(database, table, &err);
  if (!entry) return err;
  *store = runtime_->table_cache()->store_for_vcpu(*entry, vcpu_);
  if (!*store) {
    LOG_ERROR("PVFS `/`: no store for vCPU `", database, table, vcpu_);
    return -EIO;
  }
  return 0;
}

template <class F>
auto PvfsObjStore::with_store(const std::string &database,
                              const std::string &table, F &&fn, bool retry,
                              std::shared_ptr<IObjStore> *used_store)
    -> decltype(fn((IObjStore *)nullptr)) {
  int err = 0;
  auto* cache = runtime_->table_cache();
  auto entry = cache->resolve(database, table, &err);
  if (!entry) return err;
  auto store = cache->store_for_vcpu(*entry, vcpu_);
  if (!store) {
    LOG_ERROR("PVFS `/`: no store for vCPU `", database, table, vcpu_);
    return -EIO;
  }
  uint64_t generation = entry->credential_generation.load();
  if (used_store) *used_store = store;
  auto ret = fn(store.get());
  if (ret != -EACCES || !retry) return ret;
  // The token may have been revoked or replaced: one new token, one retry.
  if (cache->force_refresh(database, table, generation) != 0) return ret;
  store = cache->store_for_vcpu(*entry, vcpu_);
  if (!store) return ret;
  if (used_store) *used_store = store;
  return fn(store.get());
}

int PvfsObjStore::head_object(std::string_view path, ObjHeaderMeta& meta) {
  auto t = target(path);
  if (t.pp.level != PvfsPath::SUBPATH) return -ENOENT;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->head_object(t.table_path(), meta);
  });
}

ssize_t PvfsObjStore::get_object_range(std::string_view path,
                                       const struct iovec* iov, int iovcnt,
                                       off_t offset,
                                       std::string* response_etag) {
  auto t = target(path);
  if (t.pp.level != PvfsPath::SUBPATH) return -ENOENT;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->get_object_range(t.table_path(), iov, iovcnt, offset,
                               response_etag);
  });
}

ssize_t PvfsObjStore::get_object_range_to_fd(std::string_view path, int fd,
                                             off_t fd_offset, off_t obj_offset,
                                             size_t count) {
  auto t = target(path);
  if (t.pp.level != PvfsPath::SUBPATH) return -ENOENT;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->get_object_range_to_fd(t.table_path(), fd, fd_offset,
                                     obj_offset, count);
  });
}

ssize_t PvfsObjStore::put_object(std::string_view path, const struct iovec* iov,
                                 int iovcnt, uint64_t* expected_crc64,
                                 mode_t mode, std::string* etag) {
  auto t = target(path);
  int r = write_guard(t);
  if (r != 0) return r;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->put_object(t.table_path(), iov, iovcnt, expected_crc64, mode,
                         etag);
  });
}

ssize_t PvfsObjStore::put_object_from_fd(std::string_view path, int fd,
                                         off_t offset, size_t count,
                                         uint64_t* expected_crc64,
                                         std::string* etag) {
  auto t = target(path);
  int r = write_guard(t);
  if (r != 0) return r;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->put_object_from_fd(t.table_path(), fd, offset, count,
                                 expected_crc64, etag);
  });
}

int PvfsObjStore::copy_object(std::string_view src_path,
                              std::string_view dst_path, bool overwrite,
                              bool set_mime) {
  auto src = target(src_path), dst = target(dst_path);
  int r = write_guard(src);
  if (r == 0) r = write_guard(dst);
  if (r != 0) return r;
  if (src.pp.cache_key() != dst.pp.cache_key()) return -EXDEV;
  return with_store(src.pp.database, src.pp.table, [&](IObjStore* s) {
    return s->copy_object(src.table_path(), dst.table_path(), overwrite,
                          set_mime);
  });
}

int PvfsObjStore::rename_object(std::string_view src_path,
                                std::string_view dst_path, bool set_mime,
                                bool dst_exists) {
  auto src = target(src_path), dst = target(dst_path);
  int r = write_guard(src);
  if (r == 0) r = write_guard(dst);
  if (r != 0) return r;
  if (src.pp.cache_key() != dst.pp.cache_key()) return -EXDEV;
  return with_store(src.pp.database, src.pp.table, [&](IObjStore* s) {
    return s->rename_object(src.table_path(), dst.table_path(), set_mime,
                            dst_exists);
  });
}

int PvfsObjStore::delete_object(std::string_view path) {
  auto t = target(path);
  int r = write_guard(t);
  if (r != 0) return r;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->delete_object(t.table_path());
  });
}

int PvfsObjStore::stat(std::string_view path, struct stat* buf,
                       std::string* etag) {
  auto t = target(path);
  switch (t.pp.level) {
    case PvfsPath::ROOT:
      fill_dir_stat(buf);
      return 0;
    case PvfsPath::DATABASE: {
      int r = runtime_->database_exists(t.pp.database);
      if (r != 0) return r;
      fill_dir_stat(buf);
      return 0;
    }
    case PvfsPath::TABLE: {
      int err = 0;
      if (!runtime_->table_cache()->resolve(t.pp.database, t.pp.table, &err)) {
        return err;
      }
      fill_dir_stat(buf);
      return 0;
    }
    case PvfsPath::SUBPATH:
      break;
  }
  runtime_->count_stat();
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->stat(t.table_path(), buf, etag);
  });
}

// Databases and tables come back as one page: the marker protocol of
// OssFs ends a listing when the context comes back empty. A database that
// is gone lists empty, as a missing prefix does on OSS; stat says ENOENT.
int PvfsObjStore::list_virtual(const Target& t, ObjectList& results,
                               std::string* context) {
  if (context && !context->empty()) {
    context->clear();
    return 0;
  }
  std::vector<std::string> names;
  int r = t.pp.level == PvfsPath::ROOT
              ? runtime_->rest_client()->list_databases(names)
              : runtime_->rest_client()->list_tables(t.pp.database, names);
  if (r == -ENOENT) return 0;
  if (r != 0) return r;
  results.reserve(results.size() + names.size());
  for (auto& n : names) {
    if (n.empty() || n.size() > OssFileSystem::kOssfsMaxFileNameLength ||
        n.find('/') != std::string::npos) {
      LOG_WARN("skipped catalog entry ` under `", n, t.full);
      continue;
    }
    results.emplace_back(n, 0, timespec{}, DT_DIR, "");
  }
  return 0;
}

int PvfsObjStore::list_dir(std::string_view path, ObjectList& results,
                           std::string* context) {
  auto t = target(path);
  if (t.pp.is_virtual()) return list_virtual(t, results, context);
  int r = with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->list_dir(t.table_path(), results, context);
  });
  return r == -ENOENT ? 0 : r;  // a table that is gone lists empty
}

int PvfsObjStore::check_bucket(bool /*allow_auto_create*/) {
  std::vector<std::string> names;
  int r = runtime_->rest_client()->list_databases(names);
  if (r != 0) {
    LOG_ERROR("PVFS catalog ` cannot be listed: `",
              runtime_->options().rest.catalog, r);
    return r;
  }
  if (!prefix_.empty()) {
    // A mounted sub-tree must exist: its database, table or directory.
    struct stat st;
    r = stat("/", &st, nullptr);
    if (r != 0) {
      LOG_ERROR("PVFS prefix ` of catalog ` cannot be resolved: `", prefix_,
                runtime_->options().rest.catalog, r);
      return r;
    }
  }
  return 0;
}

int PvfsObjStore::is_dir_empty(std::string_view path, bool& is_empty) {
  auto t = target(path);
  if (t.pp.is_virtual()) {
    ObjectList entries;
    int r = list_virtual(t, entries, nullptr);
    if (r != 0) return r;
    is_empty = entries.empty();
    return 0;
  }
  int r = with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->is_dir_empty(t.table_path(), is_empty);
  });
  if (r == -ENOENT) {
    is_empty = true;
    r = 0;
  }
  return r;
}

int PvfsObjStore::init_multipart_upload(std::string_view path,
                                        void** context) {
  auto t = target(path);
  int r = write_guard(t);
  if (r != 0) return r;
  void* inner = nullptr;
  std::shared_ptr<IObjStore> origin;
  r = with_store(
      t.pp.database, t.pp.table,
      [&](IObjStore *s) {
        return s->init_multipart_upload(t.table_path(), &inner);
      },
      true, &origin);
  if (r != 0) return r;
  *context = new UploadContext{t.pp.database, t.pp.table, inner,
                               std::move(origin), executor_};
  return 0;
}

ssize_t PvfsObjStore::upload_part(void* context, const struct iovec* iov,
                                  int iovcnt, int part_number,
                                  uint64_t* expected_crc64) {
  auto* ctx = static_cast<UploadContext*>(context);
  return with_store(ctx->database, ctx->table, [&](IObjStore *s) -> ssize_t {
    if (!ctx->same_location(s)) return -ESTALE;
    return s->upload_part(ctx->inner, iov, iovcnt, part_number,
                          expected_crc64);
  });
}

ssize_t PvfsObjStore::upload_part_from_fd(void* context, int fd, off_t offset,
                                          size_t count, int part_number,
                                          uint64_t* expected_crc64) {
  auto* ctx = static_cast<UploadContext*>(context);
  return with_store(ctx->database, ctx->table, [&](IObjStore *s) -> ssize_t {
    if (!ctx->same_location(s)) return -ESTALE;
    return s->upload_part_from_fd(ctx->inner, fd, offset, count, part_number,
                                  expected_crc64);
  });
}

int PvfsObjStore::upload_part_copy(void* context, off_t offset, size_t count,
                                   int part_number, uint64_t* crc64_out) {
  auto* ctx = static_cast<UploadContext*>(context);
  return with_store(ctx->database, ctx->table, [&](IObjStore *s) {
    if (!ctx->same_location(s)) return -ESTALE;
    return s->upload_part_copy(ctx->inner, offset, count, part_number,
                               crc64_out);
  });
}

// The OSS context is freed by complete and abort alike, so the wrapper
// goes with it whatever the result.
int PvfsObjStore::complete_multipart_upload(void* context,
                                            uint64_t* expected_crc64,
                                            std::string* etag) {
  std::unique_ptr<UploadContext> ctx(static_cast<UploadContext*>(context));
  int r = with_store(
      ctx->database, ctx->table,
      [&](IObjStore *s) {
        // The upload ID and object key belong to the original location.
        if (!ctx->same_location(s)) return -ESTALE;
        void *inner = ctx->inner;
        ctx->inner = nullptr;  // OSS complete consumes it even on failure.
        return s->complete_multipart_upload(inner, expected_crc64, etag);
      },
      false);
  if (ctx->inner) ctx->abort();
  return r;
}

int PvfsObjStore::abort_multipart_upload(void* context) {
  std::unique_ptr<UploadContext> ctx(static_cast<UploadContext*>(context));
  return ctx->abort();
}

int PvfsObjStore::delete_objects_under_dir(
    std::string_view path, const std::vector<std::string_view>& objects) {
  auto t = target(path);
  int r = write_guard(t);
  if (r != 0) return r;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->delete_objects_under_dir(t.table_path(), objects);
  });
}

// Only directory renames list descendants; a table or a database cannot
// be moved through the mount.
int PvfsObjStore::list_dir_descendants(std::string_view path,
                                       std::vector<std::string>& results,
                                       std::function<bool()> checker,
                                       bool* is_dirobj) {
  auto t = target(path);
  if (t.pp.level != PvfsPath::SUBPATH) return -EPERM;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->list_dir_descendants(t.table_path(), results, checker,
                                   is_dirobj);
  });
}

int PvfsObjStore::truncate_object(std::string_view path, size_t to_size) {
  auto t = target(path);
  int r = write_guard(t);
  if (r != 0) return r;
  return with_store(t.pp.database, t.pp.table, [&](IObjStore* s) {
    return s->truncate_object(t.table_path(), to_size);
  });
}

}  // namespace PvfsFileSystem
