# CloudStore — Linux C++ 高性能云存储（服务端 + 客户端）

针对多用户文件云存储场景的完整系统：**一个服务端 `cloud_server`，一个客户端 `cloud_client`**。  
覆盖并发接入、分块传输、断点续传、路径越权防护、MD5 完整性校验与**引用计数秒传**。

项目周期：2025.09 – 2025.10

## 两端分别做什么

| 程序 | 职责 |
| --- | --- |
| `cloud_server` | Libevent 单线程非阻塞接入；MySQL 鉴权；Redis 会话/对象索引/引用计数；Linux 文件 API |
| `cloud_client` | 登录/注册、目录操作、`put`/`get`。上传时**只读本地文件**算 MD5 和切片证明，命中则秒传 |

```text
cloud_client  --TCP 9000-->  cloud_server
                                |-- MySQL   用户 / 文件索引 (virtual_path -> md5)
                                |-- Redis   会话、对象元数据、cs:refs:<md5> 引用计数
                                |-- objects/  全局只读对象库（相同内容只存一份）
                                `-- users/<name>/  用户目录树（目录结构；文件内容在 objects/）
```

## 秒传怎么工作（引用计数）

1. **客户端只读本地文件**：计算整文件 MD5，并按服务端下发的偏移再读一小段做持有证明。网络上不传文件体。
2. **命中后 addRef**：服务端在 `file_index` 登记 `user_id + virtual_path -> md5`，Redis `SADD cs:refs:<md5>`，**不拷贝物理文件**。
3. **多用户共享**：A、B 上传相同内容时，`objects/<md5>` 只有一份；各自逻辑路径独立，删除时 `removeRef`，引用为 0 才 GC 对象。
4. **立刻可读**：`ls` / `stat` / `get` 通过索引查 md5，从 `objects/` 只读下载。

## 功能对照

1. **通信框架**：Libevent + `bufferevent`，非阻塞 I/O 多路复用，单事件线程处理大量连接。
2. **混合协议**：JSON 控制面（登录、权限、目录）；二进制分块传输面（上传/下载）。
3. **多租户安全**：MySQL 鉴权；`PathGuard` 把逻辑路径映射到用户根目录，拒绝 `..`、符号链接逃逸、跨用户路径。
4. **文件管理**：`lstat` / `opendir` / `mkdir` / `rename` / `unlink`，递归遍历、类型识别、断点续传（`.part`）、资源关闭。
5. **校验与秒传**：MD5 + 路径写入 Redis/MySQL；收发 MD5 比对保证完整性；秒传带随机切片证明；**引用计数去重省磁盘**。

## 依赖（Ubuntu 22.04）

```bash
sudo apt-get install -y g++ cmake pkg-config \
    libevent-dev libjsoncpp-dev libhiredis-dev libssl-dev libmysqlclient-dev \
    mysql-server redis-server
```

## 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 得到：build/cloud_server   build/cloud_client   build/cloud_gui（可选）
```

或：`bash scripts/build.sh`

## 启动

准备 MySQL / Redis 后导入 `sql/init.sql`，按需修改 `conf/server.conf`。

```bash
sudo mkdir -p /var/cloudstore/data /var/log/cloudstore
./build/cloud_server -c conf/server.conf
```

首次启动若用户表为空，会自动创建：

- `admin` / `admin123`
- `alice` / `alice123`
- `bob` / `bob123`

Docker：

```bash
docker compose up --build
```

## 客户端用法

交互：

```bash
./build/cloud_client --host 127.0.0.1 --port 9000
alice> login alice alice123
alice> mkdir /docs
alice> put ./movie.mp4 /docs/movie.mp4
alice> ls /docs
alice> get /docs/movie.mp4 ./movie2.mp4
alice> rm /docs/movie.mp4
```

一次性命令：

```bash
./build/cloud_client --host 127.0.0.1 --port 9000 \
    --user alice --pass alice123 \
    put ./movie.mp4 /docs/movie.mp4
```

`put` 流程：读本地文件 → `instant_query` → 命中则 `instant_upload`（秒传，addRef）；否则 `upload_begin` 分块上传（按服务端返回的 `offset` 断点续传）→ `upload_end` 校验 MD5 并发布到对象库。

## Web 图形界面

编译时默认构建 `cloud_gui`（`-DBUILD_GUI=OFF` 可关闭）。在项目根目录启动：

```bash
./build/cloud_gui --host 127.0.0.1 --port 9000 --listen 9080
```

浏览器打开 **http://127.0.0.1:9080**，可登录、浏览目录、上传/下载、新建文件夹、删除文件。界面文件位于 `client/web/`。

默认测试账号：`alice` / `alice123`

## 目录结构

```text
include/ src/     服务端
client/           独立客户端
conf/             配置
sql/init.sql      表结构
docs/             协议与架构说明
scripts/          编译与启动
docker-compose.yml
```

更细的协议见 [docs/PROTOCOL.md](docs/PROTOCOL.md)，架构见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。
