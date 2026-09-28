# paimonfuse

A FUSE filesystem for Apache Paimon RestCatalog

English | [简体中文](README_zh.md)

paimonfuse is a patch set on top of [ossfs2](https://github.com/aliyun/ossfs) (upstream `main` @ `9cd0c7e`, release 2.0.10). It mounts a Paimon RestCatalog (for example, DLF — Data Lake Formation) as a local filesystem:

- `/` and `/<database>/` are virtual directories served by the Paimon REST catalog API (`list_databases`, `list_tables`) and refuse any change;
- `/<database>/<table>/...` maps to the table's OSS location. Managed tables use a per-table STS token from `get_table_token`; external tables can use separately configured OSS credentials. The control-plane credential only signs REST calls;
- under a table directory you get the standard ossfs2 engine: PVFS is a third `IObjStore` backend (`PvfsObjStore`, alongside `OssStore` and `OssHdfsStore`). It reuses `OssFs` for the staged inode cache, pipelined readahead, concurrent multipart uploads, random writes, memory/disk cache and metrics, with a backend pre-create check for virtual and protected paths.

> [!IMPORTANT]
> The mount is **read-only by default** for shell tools, Python jobs and notebooks, and direct reads of Format Table and Object Table data. Writing is opt-in (`--pvfs_allow_write`). Even then, changes are refused when the first path component below the table root is `snapshot`, `manifest`, `schema`, `index`, `changelog`, `statistics`, `tag`, `branch`, `consumer` or `bucket-*`, unless `--pvfs_allow_metadata_write` is also given. This guard does not protect arbitrary data files or nested bucket directories once writes are enabled. Use Paimon's writers for changes that must commit table metadata; paimonfuse exposes the underlying files, not a transactional table view.

## Repository layout

| Branch | Content |
|---|---|
| `paimonfuse` (default) | upstream ossfs2 + the patch, as a single commit |
| `main` | untouched mirror of `aliyun/ossfs` `main` (`9cd0c7e`), kept for tracking and rebasing |

The upstream README is preserved at [original-README.md](original-README.md).

## Getting and applying the patch

Download it as a file — [compare main...paimonfuse](https://github.com/sundapeng/ossfs-pvfs/compare/main...paimonfuse) ([.patch](https://github.com/sundapeng/ossfs-pvfs/compare/main...paimonfuse.patch), [.diff](https://github.com/sundapeng/ossfs-pvfs/compare/main...paimonfuse.diff)) — or cherry-pick the commit directly:

```bash
git remote add paimonfuse https://github.com/sundapeng/ossfs-pvfs.git
git fetch paimonfuse
git checkout 9cd0c7e && git cherry-pick paimonfuse/paimonfuse
```

## Mounting

Build the binary using the [instructions below](#building), then create the mount directory and run:

```bash
mkdir -p /mnt/paimon
./build/ossfs2 mount /mnt/paimon \
  --pvfs_catalog=<catalog> \
  --pvfs_endpoint=dlfnext.<region>.aliyuncs.com \
  --pvfs_region=<region>
```

`--oss_bucket=pvfs://<catalog>` selects the mode as well. Control-plane credentials come from `PVFS_ACCESS_KEY_ID` / `PVFS_ACCESS_KEY_SECRET`, or the `--pvfs_access_key_id` / `--pvfs_access_key_secret` flags. The mount options keep their `pvfs_` prefix from the feature's development name.

External tables can use `OSS_ACCESS_KEY_ID` / `OSS_ACCESS_KEY_SECRET` (or `--oss_access_key_id` / `--oss_access_key_secret`), with `--pvfs_external_oss_endpoint` for their OSS endpoint. Without those credentials they fall back to the catalog token, which may not cover the external location.

```console
$ ls /mnt/paimon
sales  inventory
$ ls /mnt/paimon/sales/orders
data  manifest  schema  snapshot
$ cat /mnt/paimon/sales/orders/schema/schema-0
...
$ cp /mnt/paimon/sales/orders/data/bucket-0/data-*.orc .
```

## Key options

| Option | Default | Meaning |
|---|---|---|
| `--pvfs_catalog` | | catalog to mount (or `--oss_bucket=pvfs://<catalog>`) |
| `--pvfs_endpoint`, `--pvfs_region` | | REST endpoint and its region (region is derived if omitted) |
| `--pvfs_allow_write` | `false` | make the mount writable |
| `--pvfs_allow_metadata_write` | `false` | with writes on, also allow changes under the reserved directories |
| `--pvfs_location_cache_ttl` | `300` s | table location cache |
| `--pvfs_credential_refresh_ahead` | `60` s | refresh table tokens this far ahead of expiry |
| `--pvfs_max_table_cache` | `50` | cached table entries (location + token + store) |
| `--pvfs_signing_algorithm` | `auto` | `auto` / `default` (DLF4) / `openapi` (ROA) |
| `--pvfs_external_oss_endpoint` | | endpoint for external tables served with user credentials |

Boundaries: cross-table rename returns `EXDEV`; symlink, appendable objects and xattr are unsupported. `flock` is local to the mount and does not coordinate writers across mounts. Two mounts writing the same object resolve last-writer-wins, as in OSS mode.

## Building

Same as upstream ossfs2 — the dependencies are vendored, no network needed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTING=ON
cmake --build build -j$(nproc)
```

## Status

Experimental preview. Run the credential-free path, signing, option and local HTTP tests after building:

```bash
./build/ossfs2-test --config_file=/dev/null \
  --gtest_filter='PvfsPathParse.*:DlfSignerGolden.*:PvfsOptions.*:PvfsLogThrottle.*:PvfsRestStubTest.*:PvfsRestResponseTest.*:PvfsObjStoreStubTest.*'
```

Live catalog and mounted-filesystem tests require a dedicated test database and table; they create and delete test files. Historical live, FUSE and AddressSanitizer results in the commit message describe the earlier validation runs, not CI checks on every revision. The GitHub CMake workflow builds the binaries but does not run the tests.

## License

Apache-2.0, as upstream ossfs2.
