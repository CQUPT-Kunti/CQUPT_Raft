# Java 调用上传 / 下载 RPC 说明

这份文档只说一件事：

- 服务启动后，Java 客户端怎么直接调用 RPC 做上传和下载

不讲内部实现，不讲 C++ 编排细节，只讲：

- 调哪个 RPC
- 按什么顺序调
- 每个 RPC 传什么参数

---

## 1. 先说结论

当前系统没有一个单独的：

- `UploadObject(...)`
- `DownloadObject(...)`

这种“一次 RPC 完成整文件上传/下载”的接口。

所以 Java 客户端要自己按顺序调用 RPC。

上传顺序：

1. `MetadataService.CreateObject`
2. `StorageNodeService.WriteChunk`，按 chunk 循环调用
3. `MetadataService.CommitObject`

下载顺序：

1. `MetadataService.HeadObject`
2. `StorageNodeService.ReadChunk`，按 chunk 循环调用

---

## 2. 上传怎么调用

## 2.1 第一步：调用 `CreateObject`

RPC：

- `MetadataService.CreateObject`

proto 文件：

- [metadata.proto](/home/yangjilei/Code/C++/CQUPT_Raft/proto/metadata.proto:8)

Java 里要传的参数：

- `request_id`
  - 请求唯一 ID
  - 必传
- `bucket`
  - bucket 名
  - 必传
- `object_key`
  - 对象 key，例如 `test/a.zip`
  - 必传
- `object_id`
  - 对象内部 ID
  - 必传
- `version`
  - 对象版本
  - 必传
- `size`
  - 整个文件总大小
  - 必传
- `etag`
  - 整个文件摘要
  - 建议传
- `client_time_unix_ms`
  - 客户端时间戳
  - 建议传

Java 侧理解：

- 这一步只是先在 metadata 里创建对象记录
- 还没有真正上传 chunk 数据

---

## 2.2 第二步：循环调用 `WriteChunk`

RPC：

- `StorageNodeService.WriteChunk`

proto 文件：

- [storage_node.proto](/home/yangjilei/Code/C++/CQUPT_Raft/proto/storage_node.proto:172)

你需要先把文件切成多个 chunk。

每个 chunk 都要调用一次或多次 `WriteChunk`。

Java 里每次调用要传：

- `request_id`
  - 本次 chunk 写请求 ID
  - 必传
- `chunk_id`
  - 这个 chunk 的唯一 ID
  - 必传
- `object_id`
  - 所属对象 ID
  - 必传
- `version`
  - 对象版本
  - 必传
- `chunk_index`
  - chunk 序号，从 `0` 开始
  - 必传
- `offset`
  - 这个 chunk 在整个文件里的起始偏移
  - 必传
- `expected_size`
  - 这个 chunk 的大小
  - 必传
- `expected_checksum`
  - 这个 chunk 的 checksum
  - 建议传
- `payload`
  - 这个 chunk 的二进制内容
  - 必传
- `timeout_ms`
  - 超时时间
  - 建议传
- `best_effort_cancel`
  - 可选
- `durability`
  - 传 `WRITE_CHUNK_DURABILITY_PUBLISH`
  - 必传

Java 侧理解：

- 一个 chunk 对一个 storage 节点，是一次 `WriteChunk`
- 如果你要做多副本，就对多个 storage 节点分别调 `WriteChunk`
- `WriteChunk` 成功，只表示这个 chunk 写到某个 storage 节点成功
- 不等于整个对象上传完成

---

## 2.3 第三步：调用 `CommitObject`

RPC：

- `MetadataService.CommitObject`

proto 文件：

- [metadata.proto](/home/yangjilei/Code/C++/CQUPT_Raft/proto/metadata.proto:10)

Java 里要传的参数：

- `request_id`
  - 请求 ID
  - 必传
- `bucket`
  - bucket 名
  - 必传
- `object_key`
  - 对象 key
  - 必传
