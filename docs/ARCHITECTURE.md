# 架构说明

## 进程与线程

- **网络线程（1）**：Libevent `event_base` 接受连接、解析帧、鉴权、文件元数据与传输调度。对应“单线程处理大量并发连接”。
- **MySQL 连接池 / Redis 单上下文**：元数据、会话与对象引用计数。

## 存储布局

```text
$storage_root/
  objects/ab/abcd...md5     全局只读对象（chmod 0444），按 MD5 内容寻址
  users/<username>/...      用户目录树（仅目录结构；文件内容在 objects/）
  tmp/<username>/*.part     分块上传的临时文件，用于断点续传
```

相同内容的文件在 `objects/` 中**只存一份**。每个用户的逻辑路径通过 `file_index`（MySQL）和 Redis 引用集合 `cs:refs:<md5>` 指向该对象。

## 引用计数秒传

```text
客户端                         服务端
  |  只读本地文件，算 MD5
  |  instant_query(md5,size) ->  查 Redis/对象库
  |  <- hit + challenge(offset,len)
  |  再读本地 [offset,len)，算 proof_md5
  |  instant_upload(path, proof)
  |  <- 立即成功（status=ready）
  |                              addRef(md5, user, path)
  |                              file_index 登记，不拷贝物理文件
```

- **秒传**：`addRef`，多用户共享同一 `objects/<md5>`。
- **普通上传**：`.part` 校验后 `publishObject` + `addRef`，不在 `users/` 落盘文件副本。
- **删除**：`removeRef`；引用数为 0 时 GC 删除 `objects/` 中的对象。
- **下载**：始终通过 `user_id + virtual_path` 查 md5，从 `objects/` 只读打开。

## 路径安全

`PathGuard::resolve`：

1. 规范化逻辑路径，`..` 试图越过根目录直接拒绝。  
2. `realpath` 用户根。  
3. 逐级 `lstat`，符号链接解析后必须仍落在用户根内。  
4. 物理路径必须 `== root` 或前缀为 `root/`（避免 `/data/user` 匹配 `/data/user2`）。

客户端永远不能指定 `objects/` 路径；对象库不在用户逻辑命名空间内。

## 首次上传

分块写入 `.part` → 整文件 MD5 与客户端声明比对 → 发布到对象库 → `addRef` 写入索引。之后其他用户对同一内容即可秒传（仅增加引用，不重复占盘）。
