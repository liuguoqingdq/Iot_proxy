# business_service 环境配置

## Go

当前已在用户目录安装 Go：

```bash
/home/liuguoqing/.local/sdk/go/bin/go version
```

`~/.bashrc` 已加入：

```bash
export GOROOT="$HOME/.local/sdk/go"
export GOPATH="$HOME/go"
export PATH="$GOROOT/bin:$GOPATH/bin:$PATH"
```

新终端会自动生效；当前终端可以手动执行：

```bash
source ~/.bashrc
```

Go module 环境已配置：

```bash
go env GOPROXY GOSUMDB GO111MODULE GOPATH GOROOT
```

当前依赖已写入 `go.mod`：

- Gorm: `gorm.io/gorm`
- Gorm MySQL driver: `gorm.io/driver/mysql`
- Kafka client: `github.com/segmentio/kafka-go`

## 配置文件

默认配置在：

```text
config/config.json
```

本地私有配置可以复制一份：

```bash
cp config/config.json config/config.local.json
```

`config/config.local.json` 已加入 `.gitignore`，适合放 MySQL/Kafka 私有配置。

## MySQL

当前配置按本机 MySQL 预留：

```json
"mysql": {
  "host": "127.0.0.1",
  "port": 3306,
  "user": "root",
  "password": "",
  "database": "iota_business"
}
```

本机当前没有检测到 `mysql` 或 `mysqld` 命令。如果需要本机 MySQL，需要系统安装：

```bash
sudo apt update
sudo apt install -y mysql-server mysql-client
```

创建业务库示例：

```bash
mysql -u root -p
CREATE DATABASE iota_business DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
```

MySQL 现在只保存本代理服务过哪些设备，不再保存 `mac + timestamp + payload` 明细。`proxy_served_device` 中 `mac_addr` 唯一，`timestamp` 保存最近一次收到该设备消息的设备时间戳：

```bash
mysql -u root -p iota_business < database/schema.sql
```

## Kafka

Kafka consumer 配置：

```json
"kafka": {
  "brokers": ["127.0.0.1:9092"],
  "topic": "iota.edge.raw.v1",
  "group_id": "iota-business-service",
  "min_bytes": 1,
  "max_bytes": 10485760,
  "max_wait_ms": 1000
}
```

`business_service` 现在从 Kafka consumer group 拉取消息，只 upsert `proxy_served_device`，成功后提交 offset。

## MinIO + Iceberg

设备明细数据由独立的 `storage_service` 从同一个 Kafka topic 拉取，按微批聚合成 MinIO 上的 Parquet 对象。Iceberg 表只负责组织这些文件的 metadata、manifest、snapshot 和分区信息，方便其他服务端按表查询。配置在 `../storage_service/config/config.json`：

```json
{
  "minio": {
    "endpoint": "http://127.0.0.1:9000",
    "region": "us-east-1",
    "access_key_id": "minioadmin",
    "secret_access_key": "minioadmin",
    "bucket": "iota",
    "use_path_style": true
  },
  "iceberg": {
    "namespace": "default",
    "table": "device_message",
    "warehouse": "s3://iota/warehouse/iota"
  },
  "writer": {
    "batch_max_messages": 50000,
    "flush_interval_ms": 60000,
    "target_file_size_bytes": 134217728
  }
}
```

启动 `storage_service`：

```bash
../scripts/start_iceberg_ingest.sh
```

## Redis

Redis 不再作为 business_service 的主消费队列。当前 Redis 职责由 `iota_proxy` 侧维护：

```text
device:{mac_hex}:state
device:{mac_hex}:latest
seen:{message_id}
```
