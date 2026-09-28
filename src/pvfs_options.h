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

#include <string>

#include "pvfs/pvfs_runtime.h"

namespace PvfsFileSystem {

// PVFS mode is selected by --pvfs_catalog or --oss_bucket=pvfs://<catalog>.
bool pvfs_mode_selected(std::string* catalog = nullptr);

// Region implied by a DLF endpoint (dlfnext.<region>.aliyuncs.com or
// dlf-*-<region>-vpc.*); empty when it cannot be derived.
std::string derive_region_from_endpoint(const std::string& endpoint);

// Build the runtime options from the parsed mount flags (and the
// PVFS_ACCESS_KEY_ID/SECRET environment); the mounted sub-tree comes from
// oss_bucket_prefix. Returns -errno when unusable.
int pvfs_runtime_options_from_flags(const std::string& catalog,
                                    PvfsRuntimeOptions* opts);

}  // namespace PvfsFileSystem
