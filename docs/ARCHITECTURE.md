# 架构说明

## 进程与线程

- **网络线程（1）**：Libevent `event_base` 接受连接、解析帧、鉴权、投递文件任务。对应“单线程处理大量并发连接”。
- **拷贝线程池（N）**：仅用于秒传命中后，把对象库内容写入**当前用户自己的目录**。不占用事件循环。
- **MySQL 连接池 / Redis 单上下文**：元数据与会话。

## 存储布局

```text
$storage_root/
  objects/ab/abcd...md5     全局只读对象（chmod 0444），秒传的数据源
  users/<username>/...      该用户的独立文件树，路径边界校验的根
  tmp/<username>/*.part     分块上传的临时文件，用于断点续传
```

用户 A 与用户 B 即使内容相同，目录里也是两份独立 inode。对象库只作为内部缓存与秒传来源，客户端永远不能指定 `objects/` 路径。

## 秒传时序

```text
客户端                         服务端
  |  只读本地文件，算 MD5
  |  instant_query(md5,size) ->  查 Redis/对象库
  |  <- hit + challenge(offset,len)
  |  再读本地 [offset,len)，算 proof_md5
  |  instant_upload(path, proof)
  |  <- 立即成功（status=copying，已经能 get）
  |                              后台线程：objects -> users/me/path
  |                              完成后 status=ready
```

下载在 `copying` 阶段打开对象库只读文件；`ready` 后打开用户目录中的独立副本。

## 路径安全

`PathGuard::resolve`：

1. 规范化逻辑路径，`..` 试图越过根目录直接拒绝。  
2. `realpath` 用户根。  
3. 逐级 `lstat`，符号链接解析后必须仍落在用户根内。  
4. 物理路径必须 `== root` 或前缀为 `root/`（避免 `/data/user` 匹配 `/data/user2`）。

## 首次上传

分块写入 `.part` → 整文件 MD5 与客户端声明比对 → 发布到对象库 → 同时拷入用户目录（`ready`）。之后其他用户对同一内容即可秒传。
