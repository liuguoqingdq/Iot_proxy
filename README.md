# Iota

Iota 当前闭环链路：

```text
IoT 设备 MQTT 发布
-> EMQX
-> Kafka topic
   -> business_service 作为 Kafka consumer group 消费，写入 MySQL proxy_served_device
   -> storage_service 聚合为 HDFS Parquet 数据文件，并用 Iceberg device_message 表组织元数据
```

`iota_proxy` 当前保留为服务器间代理网络节点：

```text
节点发现
KCP peer 连接
容量广播
按负载选路
跨节点复制/转发
```

兼容旧设备 TCP 接入链路仍保留，可通过 `--disable-tcp-ingress` 关闭：

```text
边缘设备 TCP 帧
-> iota_proxy 解析帧
-> Redis 维护设备状态、最新值、短期去重
-> iota_proxy 写入 Kafka topic
   -> business_service 作为 Kafka consumer group 消费，写入 MySQL proxy_served_device 表
   -> storage_service 聚合为 HDFS Parquet 数据文件，并用 Iceberg device_message 表组织元数据
```

## 数据格式

MQTT 设备发布 topic：

```text
iota/{node_id}/devices/{mac}/telemetry
```

EMQX 到 Kafka 的配置见：

```text
emqx/README.md
```

兼容旧 TCP 设备时，边缘设备通过 TCP 发送的二进制帧格式：

```text
MAC(6 bytes) + timestamp_ms(8 bytes, big-endian) + data_len(4 bytes, big-endian) + payload
```

`data_len` 只用于 TCP 粘包/拆包，不直接写入 Redis 或 MySQL。

`iota_proxy` 写入 Kafka 的 topic：

```text
iota.edge.raw.v1
```

Kafka key 为 `mac_hex`，便于同一设备按 partition 保序。Kafka value 是 JSON：

```json
{
  "schema": "iota.edge.raw.v1",
  "mac_hex": "aabbccddeeff",
  "mac_addr": "aa:bb:cc:dd:ee:ff",
  "timestamp_ms": 1780045491751,
  "stream_id": 1,
  "payload_base64": "...",
  "payload_len": 32,
  "origin": "...",
  "message_id": "...",
  "received_at_ms": 1780045491800
}
```

设备明细不再写 MySQL。`storage_service` 会按微批从 Kafka 拉取消息，聚合成 Parquet 数据文件并通过 WebHDFS 写到 HDFS；Iceberg 表 `device_message` 维护这些文件的 metadata、manifest、snapshot 和分区信息：

```text
mac_addr   = aa:bb:cc:dd:ee:ff
timestamp_ms = timestamp_ms
event_time   = timestamp_ms 对应的时间
payload      = payload
created_at   = storage_service 写入时间
```

在默认 Hadoop catalog 下，表数据和元数据都会落在 warehouse 对应路径下，例如：

```text
hdfs:///warehouse/iota/default/device_message/data      # Parquet 数据文件
hdfs:///warehouse/iota/default/device_message/metadata  # Iceberg metadata/manifest/snapshot
```

其他人自己开发的服务端应该通过 Iceberg catalog/table 读取 `iota.default.device_message`，而不是直接扫 HDFS 目录；这样可以拿到快照一致性、分区裁剪和 schema 演进能力。

`business_service` 只维护代理服务记录表 `proxy_served_device`：

```text
mac_addr  = aa:bb:cc:dd:ee:ff，唯一
timestamp = 最近一次收到该设备消息的 timestamp_ms
```

消费到 Kafka 消息后，服务会按 `mac_addr` 执行 upsert：不存在则插入，已存在则更新时间戳。设备明细由独立的 `storage_service` 使用另一个 consumer group 消费同一个 Kafka topic。

Redis 不再作为主 ingestion 队列，只做辅助状态：

```text
device:{mac_hex}:state  -> "online"，带 TTL
device:{mac_hex}:latest -> 最近一条 Kafka JSON，带 TTL
seen:{message_id}       -> "1"，SET NX EX，用于短期去重
```

限流、临时锁、路由/节点心跳缓存、查询加速缓存也应继续放 Redis；不要再用普通 KV + 随机读作为主消息队列。

## 节点准入和选路

`iota_proxy` 支持本节点自行控制可接受的数据量：

```text
--max-kcp-links <count>                 KCP peer link 上限，默认 512
--max-ingress-streams <count>           TCP ingress stream 上限，默认 4096，0 表示不限
--max-ingress-bytes-per-second <bytes>  TCP/KCP 入站字节速率上限，默认 0 表示不限
```

节点会在 discovery 控制面广播自己的能力快照：

```json
{
  "accepting_ingress": true,
  "max_kcp_links": 512,
  "current_kcp_links": 12,
  "max_ingress_streams": 4096,
  "current_ingress_streams": 128,
  "max_ingress_bytes_per_second": 10485760,
  "current_ingress_bytes_per_second": 5242880,
  "updated_at_ms": 1780045491800
}
```

