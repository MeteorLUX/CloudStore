# CloudStore — Linux C++ 高性能云存储（服务端 + 客户端）

针对多用户文件云存储场景的完整系统：**一个服务端 `cloud_server`，一个客户端 `cloud_client`**。  
覆盖并发接入、分块传输、断点续传、路径越权防护、MD5 完整性校验与秒传。

项目周期：2025.09 – 2025.10

## 两端分别做什么

| 程序 | 职责 |
| --- | --- |
| `cloud_server` | Libevent 单线程非阻塞接入；MySQL 鉴权；Redis 会话/对象索引；Linux 文件 API；秒传后台拷贝 |
| `cloud_client` | 登录/注册、目录操作、`put`/`get`。上传时**只读本地文件**算 MD5 和切片证明，命中则秒传 |

```text
cloud_client  --TCP 9000-->  cloud_server
                                |-- MySQL   用户 / 文件索引
                                |-- Redis   会话、对象 MD5、秒传挑战
                                |-- objects/  全局只读对象库（秒传来源）
                                `-- users/<name>/  每个用户自己的独立副本
```

## 秒传怎么工作

1. **客户端只读本地文件**：计算整文件 MD5，并按服务端下发的偏移再读一小段做持有证明。  
   网络上不传文件体。
2. **立刻可读**：服务端确认对象库里已有相同内容后，立刻在文件索引中登记。此时 `ls` / `stat` / `get` 都可以用，下载从**全局对象库只读**。
3. **后台再写入本用户目录**：工作线程把对象库中的内容**拷贝**成当前用户目录下的独立普通文件。拷贝完成后，该用户拥有自己的副本，与其他用户互不影响；对方删除自己的文件不会让你丢数据。


## 功能对照

1. **通信框架**：Libevent + `bufferevent`，非阻塞 I/O 多路复用，单事件线程处理大量连接。  
2. **混合协议**：JSON 控制面（登录、权限、目录）；二进制分块传输面（上传/下载）。  
3. **多租户安全**：MySQL 鉴权；`PathGuard` 把逻辑路径映射到用户根目录，拒绝 `..`、符号链接逃逸、跨用户路径。  
4. **文件管理**：`lstat` / `opendir` / `mkdir` / `rename` / `unlink`，递归遍历、类型识别、断点续传（`.part`）、资源关闭。  
5. **校验与秒传**：MD5 + 路径写入 Redis/MySQL；收发 MD5 比对保证完整性；秒传带随机切片证明，避免只靠哈希撞库。

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
# 得到：build/cloud_server   build/cloud_client
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

`put` 流程：读本地文件 → `instant_query` → 命中则 `instant_upload`（秒传）；否则 `upload_begin` 分块上传（按服务端返回的 `offset` 断点续传）→ `upload_end` 校验 MD5。

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
