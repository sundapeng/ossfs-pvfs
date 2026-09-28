# paimonfuse

把 Apache Paimon RestCatalog 挂载成本地文件系统的 FUSE 文件系统

[English](README.md) | 简体中文

paimonfuse 是叠加在 [ossfs2](https://github.com/aliyun/ossfs)（上游 `main` @ `9cd0c7e`，2.0.10 版）之上的一个补丁集。它把 Paimon RestCatalog（如 DLF，Data Lake Formation）挂载成本地目录树：

- `/` 与 `/<database>/` 是由 Paimon REST catalog API（`list_databases`、`list_tables`）服务的虚拟目录，任何变更都被拒绝；
- `/<database>/<table>/...` 映射到表的 OSS location。托管表用 `get_table_token` 拿到的表级 STS 访问；外表可以使用单独配置的 OSS 凭证。控制面凭证只用来签 REST 请求；
- 表目录以下就是标准的 ossfs2 引擎：PVFS 是第三个 `IObjStore` 后端（`PvfsObjStore`，与 `OssStore`、`OssHdfsStore` 并列），复用 `OssFs` 的 staged inode 缓存、流水线预读、并发 multipart 上传、随机写、内存/磁盘缓存与 metrics，并通过后端创建前检查拒绝虚拟路径及受保护路径上的创建操作。

> [!IMPORTANT]
> 挂载**默认只读**，支持 shell 工具、Python 任务与 notebook，以及对 Format Table、Object Table 数据的直接读取。写入需要显式开启（`--pvfs_allow_write`）。即使开启写入，当表根目录下的首级路径是 `snapshot`、`manifest`、`schema`、`index`、`changelog`、`statistics`、`tag`、`branch`、`consumer` 或 `bucket-*` 时，仍会拒绝变更，除非再给 `--pvfs_allow_metadata_write`。开启写入后，普通数据文件和嵌套的 bucket 目录不在此保护范围内。需要提交表元数据的变更应使用 Paimon 写入端；paimonfuse 提供底层文件访问，不提供事务性表视图。

## 仓库布局

| 分支 | 内容 |
|---|---|
| `paimonfuse`（默认） | 上游 ossfs2 + 补丁，单提交 |
| `main` | `aliyun/ossfs` `main`（`9cd0c7e`）的未改动镜像，用于跟踪与 rebase |

上游自己的 README 保留在 [original-README.md](original-README.md)。

## 获取并应用补丁

直接下载补丁文件——[compare main...paimonfuse](https://github.com/sundapeng/ossfs-pvfs/compare/main...paimonfuse)（[.patch](https://github.com/sundapeng/ossfs-pvfs/compare/main...paimonfuse.patch)、[.diff](https://github.com/sundapeng/ossfs-pvfs/compare/main...paimonfuse.diff)）——或者直接 cherry-pick：

```bash
git remote add paimonfuse https://github.com/sundapeng/ossfs-pvfs.git
git fetch paimonfuse
git checkout 9cd0c7e && git cherry-pick paimonfuse/paimonfuse
```

## 挂载

先按下方的[构建步骤](#构建)生成二进制，再创建挂载目录并运行：

```bash
mkdir -p /mnt/paimon
./build/ossfs2 mount /mnt/paimon \
  --pvfs_catalog=<catalog> \
  --pvfs_endpoint=dlfnext.<region>.aliyuncs.com \
  --pvfs_region=<region>
```

`--oss_bucket=pvfs://<catalog>` 也能选中该模式。控制面凭证来自环境变量 `PVFS_ACCESS_KEY_ID` / `PVFS_ACCESS_KEY_SECRET`，或 `--pvfs_access_key_id` / `--pvfs_access_key_secret`。挂载选项沿用开发期的 `pvfs_` 前缀。

外表可以使用 `OSS_ACCESS_KEY_ID` / `OSS_ACCESS_KEY_SECRET`（或 `--oss_access_key_id` / `--oss_access_key_secret`），并通过 `--pvfs_external_oss_endpoint` 指定 OSS 端点。不提供这些凭证时会回退到 catalog token，但该 token 不一定有权访问外表的位置。

```console
$ ls /mnt/paimon
sales  inventory
$ ls /mnt/paimon/sales/orders
data  manifest  schema  snapshot
$ cat /mnt/paimon/sales/orders/schema/schema-0
...
$ cp /mnt/paimon/sales/orders/data/bucket-0/data-*.orc .
```

## 主要选项

| 选项 | 默认 | 含义 |
|---|---|---|
| `--pvfs_catalog` | | 要挂载的 catalog（或 `--oss_bucket=pvfs://<catalog>`） |
| `--pvfs_endpoint`、`--pvfs_region` | | REST 端点与其 region（region 缺省时自动推导） |
| `--pvfs_allow_write` | `false` | 挂载可写 |
| `--pvfs_allow_metadata_write` | `false` | 可写时同时放开保留目录的变更 |
| `--pvfs_location_cache_ttl` | `300` 秒 | 表 location 缓存时间 |
| `--pvfs_credential_refresh_ahead` | `60` 秒 | 提前多久刷新表 token |
| `--pvfs_max_table_cache` | `50` | 缓存的表条目（location + token + store） |
| `--pvfs_signing_algorithm` | `auto` | `auto` / `default`（DLF4）/ `openapi`（ROA） |
| `--pvfs_external_oss_endpoint` | | 外表（用户凭证访问）的端点 |

边界：跨表 rename 返回 `EXDEV`；不支持 symlink、appendable 对象和 xattr。`flock` 由本地挂载处理，不能协调不同挂载点的写入者。两个挂载点写同一对象时后写者胜出，与 OSS 模式一致。

## 构建

与上游 ossfs2 相同——依赖全部 vendored，构建不需要外网：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTING=ON
cmake --build build -j$(nproc)
```

## 状态

实验性预览。构建后可运行不需要凭证的路径、签名、选项和本地 HTTP 测试：

```bash
./build/ossfs2-test --config_file=/dev/null \
  --gtest_filter='PvfsPathParse.*:DlfSignerGolden.*:PvfsOptions.*:PvfsLogThrottle.*:PvfsRestStubTest.*:PvfsRestResponseTest.*:PvfsObjStoreStubTest.*'
```

真实 catalog 和挂载级测试会创建、删除测试文件，必须使用专用测试库表。提交信息中的在线、FUSE 和 AddressSanitizer 结果属于此前的验证轮次，不代表每个版本都已通过 CI 验证。GitHub CMake 工作流只编译二进制，不执行测试。

## 许可证

Apache-2.0，与上游 ossfs2 相同。