路由选择会优先选择 KCP 已连接、失败少、跳数低、且负载较低的节点。满载或声明 `accepting_ingress=false` 的节点不会被选为默认下一跳。

## 配置文件

C++ 边缘数据管线配置：

```text
iota_proxy/config/edge_pipeline.json
```

包含 Redis 状态缓存和 Kafka producer 配置。CLI 仍兼容旧参数名 `--redis-config`，也可通过 `--edge-config` 指定。

Go 业务服务配置：

```text
business_service/config/config.json
```

Go 服务也支持通过环境变量指定配置文件：

```bash
export IOTA_BUSINESS_CONFIG=/path/to/config.json
```

Go HDFS/Iceberg 写入服务配置：

```text
storage_service/config/config.json
```

也可通过环境变量指定配置文件：

```bash
export IOTA_STORAGE_CONFIG=/path/to/config.json
```

关键配置包括 Kafka、WebHDFS、Iceberg 表位置和批量写入阈值：

```json
{
  "webhdfs": {
    "endpoint": "http://127.0.0.1:9870/webhdfs/v1",
    "user": "iota"
  },
  "iceberg": {
    "namespace": "default",
    "table": "device_message",
    "warehouse": "hdfs:///warehouse/iota"
  },
  "writer": {
    "batch_max_messages": 50000,
    "flush_interval_ms": 60000,
    "target_file_size_bytes": 134217728
  }
}
```

## Kafka 安装命令

如果安装时需要 `sudo` 密码，请在本机终端手动执行：

```bash
sudo apt update
sudo apt install -y librdkafka-dev openjdk-17-jre-headless
```

`librdkafka-dev` 是 C++ producer 的编译依赖。Kafka 服务端可以用 Apache Kafka 二进制包启动：

```bash
cd /tmp
curl -LO https://archive.apache.org/dist/kafka/3.7.0/kafka_2.13-3.7.0.tgz
tar -xzf kafka_2.13-3.7.0.tgz
cd kafka_2.13-3.7.0
KAFKA_CLUSTER_ID="$(bin/kafka-storage.sh random-uuid)"
bin/kafka-storage.sh format -t "$KAFKA_CLUSTER_ID" -c config/kraft/server.properties
bin/kafka-server-start.sh config/kraft/server.properties
```

另开一个终端创建 topic：

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-topics.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --create \
  --if-not-exists \
  --topic iota.edge.raw.v1 \
  --partitions 3 \
  --replication-factor 1
```

## 一键构建

```bash
./scripts/build_all.sh
```

构建产物：

```text
iota_proxy/build/iota_proxy
business_service/demo
```

## 一键启动

启动前确认 Redis、Kafka、MySQL 已运行，Kafka 中存在 `iota.edge.raw.v1` topic，并且 MySQL 中存在 `proxy_served_device` 表。

`proxy_served_device` 建表示例在：

```text
business_service/database/schema.sql
```

```bash
./scripts/start_all.sh
```

默认启动：

```text
iota_proxy 关闭设备 TCP ingress，仅保留服务器间 discovery/KCP 职责
iota_proxy KCP 监听 9000
business_service 持续消费 Kafka 并把设备连接记录写入 MySQL
```

运行日志写到：

```text
logs/iota_proxy.log
logs/business_service.log
```

另开终端启动 Kafka 到 HDFS+Iceberg 的写入服务。它会按 `writer.flush_interval_ms` 或批量阈值形成微批，把每批数据写成 HDFS 上的 Parquet 数据文件，并提交一次 Iceberg 快照：

```bash
./scripts/start_iceberg_ingest.sh
```

Iceberg 表结构也可以参考：

```text
iceberg_ingest/schema.sql
```

停止服务：

```text
Ctrl+C
```

## 端到端测试

启动后可以用下面命令发送一条测试帧：

```bash
python3 -c 'import socket,struct,time; mac=bytes.fromhex("aabbccddeeff"); ts=int(time.time()*1000); payload=("iota_tcp_mysql_test_%d"%ts).encode(); frame=mac+struct.pack(">Q",ts)+struct.pack(">I",len(payload))+payload; s=socket.create_connection(("127.0.0.1",7000),timeout=3); s.sendall(frame); s.close(); print(ts)'
```

然后查询 MySQL 中的设备连接记录：

```bash
mysql -h 127.0.0.1 -P 3306 -u root -p123456 IOTA \
  -e "SELECT id, mac_addr, timestamp FROM proxy_served_device ORDER BY id DESC LIMIT 5;"
```

设备明细请通过 Iceberg catalog/table 读取 `iota.default.device_message`。本仓库只提供 WebHDFS 写入和 Iceberg 元数据组织的最小实现，具体查询服务或分析任务可以在这个表之上自行开发。
