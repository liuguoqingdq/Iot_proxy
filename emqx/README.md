# EMQX MQTT -> Kafka Bridge

设备接入改为 MQTT 后，推荐链路：

```text
IoT device -> EMQX -> Kafka topic iota.edge.raw.v1
  -> business_service -> MySQL proxy_served_device
  -> storage_service -> HDFS Parquet data files + Iceberg metadata
```

`iota_proxy` 不再负责设备 TCP 接入，可用 `--disable-tcp-ingress` 只保留节点 discovery/KCP 等服务器间职责。

## Install EMQX

Ubuntu 上可以直接运行项目里的安装脚本：

```bash
./scripts/install_emqx_ubuntu.sh
```

这个脚本使用 EMQX 官方 apt 仓库安装并启动 `emqx` 服务。手动执行时注意第一条命令必须是一整行：

```bash
curl -s https://packagecloud.io/install/repositories/emqx/emqx-enterprise5/script.deb.sh | sudo bash
sudo apt-get update
sudo apt-get install -y emqx-enterprise
sudo systemctl enable --now emqx
```

EMQX 文档页面有时写 `emqx`，但 `emqx-enterprise5` 仓库在 Ubuntu 22.04/jammy 下实际包名通常是 `emqx-enterprise`。项目脚本会自动在 `emqx` 和 `emqx-enterprise` 之间选择 apt 中可用的包。

启动后访问：

```text
http://127.0.0.1:18083
```

默认账号密码：

```text
admin / public
```

## MQTT Topic

设备发布遥测数据到：

```text
iota/{node_id}/devices/{mac}/telemetry
```

示例：

```text
iota/node-a/devices/aabbccddeeff/telemetry
```

`business_service` 会从 topic 中提取 `{mac}`，只写入 MySQL 的 `proxy_served_device`。`mac + timestamp + payload` 明细由 `storage_service` 从 Kafka 聚合成 HDFS Parquet 数据文件，并通过 `device_message` Iceberg 表组织文件元数据。

## EMQX Rule SQL

在 EMQX Dashboard 中进入 `Integration -> Rules`，创建规则，SQL 使用：

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

同内容也保存在：

```text
emqx/mqtt_to_kafka_rule.sql
```

## Kafka Sink

在规则里添加 `Kafka Producer` action：

```text
Bootstrap Hosts: 127.0.0.1:9092
Kafka Topic:     iota.edge.raw.v1
Kafka Key:       ${topic}
Timestamp:       ${timestamp}
```

Message Template 使用：

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

同内容也保存在：

```text
emqx/kafka_message_template.json
```

`base64_encode(payload)` 用来安全承载二进制 payload。当前模板按 EMQX 6.x 使用；如果使用旧版本且不支持该函数，需要把设备 payload 约束为 UTF-8 文本，并把模板改为 `"payload": "${payload}"`。

## Test

使用 MQTTX 或其他 MQTT client 发布：

```bash
mqttx pub -h 127.0.0.1 -p 1883 \
  -i device-aabbccddeeff \
  -t iota/node-a/devices/aabbccddeeff/telemetry \
  -q 1 \
  -m '{"temperature":23.5}'
```

查看 Kafka：

```bash
/tmp/kafka_2.13-3.7.0/bin/kafka-console-consumer.sh \
  --bootstrap-server 127.0.0.1:9092 \
  --topic iota.edge.raw.v1 \
  --from-beginning
```