- `object_id`
  - 对象 ID
  - 必传
- `version`
  - 对象版本
  - 必传
- `size`
  - 整个对象大小
  - 必传
- `etag`
  - 对象摘要
  - 建议传
- `chunks`
  - 必传
  - 这里要放所有成功写入后的 chunk 信息
- `client_time_unix_ms`
  - 建议传

这里的 `chunks` 你可以理解成：

- 每个 chunk 的 `chunk_id`
- `chunk_index`
- `size`
- `checksum`
- 成功写入了哪些 storage 节点

Java 侧理解：

- 只有 `CommitObject` 成功，这个对象才真正上传完成
- 所以前面 `WriteChunk` 都成功了，也还不能立刻对外说“上传成功”

---

## 3. 下载怎么调用

## 3.1 第一步：调用 `HeadObject`

RPC：

- `MetadataService.HeadObject`

proto 文件：

- [metadata.proto](/home/yangjilei/Code/C++/CQUPT_Raft/proto/metadata.proto:13)

Java 里要传的参数：

- `bucket`
  - bucket 名
  - 必传
- `object_key`
  - 对象 key
  - 必传
- `object_id`
  - 对象 ID
  - 可选
- `version`
  - 对象版本
  - 可选

Java 侧理解：

- 先确认对象是否存在
- 先拿到对象的基础元信息

---

## 3.2 第二步：循环调用 `ReadChunk`

RPC：

- `StorageNodeService.ReadChunk`

proto 文件：

- [storage_node.proto](/home/yangjilei/Code/C++/CQUPT_Raft/proto/storage_node.proto:190)

Java 里每次调用要传：

- `request_id`
  - 请求 ID
  - 必传
- `chunk_id`
  - 要读取的 chunk ID
  - 必传
- `object_id`
  - 所属对象 ID
  - 必传
- `version`
  - 对象版本
  - 必传
- `chunk_index`
  - chunk 序号
  - 必传
- `offset`
  - 读取偏移
  - 必传
- `length`
  - 本次读取长度
  - 必传
- `expected_checksum`
  - 期望 checksum
  - 建议传
- `timeout_ms`
  - 超时时间
  - 建议传
- `best_effort_cancel`
  - 可选
- `verify_checksum`
  - 是否要求校验 checksum
  - 建议传 `true`

Java 侧理解：

- 你要按 chunk 顺序把内容读回来
- 然后自己在本地按 `offset` 拼成完整文件

---

## 4. Java 侧最简单的调用顺序

## 4.1 上传

Java 客户端最简单按这个顺序做：

1. 调 `CreateObject`
2. 本地把文件按固定 chunk 大小切分
3. 每个 chunk 调 `WriteChunk`
4. 收集所有成功写入的 chunk 信息
5. 调 `CommitObject`

一句话理解：

- `CreateObject -> WriteChunk -> CommitObject`

---

## 4.2 下载

Java 客户端最简单按这个顺序做：

1. 调 `HeadObject`
2. 确认对象存在
3. 按 chunk 顺序循环调 `ReadChunk`
4. 把每个 chunk 写到本地文件对应位置
5. 最后做一次整文件 checksum 校验

一句话理解：

- `HeadObject -> ReadChunk`

---

## 5. 你最常用的字段

如果你后面自己写 Java gRPC 客户端，最常用、最重要的字段就是这些：

- `request_id`
- `bucket`
- `object_key`
- `object_id`
- `version`
- `chunk_id`
- `chunk_index`
- `offset`
- `expected_size`
- `expected_checksum`
- `payload`
- `length`

---

## 6. 最后一句话

如果你只想记最简单版本：

上传：

- `CreateObject`
- `WriteChunk`
- `CommitObject`

下载：

- `HeadObject`
- `ReadChunk`

如果你愿意，我下一步可以直接继续给你补一版：

- Java gRPC 上传示例代码
- Java gRPC 下载示例代码

这样你可以直接复制到 Java 客户端里改。喵!
