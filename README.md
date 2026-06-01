# Iot_proxy

[中文](#中文) | [English](#english)

> A decentralized distributed IoT data collection service.
> 一个面向IoT设备数据采集场景的去中心化分布式数据采集服务。

---

## 中文

### 项目简介

`Iot_proxy`是一个去中心化的分布式IoT设备数据采集服务。项目面向大量边缘设备接入场景，设备通过MQTT协议连接到服务器并上传遥测数据，服务端将数据写入Kafka数据管道，再由独立的业务服务和存储服务完成设备状态维护、HDFS落盘和Iceberg元数据组织。

项目的核心目标不是让每个节点都保存完整的全局数据，而是让每个节点在自身资源约束下尽可能多地采集、转发和沉淀数据。不同节点可以根据实际设备规模、网络条件和存储资源调整配置，例如存储容量、KCP连接数量、入口连接数量和入站速率限制等，从而形成一个可弹性扩展的分布式数据采集网络。

---

### 核心特性

* **MQTT设备接入**
  IoT设备通过MQTT发布遥测数据，推荐接入链路为`IoT Device -> EMQX -> Kafka`。

* **Kafka数据总线**
  设备数据统一写入Kafka topic，业务服务和存储服务通过不同consumer group独立消费，避免采集、业务处理和存储逻辑强耦合。

* **HDFS+Iceberg存储组织**
  存储服务将Kafka中的设备消息按微批聚合为Parquet文件写入HDFS，并使用Iceberg表管理元数据、manifest、snapshot和分区信息。

* **去中心化节点发现与路由**
  节点发现与地址管理思想借鉴Bitcoin Core addrman机制，节点之间通过bootstrap节点、路由表、能力广播和feeler探测维护可用节点集合。

* **KCP节点间传输**
  各代理节点之间使用KCP建立peer连接，用于提高节点间数据转发、广播和复制的传输效率。

* **节点能力自描述**
  每个节点会广播自身能力，包括是否接受入口流量、KCP连接上限、当前连接数、入口流数量和入站字节速率等。

* **资源约束下的最大化采集**
  每个节点都可以按照自身资源限制调整配置，不强制追求全局完整数据，而是优先提高本节点可承载范围内的数据采集数量。

* **支持二次定制开发**
  可以基于Kafka topic、Iceberg表、KCP转发链路和业务服务模块开发自定义分析服务、告警服务、数据清洗服务或设备画像服务。

---

### 整体架构

```text
IoT Device
  |
  | MQTT publish
  v
EMQX
  |
  | Rule Engine / Kafka Sink
  v
Kafka topic: iota.edge.raw.v1
  |
  +------------------------------+
  |                              |
  v                              v
business_service                 storage_service
  |                              |
  | upsert device state           | micro-batch write
  v                              v
MySQL: proxy_served_device       HDFS Parquet files
                                 +
                                 Iceberg table metadata
```

`iota_proxy`作为服务器间代理网络节点，主要承担以下职责：

```text
Node discovery
KCP peer connection
Capability broadcast
Route selection
Cross-node forwarding / replication
```

项目中仍保留兼容旧设备的TCP接入链路，默认推荐使用MQTT接入；如只希望保留服务器间discovery/KCP职责，可以启动`iota_proxy`时使用`--disable-tcp-ingress`关闭TCP入口。

---

### 模块说明

| 模块                 | 语言         | 作用                                                  |
| ------------------ | ---------- | --------------------------------------------------- |
| `iota_proxy`       | C++17      | 分布式代理节点，负责节点发现、KCP连接、路由管理、兼容TCP入口、Redis状态缓存和Kafka写入 |
| `business_service` | Go         | Kafka消费者，维护设备服务记录，将设备最近一次上报时间写入MySQL                |
| `storage_service`  | Go         | Kafka消费者，将设备消息聚合为Parquet文件写入HDFS，并通过Iceberg组织元数据    |
| `emqx`             | SQL/JSON配置 | MQTT到Kafka的规则、Kafka Sink模板和测试说明                     |
| `scripts`          | Bash       | 一键构建、启动代理服务、启动HDFS/Iceberg写入服务                      |

---

### 数据链路

#### 1. MQTT设备上报

设备发布遥测数据到：

```text
iota/{node_id}/devices/{mac}/telemetry
```

示例：

```text
iota/node-a/devices/aabbccddeeff/telemetry
```

EMQX规则会将MQTT消息转换为Kafka消息，写入：

```text
iota.edge.raw.v1
```

推荐Kafka消息结构：

```json
{
  "schema": "iota.edge.raw.v1",
  "ingress_protocol": "mqtt",
  "topic": "iota/node-a/devices/aabbccddeeff/telemetry",
  "clientid": "device-aabbccddeeff",
  "username": "device-user",
  "qos": 1,
  "timestamp": 1780045491751,
  "mqtt_message_id": "mqtt-message-id",
  "payload_base64": "..."
}
```

#### 2. 业务服务消费

`business_service`消费Kafka消息后，从topic或消息字段中提取设备标识，并维护MySQL表：

```text
proxy_served_device
```

该表只保存代理服务记录，例如：

```text
mac_addr  = aa:bb:cc:dd:ee:ff
timestamp = 最近一次收到该设备消息的时间戳
```

设备明细数据不直接写入MySQL，避免高频数据写入给关系型数据库造成过大压力。

#### 3. 存储服务写入HDFS

`storage_service`使用独立consumer group消费同一个Kafka topic，将消息按批次写成Parquet文件，并通过WebHDFS写入HDFS。

默认Iceberg表：

```text
iota.default.device_message
```

默认HDFS路径示例：

```text
hdfs:///warehouse/iota/default/device_message/data
hdfs:///warehouse/iota/default/device_message/metadata
```

二次开发的查询服务、分析服务或告警服务建议通过Iceberg catalog/table读取数据，而不是直接扫描HDFS目录。这样可以获得快照一致性、分区裁剪和schema演进能力。

---

### 去中心化节点发现与KCP网络

每个`iota_proxy`节点都拥有自己的节点身份，并通过节点发现服务维护路由表。节点之间可以通过bootstrap节点互相发现，并在条件允许时建立KCP peer连接。

节点发现控制面包括：

```text
hello handshake
signature verification
route exchange
capability broadcast
route persistence
feeler probing
```

节点能力广播示例：

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

路由选择会综合考虑：

```text
是否已经建立KCP连接
节点是否接受入口流量
KCP连接是否达到上限
入口流数量是否达到上限
入站速率是否达到上限
失败次数
跳数
最近探测状态
```

满载节点或声明`accepting_ingress=false`的节点不会被优先选为默认下一跳。

---

### 设计思想

本项目的核心思想可以概括为：

```text
在资源有限的分布式IoT场景下，优先让每个节点尽可能多地采集和保留本地可承载的数据，
而不是要求所有节点都维护完整一致的全局数据。
```

因此，系统设计更强调：

* 节点自治：每个节点可以独立配置接入、存储和转发能力；
* 弹性采集：节点根据自身资源决定能够承载多少设备数据；
* 弱全局依赖：不依赖单一中心节点维护所有设备状态；
* 数据管道解耦：MQTT、Kafka、MySQL、HDFS和Iceberg各自承担不同职责；
* 可扩展开发：后续可以在Kafka和Iceberg之上继续开发统计、告警、清洗、画像和分析服务。

---

### 配置文件

#### C++代理节点配置

```text
iota_proxy/config/edge_pipeline.json
```

示例：

```json
{
  "redis-host": "127.0.0.1",
  "redis-port": 6379,
  "redis-state-ttl-seconds": 60,
  "redis-latest-ttl-seconds": 86400,
  "redis-seen-ttl-seconds": 600,
  "kafka-brokers": "127.0.0.1:9092",
  "kafka-topic": "iota.edge.raw.v1",
  "kafka-client-id": "iota-proxy",
  "kafka-acks": "all"
}
```

#### Go业务服务配置

```text
business_service/config/config.json
```

也可以通过环境变量指定：

```bash
export IOTA_BUSINESS_CONFIG=/path/to/config.json
```

#### Go存储服务配置

```text
storage_service/config/config.json
```

也可以通过环境变量指定：

```bash
export IOTA_STORAGE_CONFIG=/path/to/config.json
```

关键配置包括Kafka、WebHDFS、Iceberg表位置和批量写入阈值。

---

### 常用启动参数

`iota_proxy`支持以下常用参数：

```text
--disable-tcp-ingress
--edge-config <path>
--kcp-host <ip>
--kcp-port <port>
--bootstrap-node <host:port:public_key>
--route-table-file <path>
--max-kcp-links <count>
--max-ingress-streams <count>
--max-ingress-bytes-per-second <bytes>
--broadcast-ttl <hops>
--broadcast-fanout <count>
--generate-identity
```

其中资源控制相关参数是项目的重点：

```text
--max-kcp-links
--max-ingress-streams
--max-ingress-bytes-per-second
```

不同节点可以根据实际设备数量、网络带宽、CPU负载和存储能力设置不同参数。

---

### 构建

依赖示例：

```bash
sudo apt update
sudo apt install -y cmake g++ openssl libssl-dev librdkafka-dev
```

Go服务需要安装Go环境。

一键构建：

```bash
./scripts/build_all.sh
```

构建产物：

```text
iota_proxy/build/iota_proxy
business_service/demo
storage_service/storage_service
```

---

### 启动

启动前请确认以下组件已经运行：

```text
Redis
Kafka
MySQL
EMQX
HDFS / WebHDFS
```

并确认Kafka中已经创建topic：

```text
iota.edge.raw.v1
```

启动代理节点和业务服务：

```bash
./scripts/start_all.sh
```

默认启动行为：

```text
iota_proxy关闭设备TCP ingress，仅保留服务器间discovery/KCP职责
iota_proxy KCP监听9000端口
business_service持续消费Kafka并写入MySQL设备服务记录
```

启动HDFS+Iceberg写入服务：

```bash
./scripts/start_iceberg_ingest.sh
```

---

### MQTT测试

可以使用MQTTX或其他MQTT客户端发布测试数据：

```bash
mqttx pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

查看Kafka消息：

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-console-consumer.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --topic iota.edge.raw.v1 \
  --from-beginning
```

---

### 二次开发方向

本项目支持在多个层面进行二次开发：

#### 1. 自定义设备接入

可以基于MQTT topic规范扩展不同类型设备的数据格式，也可以为特定设备定义payload schema。

#### 2. 自定义业务服务

可以新增Kafka consumer group，独立消费`iota.edge.raw.v1`，开发：

```text
设备在线状态服务
设备异常检测服务
数据清洗服务
实时告警服务
统计分析服务
设备画像服务
```

#### 3. 自定义存储策略

可以根据实际需求调整：

```text
微批大小
flush间隔
目标Parquet文件大小
HDFS warehouse路径
Iceberg namespace/table
```

#### 4. 自定义路由与准入策略

可以根据节点负载、网络质量、设备类型或地理位置定制节点选择策略。

#### 5. 自定义跨节点服务

可以基于KCP peer网络开发节点间复制、数据转发、远程查询、控制广播等功能。

---

### 项目定位

`Iot_proxy`更适合作为一个分布式IoT数据采集底座，而不是单一的数据分析应用。它关注的是：

```text
设备如何接入
消息如何进入数据管道
节点之间如何发现和连接
数据如何以低耦合方式落盘
不同资源节点如何尽可能多地采集数据
上层服务如何在采集底座之上继续扩展
```

---

## English

### Overview

`Iot_proxy` is a decentralized distributed IoT data collection service. It is designed for large-scale edge-device scenarios where IoT devices publish telemetry data through MQTT, the server side forwards messages into Kafka, and independent services process device states and persist raw data into HDFS with Iceberg metadata.

The core goal of this project is not to force every node to store complete global data. Instead, each node collects and retains as much data as possible within its own resource constraints. Different nodes can tune their storage capacity, KCP peer link limits, ingress stream limits, and ingress bandwidth limits according to real deployment conditions.

---

### Key Features

* **MQTT device ingress**
  IoT devices publish telemetry data through MQTT. The recommended ingestion path is `IoT Device -> EMQX -> Kafka`.

* **Kafka-based data pipeline**
  Device messages are written into a shared Kafka topic. Business and storage services consume the same topic with different consumer groups.

* **HDFS + Iceberg storage**
  The storage service writes micro-batched Parquet files into HDFS and manages metadata through an Iceberg table.

* **Decentralized node discovery and routing**
  The discovery and route management design is inspired by Bitcoin Core addrman. Nodes discover peers, exchange routes, broadcast capabilities, and maintain local route tables.

* **KCP-based inter-node transport**
  Proxy nodes use KCP peer connections to improve cross-node forwarding, broadcast, and replication efficiency.

* **Capability-aware routing**
  Each node advertises its current capacity, including KCP link limits, ingress stream limits, ingress bandwidth, and whether it accepts ingress traffic.

* **Resource-aware data collection**
  Nodes are expected to maximize local data collection under their own CPU, network, and storage constraints.

* **Custom service development**
  Developers can build custom analytics, alerting, cleaning, indexing, or device-profile services on top of Kafka and Iceberg.

---

### Architecture

```text
IoT Device
  |
  | MQTT publish
  v
EMQX
  |
  | Rule Engine / Kafka Sink
  v
Kafka topic: iota.edge.raw.v1
  |
  +------------------------------+
  |                              |
  v                              v
business_service                 storage_service
  |                              |
  | upsert device state           | micro-batch write
  v                              v
MySQL: proxy_served_device       HDFS Parquet files
                                 +
                                 Iceberg table metadata
```

`iota_proxy` acts as the inter-server proxy network node and is responsible for:

```text
Node discovery
KCP peer connection
Capability broadcast
Route selection
Cross-node forwarding / replication
```

A legacy TCP ingress path is still kept for compatibility. For MQTT-first deployments, `iota_proxy` can be started with `--disable-tcp-ingress` so that it only keeps discovery and KCP responsibilities.

---

### Modules

| Module             | Language        | Responsibility                                                                                                 |
| ------------------ | --------------- | -------------------------------------------------------------------------------------------------------------- |
| `iota_proxy`       | C++17           | Proxy node, discovery, KCP peer links, route management, legacy TCP ingress, Redis state cache, Kafka producer |
| `business_service` | Go              | Kafka consumer that maintains device service records in MySQL                                                  |
| `storage_service`  | Go              | Kafka consumer that writes Parquet files into HDFS and organizes metadata through Iceberg                      |
| `emqx`             | SQL/JSON config | MQTT-to-Kafka rule and Kafka Sink template                                                                     |
| `scripts`          | Bash            | Build and startup scripts                                                                                      |

---

### Data Flow

#### 1. MQTT Telemetry

Devices publish telemetry data to:

```text
iota/{node_id}/devices/{mac}/telemetry
```

Example:

```text
iota/node-a/devices/aabbccddeeff/telemetry
```

EMQX forwards MQTT messages into Kafka topic:

```text
iota.edge.raw.v1
```

Example Kafka payload:

```json
{
  "schema": "iota.edge.raw.v1",
  "ingress_protocol": "mqtt",
  "topic": "iota/node-a/devices/aabbccddeeff/telemetry",
  "clientid": "device-aabbccddeeff",
  "username": "device-user",
  "qos": 1,
  "timestamp": 1780045491751,
  "mqtt_message_id": "mqtt-message-id",
  "payload_base64": "..."
}
```

#### 2. Business Service

`business_service` consumes Kafka messages and maintains the latest service record of each device in MySQL:

```text
proxy_served_device
```

This table stores lightweight device service metadata, such as:

```text
mac_addr  = aa:bb:cc:dd:ee:ff
timestamp = last received timestamp
```

Raw device detail data is not directly written into MySQL.

#### 3. Storage Service

`storage_service` consumes the same Kafka topic with another consumer group, aggregates messages into micro-batches, writes Parquet files to HDFS through WebHDFS, and registers metadata in an Iceberg table.

Default Iceberg table:

```text
iota.default.device_message
```

Default HDFS paths:

```text
hdfs:///warehouse/iota/default/device_message/data
hdfs:///warehouse/iota/default/device_message/metadata
```

Custom query or analytics services should read through the Iceberg catalog/table instead of directly scanning HDFS directories.

---

### Decentralized Discovery and KCP Network

Each `iota_proxy` node owns a node identity and maintains a local route table. Nodes can discover each other through bootstrap nodes and establish KCP peer links when capacity allows.

The discovery control plane includes:

```text
hello handshake
signature verification
route exchange
capability broadcast
route persistence
feeler probing
```

Example capability snapshot:

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

Route selection considers whether the node is connected through KCP, whether it accepts ingress traffic, whether it is overloaded, its failure history, hop count, and recent probing state.

---

### Design Philosophy

The project follows this principle:

```text
In resource-constrained distributed IoT environments,
each node should collect and retain as much data as it can handle,
instead of requiring every node to maintain a complete global dataset.
```

Therefore, the system emphasizes:

* node autonomy;
* resource-aware collection;
* weak dependency on a global center;
* decoupled ingestion, processing, and storage;
* extensibility for custom upper-layer services.

---

### Configuration

C++ proxy node config:

```text
iota_proxy/config/edge_pipeline.json
```

Go business service config:

```text
business_service/config/config.json
```

Go storage service config:

```text
storage_service/config/config.json
```

The Go services also support environment variables:

```bash
export IOTA_BUSINESS_CONFIG=/path/to/config.json
export IOTA_STORAGE_CONFIG=/path/to/config.json
```

---

### Build

Install basic dependencies:

```bash
sudo apt update
sudo apt install -y cmake g++ openssl libssl-dev librdkafka-dev
```

Build all services:

```bash
./scripts/build_all.sh
```

Build outputs:

```text
iota_proxy/build/iota_proxy
business_service/demo
storage_service/storage_service
```

---

### Run

Make sure the following components are running:

```text
Redis
Kafka
MySQL
EMQX
HDFS / WebHDFS
```

Start proxy and business services:

```bash
./scripts/start_all.sh
```

Start HDFS + Iceberg ingestion service:

```bash
./scripts/start_iceberg_ingest.sh
```

---

### MQTT Test

Publish a sample MQTT message:

```bash
mqttx pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

Consume Kafka messages:

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-console-consumer.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --topic iota.edge.raw.v1 \
  --from-beginning
```

---

### Custom Development

Possible extension directions include:

* custom MQTT payload schema;
* custom Kafka consumer services;
* real-time alerting service;
* device status and profiling service;
* data cleaning and indexing service;
* custom Iceberg table schema;
* custom KCP routing and admission policies;
* cross-node replication or forwarding services.

---

### Project Positioning

`Iot_proxy` is a distributed IoT data collection foundation rather than a single-purpose analytics application. It focuses on device ingestion, decentralized node discovery, KCP-based peer networking, Kafka-based data pipelines, HDFS/Iceberg persistence, and extensibility for custom services.
