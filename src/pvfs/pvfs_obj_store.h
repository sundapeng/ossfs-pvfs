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

#pragma once

#include <memory>
#include <string>

#include "oss/obj_store.h"
#include "pvfs_path.h"
#include "pvfs_runtime.h"

namespace PvfsFileSystem {

// IObjStore over a DLF Paimon catalog: "/" and "/db" are virtual directories
// served by the REST catalog, everything from "/db/table" down is delegated
// to that table's OssStore on this vCPU. One instance per shared vCPU.
class PvfsObjStore : public OssFileSystem::IObjStore {
 public:
  using IObjStore = OssFileSystem::IObjStore;
  using ObjStoreOptions = OssFileSystem::ObjStoreOptions;
  using ObjectList = OssFileSystem::ObjectList;
  using ObjHeaderMeta = OssFileSystem::ObjHeaderMeta;
  using ObjCredentials = OssFileSystem::ObjCredentials;
  using CredsMeta = OssFileSystem::CredsMeta;

  PvfsObjStore(PvfsRuntime* runtime, int vcpu);
  ~PvfsObjStore() override = default;

  StorageBackend get_backend_type() const override {
    return StorageBackend::kOSS;
  }
  // Per-table credentials are the table cache's job.
  void set_credentials(ObjCredentials&&, const CredsMeta&) override {}
  void set_creds_refresh_handler(std::function<int(uint64_t)>) override {}
  const ObjStoreOptions& get_options() const override { return opts_; }

  int head_object(std::string_view path, ObjHeaderMeta& meta) override;
  ssize_t get_object_range(std::string_view path, const struct iovec* iov,
                           int iovcnt, off_t offset,
                           std::string* response_etag = nullptr) override;
  ssize_t get_object_range_to_fd(std::string_view path, int fd, off_t fd_offset,
                                 off_t obj_offset, size_t count) override;
  ssize_t put_object(std::string_view path, const struct iovec* iov, int iovcnt,
                     uint64_t* expected_crc64 = nullptr, mode_t mode = 0755,
                     std::string* etag = nullptr) override;
  ssize_t put_object_from_fd(std::string_view path, int fd, off_t offset,
                             size_t count, uint64_t* expected_crc64 = nullptr,
                             std::string* etag = nullptr) override;
  int open_object(std::string_view, int, mode_t,
                  OssFileSystem::RawObjHandle**) override {
    return -ENOTSUP;
  }
  int copy_object(std::string_view src_path, std::string_view dst_path,
                  bool overwrite = false, bool set_mime = false) override;
  int rename_object(std::string_view src_path, std::string_view dst_path,
                    bool set_mime = false, bool dst_exists = false) override;
  int rename_dir(std::string_view, std::string_view, bool = false) override {
    return -ENOTSUP;  // OssFs renames directories by list, copy and delete
  }
  int delete_object(std::string_view path) override;
  int stat(std::string_view path, struct stat* buf, std::string* etag) override;
  int list_dir(std::string_view path, ObjectList& results,
               std::string* context = nullptr) override;
  int check_bucket(bool allow_auto_create = true) override;
  int delete_bucket() override { return -EPERM; }
  int is_dir_empty(std::string_view path, bool& is_empty) override;
  int get_symlink(std::string_view, std::string&) override { return -ENOTSUP; }
  ssize_t put_symlink(std::string_view, std::string_view) override {
    return -ENOTSUP;
  }
  ssize_t append_object(std::string_view, const struct iovec*, int, off_t,
                        uint64_t* = nullptr, std::string* = nullptr) override {
    return -ENOTSUP;
  }
  int init_multipart_upload(std::string_view path, void** context) override;
  ssize_t upload_part(void* context, const struct iovec* iov, int iovcnt,
                      int part_number,
                      uint64_t* expected_crc64 = nullptr) override;
  ssize_t upload_part_from_fd(void* context, int fd, off_t offset, size_t count,
                              int part_number,
                              uint64_t* expected_crc64 = nullptr) override;
  int upload_part_copy(void* context, off_t offset, size_t count,
                       int part_number, uint64_t* crc64_out = nullptr) override;
  int complete_multipart_upload(void* context, uint64_t* expected_crc64,
                                std::string* etag = nullptr) override;
  int abort_multipart_upload(void* context) override;
  int delete_objects_under_dir(
      std::string_view path,
      const std::vector<std::string_view>& objects) override;
  int list_dir_descendants(std::string_view path,
                           std::vector<std::string>& results,
                           std::function<bool()> checker = nullptr,
                           bool* is_dirobj = nullptr) override;
  int truncate_object(std::string_view path, size_t to_size) override;
  // creat/mkdir/mknod/symlink fail at once at the virtual levels, under
  // reserved directories and on a read-only mount, instead of at flush.
  bool wants_create_check() const override { return true; }
  int check_create(std::string_view path) override {
    return write_guard(target(path));
  }
  int set_permission(std::string_view, mode_t) override { return -ENOTSUP; }
  int set_owner(std::string_view, uid_t, gid_t, int) override {
    return -ENOTSUP;
  }
  int set_lock(std::string_view, int64_t, int64_t, int16_t, int64_t,
               uint64_t) override {
    return -ENOTSUP;
  }
  int get_lock(std::string_view, int64_t&, int64_t&, int16_t&, int64_t&,
               uint64_t) override {
    return -ENOTSUP;
  }
  int set_xattr(std::string_view, const char*, const char*, size_t,
                int) override {
    return -ENOTSUP;
  }
  int get_xattr(std::string_view, const char*, char*, size_t) override {
    return -ENOTSUP;
  }
  int list_xattr(std::string_view, char*, size_t) override { return -ENOTSUP; }
  int remove_xattr(std::string_view, const char*) override { return -ENOTSUP; }

  PvfsRuntime* runtime() const { return runtime_; }
  int vcpu() const { return vcpu_; }

 private:
  // A mount-relative path resolved against the prefix and parsed.
  struct Target {
    PvfsPath pp;
    std::string full;  // path below the catalog root, e.g. "/db/tbl/x/"
    // The path inside the table as OssStore expects it: "/x" or "/x/".
    std::string table_path() const;
  };
  // The multipart context handed to OssFs: the table and the OSS context.
  struct UploadContext {
    std::string database;
    std::string table;
    void* inner = nullptr;
  };

  Target target(std::string_view path) const;
  // Refuses mutations of the virtual levels and of reserved Paimon
  // directories (-EPERM) and, on a read-only mount, of table contents
  // (-EROFS); 0 otherwise.
  int write_guard(const Target& t) const;
  // The table's store on this vCPU; a REST failure is returned as -errno.
  int table_store(const std::string& database, const std::string& table,
                  std::shared_ptr<IObjStore>* store);
  // Runs fn on the table's store; a 403 with a token that has since been
  // refused makes it fetch a new token and run fn once more, unless
  // `retry` is false (calls that consume their context).
  template <class F>
  auto with_store(const std::string& database, const std::string& table,
                  F&& fn, bool retry = true) -> decltype(fn((IObjStore*)nullptr));
  int list_virtual(const Target& t, ObjectList& results, std::string* context);
  static void fill_dir_stat(struct stat* buf);

  PvfsRuntime* runtime_;  // not owned
  int vcpu_;
  std::string prefix_;
  ObjStoreOptions opts_;
};

}  // namespace PvfsFileSystem
