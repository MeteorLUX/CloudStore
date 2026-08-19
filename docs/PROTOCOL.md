# 应用层协议

传输在 TCP 上，每个帧：

```text
magic[4] = "CSTR"
version[1] = 1
type[1]    = 0 JSON 控制帧 / 1 二进制分块
flags[2]   = 大端
length[4]  = 大端，payload 字节数
payload[length]
```

单帧上限 2MiB。JSON 与分块可以在同一条连接上交错（上传时控制帧之后连续发送分块）。

## JSON 控制帧

```json
{ "cmd": "login", "seq": 1, "token": "", "data": { } }
```

成功回复：`{ "seq", "code": 0, "msg": "ok", "data": { } }`  
失败：`code` 为 HTTP 风格错误码，`msg` 为原因。

除 `login` / `register` 外需要先登录。后续请求可带 `token`，便于断线后恢复会话。

### 命令

| cmd | data | 说明 |
| --- | --- | --- |
| register | username, password | 注册并创建用户根目录 |
| login | username, password | 返回 token |
| logout | | 删除 Redis 会话 |
| ls | path, recursive | 目录遍历；秒传拷贝中的文件 `status=copying` 仍可见 |
| mkdir | path | 创建目录 |
| rm | path | 删除文件或递归目录 |
| stat | path | 类型/大小；拷贝中的文件仍可查询 |
| rename | from, to | 重命名（拷贝中禁止） |
| instant_query | md5, size | 是否可秒传，并下发切片挑战 |
| instant_upload | path, md5, size, challenge_offset, proof_md5, overwrite | 证明持有后立刻可读 |
| upload_begin | path, md5, size | 返回 file_id、已有 offset（断点续传） |
| upload_end | | 校验 MD5，写入对象库和用户目录 |
| download_begin | path | 返回 file_id、size。秒传拷贝中则从对象库只读 |
| download_chunk | file_id, offset, length | 成功时服务器回 **二进制分块帧**；结束时回 JSON `eof` |

## 分块帧 payload

```text
file_id[32 ascii hex]
offset[8 big-endian]
data[]
```

客户端 `put` 在 `upload_begin` 之后按 `offset` 起发送分块帧。  
客户端 `get` 循环发送 `download_chunk`，读取分块帧写本地文件。
