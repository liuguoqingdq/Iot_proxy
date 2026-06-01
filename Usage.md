## 使用方式

[中文](#中文使用方式) | [English](#english-usage)

---

## 中文使用方式

### 1. 克隆项目

```bash
git clone https://github.com/liuguoqingdq/Iot_proxy.git
cd Iot_proxy
```

---

### 2. 准备运行环境

推荐在Ubuntu/Linux环境下运行。

安装C++代理节点依赖：

```bash
sudo apt update
sudo apt install -y cmake g++ openssl libssl-dev librdkafka-dev
```

检查Go环境：

```bash
go version
```

项目运行依赖以下组件：

```text
Redis
Kafka
MySQL
EMQX
HDFS / WebHDFS
```

如果只是进行本地快速测试，可以先启动：

```text
Redis
Kafka
MySQL
EMQX
```

HDFS和Iceberg写入服务可以后续再配置。

---

### 3. 启动Redis

```bash
sudo systemctl enable --now redis-server
```

检查Redis是否可用：

```bash
redis-cli ping
```

正常返回：

```text
PONG
```

---

### 4. 启动Kafka并创建Topic

安装Java运行环境：

```bash
sudo apt install -y openjdk-17-jre-headless
```

下载并启动Kafka：

```bash
cd /tmp
curl -LO https://archive.apache.org/dist/kafka/3.7.0/kafka_2.13-3.7.0.tgz
tar -xzf kafka_2.13-3.7.0.tgz
cd kafka_2.13-3.7.0

KAFKA_CLUSTER_ID="$(bin/kafka-storage.sh random-uuid)"
bin/kafka-storage.sh format -t "$KAFKA_CLUSTER_ID" -c config/kraft/server.properties
bin/kafka-server-start.sh config/kraft/server.properties
```

另开终端创建项目使用的Kafka Topic：

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-topics.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --create \
  --if-not-exists \
  --topic iota.edge.raw.v1 \
  --partitions 3 \
  --replication-factor 1
```

---

### 5. 初始化MySQL

创建数据库：

```sql
CREATE DATABASE IF NOT EXISTS IOTA DEFAULT CHARACTER SET utf8mb4;
```

导入表结构：

```bash
mysql -h 127.0.0.1 -P 3306 -u root -p IOTA < business_service/database/schema.sql
```

业务服务默认配置文件：

```text
business_service/config/config.json
```

如果本机MySQL用户名、密码或数据库名不同，需要修改该配置文件。

---

### 6. 安装并配置EMQX

使用项目脚本安装EMQX：

```bash
./scripts/install_emqx_ubuntu.sh
```

启动后访问EMQX Dashboard：

```text
http://127.0.0.1:18083
```

默认账号密码：

```text
admin / public
```

设备MQTT Topic格式：

```text
iota/{node_id}/devices/{mac}/telemetry
```

示例：

```text
iota/node-a/devices/aabbccddeeff/telemetry
```

在EMQX Dashboard中进入：

```text
Integration -> Rules
```

创建规则，Rule SQL使用：

```sql
SELECT
  'iota.edge.raw.v1' AS schema,
  'mqtt' AS ingress_protocol,
  topic,
  clientid,
  username,
  qos,
  timestamp,
  id AS mqtt_message_id,
  base64_encode(payload) AS payload_base64
FROM
  "iota/+/devices/+/telemetry"
```

同样的SQL文件保存在：

```text
emqx/mqtt_to_kafka_rule.sql
```

然后添加`Kafka Producer` Action：

```text
Bootstrap Hosts: 127.0.0.1:9092
Kafka Topic:     iota.edge.raw.v1
Kafka Key:       ${topic}
Timestamp:       ${timestamp}
```

Message Template使用：

```json
{
  "schema": "${schema}",
  "ingress_protocol": "${ingress_protocol}",
  "topic": "${topic}",
  "clientid": "${clientid}",
  "username": "${username}",
  "qos": ${qos},
  "timestamp": ${timestamp},
  "mqtt_message_id": "${mqtt_message_id}",
  "payload_base64": "${payload_base64}"
}
```

模板文件保存在：

```text
emqx/kafka_message_template.json
```

---

### 7. 检查项目配置

C++代理节点配置：

```text
iota_proxy/config/edge_pipeline.json
```

业务服务配置：

```text
business_service/config/config.json
```

存储服务配置：

```text
storage_service/config/config.json
```

如果Redis、Kafka、MySQL或HDFS不是部署在本机，需要修改这些配置文件中的地址。

---

### 8. 构建项目

在项目根目录执行：

```bash
./scripts/build_all.sh
```

构建完成后会生成：

```text
iota_proxy/build/iota_proxy
business_service/demo
storage_service/storage_service
```

---

### 9. 启动代理节点和业务服务

启动前确认Redis、Kafka、MySQL和EMQX已经运行。

```bash
./scripts/start_all.sh
```

默认启动：

```text
iota_proxy
business_service
```

默认情况下，`iota_proxy`会关闭旧TCP设备入口，只保留服务器间discovery/KCP职责。

日志文件：

```text
logs/iota_proxy.log
logs/business_service.log
```

查看日志：

```bash
tail -f logs/iota_proxy.log
tail -f logs/business_service.log
```

停止服务：

```text
Ctrl+C
```

---

### 10. 启动HDFS+Iceberg写入服务

如果已经配置好HDFS/WebHDFS，可以启动存储服务：

```bash
./scripts/start_iceberg_ingest.sh
```

该服务会读取：

```text
storage_service/config/config.json
```

也可以通过环境变量指定配置文件：

```bash
export IOTA_STORAGE_CONFIG=/path/to/config.json
./storage_service/storage_service
```

---

### 11. 发送MQTT测试数据

使用MQTTX CLI：

```bash
mqttx pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

或者使用`mosquitto_pub`：

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

---

### 12. 验证Kafka消息

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-console-consumer.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --topic iota.edge.raw.v1 \
  --from-beginning
```

如果EMQX规则配置正确，可以看到MQTT消息被转成Kafka JSON消息。

---

### 13. 验证MySQL写入

```bash
mysql -h 127.0.0.1 -P 3306 -u root -p IOTA \
  -e "SELECT id, mac_addr, timestamp FROM proxy_served_device ORDER BY id DESC LIMIT 5;"
```

如果能看到设备MAC和时间戳，说明以下链路已经跑通：

```text
MQTT -> EMQX -> Kafka -> business_service -> MySQL
```

---

### 14. 启用兼容旧TCP设备入口

项目默认推荐MQTT接入，但仍保留旧TCP设备入口。

手动启动TCP入口：

```bash
cd iota_proxy
./build/iota_proxy \
  --tcp-host 0.0.0.0 \
  --tcp-port 7000 \
  --edge-config config/edge_pipeline.json
```

旧TCP设备帧格式：

```text
MAC(6 bytes) + timestamp_ms(8 bytes, big-endian) + data_len(4 bytes, big-endian) + payload
```

发送测试帧：

```bash
python3 -c 'import socket,struct,time; mac=bytes.fromhex("aabbccddeeff"); ts=int(time.time()*1000); payload=("iota_tcp_test_%d"%ts).encode(); frame=mac+struct.pack(">Q",ts)+struct.pack(">I",len(payload))+payload; s=socket.create_connection(("127.0.0.1",7000),timeout=3); s.sendall(frame); s.close(); print(ts)'
```

---

### 15. 多节点组网

每个`iota_proxy`节点都有自己的节点身份。先生成节点身份：

```bash
./iota_proxy/build/iota_proxy --generate-identity
```

输出示例：

```text
private=<private_key_hex>
public=<public_key_hex>
id=<node_id_hex>
```

启动节点A：

```bash
./iota_proxy/build/iota_proxy \
  --disable-tcp-ingress \
  --edge-config iota_proxy/config/edge_pipeline.json \
  --kcp-host 0.0.0.0 \
  --kcp-port 9000 \
  --private-key-hex <node_a_private_key_hex> \
  --max-kcp-links 512 \
  --max-ingress-streams 4096 \
  --max-ingress-bytes-per-second 10485760
```

启动节点B，并将节点A作为bootstrap节点：

```bash
./iota_proxy/build/iota_proxy \
  --disable-tcp-ingress \
  --edge-config iota_proxy/config/edge_pipeline.json \
  --kcp-host 0.0.0.0 \
  --kcp-port 9001 \
  --private-key-hex <node_b_private_key_hex> \
  --bootstrap-node 192.168.1.10:9000:<node_a_public_key_hex> \
  --max-kcp-links 256 \
  --max-ingress-streams 2048 \
  --max-ingress-bytes-per-second 5242880
```

也可以手动指定路由表文件：

```bash
--route-table-file /path/to/routes.json
```

---

### 16. 节点资源参数建议

小型边缘节点：

```bash
--max-kcp-links 64 \
--max-ingress-streams 512 \
--max-ingress-bytes-per-second 1048576
```

普通服务器节点：

```bash
--max-kcp-links 512 \
--max-ingress-streams 4096 \
--max-ingress-bytes-per-second 10485760
```

高性能汇聚节点：

```bash
--max-kcp-links 2048 \
--max-ingress-streams 20000 \
--max-ingress-bytes-per-second 104857600
```

不同节点可以根据自身CPU、网络、存储容量和设备数量调整参数。系统的设计目标不是强制每个节点收集完整全局数据，而是让节点在自身资源限制下尽可能多地采集数据。

---

### 17. 二次开发方式

#### 新增实时业务服务

推荐新增一个Kafka consumer group，消费：

```text
iota.edge.raw.v1
```

可以开发：

```text
设备异常检测服务
实时告警服务
设备画像服务
数据清洗服务
统计聚合服务
边缘规则引擎
```

#### 新增离线分析服务

推荐通过Iceberg表读取数据：

```text
iota.default.device_message
```

不要直接扫描HDFS目录，这样可以利用Iceberg提供的快照一致性、分区裁剪和schema演进能力。

#### 定制存储策略

可以调整：

```text
batch_max_messages
batch_max_bytes
flush_interval_ms
target_file_size_bytes
warehouse
namespace
table
```

#### 定制路由策略

可以基于以下因素扩展路由逻辑：

```text
节点负载
KCP连接数量
入口流数量
入站字节速率
失败次数
跳数
设备类型
业务优先级
```

---

## English Usage

### 1. Clone the Repository

```bash
git clone https://github.com/liuguoqingdq/Iot_proxy.git
cd Iot_proxy
```

---

### 2. Prepare the Runtime Environment

Ubuntu/Linux is recommended.

Install C++ proxy dependencies:

```bash
sudo apt update
sudo apt install -y cmake g++ openssl libssl-dev librdkafka-dev
```

Check Go:

```bash
go version
```

The project depends on the following runtime components:

```text
Redis
Kafka
MySQL
EMQX
HDFS / WebHDFS
```

For a quick local test, you can start with:

```text
Redis
Kafka
MySQL
EMQX
```

HDFS and Iceberg ingestion can be enabled later.

---

### 3. Start Redis

```bash
sudo systemctl enable --now redis-server
```

Check Redis:

```bash
redis-cli ping
```

Expected output:

```text
PONG
```

---

### 4. Start Kafka and Create Topic

Install Java runtime:

```bash
sudo apt install -y openjdk-17-jre-headless
```

Download and start Kafka:

```bash
cd /tmp
curl -LO https://archive.apache.org/dist/kafka/3.7.0/kafka_2.13-3.7.0.tgz
tar -xzf kafka_2.13-3.7.0.tgz
cd kafka_2.13-3.7.0

KAFKA_CLUSTER_ID="$(bin/kafka-storage.sh random-uuid)"
bin/kafka-storage.sh format -t "$KAFKA_CLUSTER_ID" -c config/kraft/server.properties
bin/kafka-server-start.sh config/kraft/server.properties
```

Create the Kafka topic in another terminal:

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-topics.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --create \
  --if-not-exists \
  --topic iota.edge.raw.v1 \
  --partitions 3 \
  --replication-factor 1
```

---

### 5. Initialize MySQL

Create the database:

```sql
CREATE DATABASE IF NOT EXISTS IOTA DEFAULT CHARACTER SET utf8mb4;
```

Import the schema:

```bash
mysql -h 127.0.0.1 -P 3306 -u root -p IOTA < business_service/database/schema.sql
```

Default business service config:

```text
business_service/config/config.json
```

Update the MySQL username, password, or database name if your local environment is different.

---

### 6. Install and Configure EMQX

Install EMQX with the project script:

```bash
./scripts/install_emqx_ubuntu.sh
```

Open EMQX Dashboard:

```text
http://127.0.0.1:18083
```

Default credentials:

```text
admin / public
```

MQTT topic format:

```text
iota/{node_id}/devices/{mac}/telemetry
```

Example:

```text
iota/node-a/devices/aabbccddeeff/telemetry
```

In EMQX Dashboard, go to:

```text
Integration -> Rules
```

Create a rule with the following SQL:

```sql
SELECT
  'iota.edge.raw.v1' AS schema,
  'mqtt' AS ingress_protocol,
  topic,
  clientid,
  username,
  qos,
  timestamp,
  id AS mqtt_message_id,
  base64_encode(payload) AS payload_base64
FROM
  "iota/+/devices/+/telemetry"
```

The same SQL is stored at:

```text
emqx/mqtt_to_kafka_rule.sql
```

Add a `Kafka Producer` Action:

```text
Bootstrap Hosts: 127.0.0.1:9092
Kafka Topic:     iota.edge.raw.v1
Kafka Key:       ${topic}
Timestamp:       ${timestamp}
```

Use the following message template:

```json
{
  "schema": "${schema}",
  "ingress_protocol": "${ingress_protocol}",
  "topic": "${topic}",
  "clientid": "${clientid}",
  "username": "${username}",
  "qos": ${qos},
  "timestamp": ${timestamp},
  "mqtt_message_id": "${mqtt_message_id}",
  "payload_base64": "${payload_base64}"
}
```

The template file is stored at:

```text
emqx/kafka_message_template.json
```

---

### 7. Check Project Configuration

C++ proxy config:

```text
iota_proxy/config/edge_pipeline.json
```

Business service config:

```text
business_service/config/config.json
```

Storage service config:

```text
storage_service/config/config.json
```

If Redis, Kafka, MySQL, or HDFS is not running on localhost, update the corresponding addresses in these config files.

---

### 8. Build the Project

Run from the project root:

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

### 9. Start Proxy and Business Services

Make sure Redis, Kafka, MySQL, and EMQX are running.

```bash
./scripts/start_all.sh
```

This starts:

```text
iota_proxy
business_service
```

By default, `iota_proxy` disables the legacy TCP device ingress and keeps only inter-server discovery/KCP responsibilities.

Log files:

```text
logs/iota_proxy.log
logs/business_service.log
```

Follow logs:

```bash
tail -f logs/iota_proxy.log
tail -f logs/business_service.log
```

Stop the services:

```text
Ctrl+C
```

---

### 10. Start HDFS + Iceberg Ingestion

If HDFS/WebHDFS is ready, start the storage service:

```bash
./scripts/start_iceberg_ingest.sh
```

The service reads:

```text
storage_service/config/config.json
```

You can also specify the config file with an environment variable:

```bash
export IOTA_STORAGE_CONFIG=/path/to/config.json
./storage_service/storage_service
```

---

### 11. Publish MQTT Test Data

Using MQTTX CLI:

```bash
mqttx pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

Or using `mosquitto_pub`:

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

---

### 12. Verify Kafka Messages

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-console-consumer.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --topic iota.edge.raw.v1 \
  --from-beginning
```

If the EMQX rule is configured correctly, MQTT messages will appear as Kafka JSON messages.

---

### 13. Verify MySQL Records

```bash
mysql -h 127.0.0.1 -P 3306 -u root -p IOTA \
  -e "SELECT id, mac_addr, timestamp FROM proxy_served_device ORDER BY id DESC LIMIT 5;"
```

If records appear, the following path is working:

```text
MQTT -> EMQX -> Kafka -> business_service -> MySQL
```

---

### 14. Enable Legacy TCP Device Ingress

MQTT is the recommended ingress path, but the project still keeps a legacy TCP device ingress.

Start TCP ingress manually:

```bash
cd iota_proxy
./build/iota_proxy \
  --tcp-host 0.0.0.0 \
  --tcp-port 7000 \
  --edge-config config/edge_pipeline.json
```

Legacy TCP frame format:

```text
MAC(6 bytes) + timestamp_ms(8 bytes, big-endian) + data_len(4 bytes, big-endian) + payload
```

Send a test frame:

```bash
python3 -c 'import socket,struct,time; mac=bytes.fromhex("aabbccddeeff"); ts=int(time.time()*1000); payload=("iota_tcp_test_%d"%ts).encode(); frame=mac+struct.pack(">Q",ts)+struct.pack(">I",len(payload))+payload; s=socket.create_connection(("127.0.0.1",7000),timeout=3); s.sendall(frame); s.close(); print(ts)'
```

---

### 15. Multi-node Deployment

Each `iota_proxy` node has its own node identity. Generate identity first:

```bash
./iota_proxy/build/iota_proxy --generate-identity
```

Example output:

```text
private=<private_key_hex>
public=<public_key_hex>
id=<node_id_hex>
```

Start node A:

```bash
./iota_proxy/build/iota_proxy \
  --disable-tcp-ingress \
  --edge-config iota_proxy/config/edge_pipeline.json \
  --kcp-host 0.0.0.0 \
  --kcp-port 9000 \
  --private-key-hex <node_a_private_key_hex> \
  --max-kcp-links 512 \
  --max-ingress-streams 4096 \
  --max-ingress-bytes-per-second 10485760
```

Start node B with node A as the bootstrap node:

```bash
./iota_proxy/build/iota_proxy \
  --disable-tcp-ingress \
  --edge-config iota_proxy/config/edge_pipeline.json \
  --kcp-host 0.0.0.0 \
  --kcp-port 9001 \
  --private-key-hex <node_b_private_key_hex> \
  --bootstrap-node 192.168.1.10:9000:<node_a_public_key_hex> \
  --max-kcp-links 256 \
  --max-ingress-streams 2048 \
  --max-ingress-bytes-per-second 5242880
```

You can specify the route table file manually:

```bash
--route-table-file /path/to/routes.json
```

---

### 16. Resource Tuning Examples

Small edge node:

```bash
--max-kcp-links 64 \
--max-ingress-streams 512 \
--max-ingress-bytes-per-second 1048576
```

Normal server node:

```bash
--max-kcp-links 512 \
--max-ingress-streams 4096 \
--max-ingress-bytes-per-second 10485760
```

High-capacity aggregation node:

```bash
--max-kcp-links 2048 \
--max-ingress-streams 20000 \
--max-ingress-bytes-per-second 104857600
```

Different nodes can tune these parameters according to their CPU, network, storage capacity, and device scale. The goal is not to force every node to collect complete global data, but to let each node collect as much data as possible within its own resource limits.

---

### 17. Custom Development

#### Add a Real-time Business Service

Create a new Kafka consumer group and consume:

```text
iota.edge.raw.v1
```

Possible services:

```text
Device anomaly detection
Real-time alerting
Device profiling
Data cleaning
Statistics aggregation
Edge rule engine
```

#### Add an Offline Analytics Service

Read from the Iceberg table:

```text
iota.default.device_message
```

Avoid directly scanning HDFS directories so that the service can benefit from Iceberg snapshot consistency, partition pruning, and schema evolution.

#### Customize Storage Strategy

You can tune:

```text
batch_max_messages
batch_max_bytes
flush_interval_ms
target_file_size_bytes
warehouse
namespace
table
```

#### Customize Routing Strategy

The route selection logic can be extended with:

```text
Node load
KCP link count
Ingress stream count
Ingress bandwidth
Failure count
Hop count
Device type
Business priority
```
