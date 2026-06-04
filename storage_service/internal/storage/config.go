package storage

import (
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"strings"
)

const (
	defaultConfigPath = "config/config.json"
	configPathEnv     = "IOTA_STORAGE_CONFIG"
)

type Config struct {
	Kafka   KafkaConfig   `json:"kafka"`
	MinIO   MinIOConfig   `json:"minio"`
	Iceberg IcebergConfig `json:"iceberg"`
	Writer  WriterConfig  `json:"writer"`
}

type KafkaConfig struct {
	Brokers   []string `json:"brokers"`
	Topic     string   `json:"topic"`
	GroupID   string   `json:"group_id"`
	MinBytes  int      `json:"min_bytes"`
	MaxBytes  int      `json:"max_bytes"`
	MaxWaitMS int      `json:"max_wait_ms"`
}

type MinIOConfig struct {
	Endpoint        string `json:"endpoint"`
	Region          string `json:"region"`
	AccessKeyID     string `json:"access_key_id"`
	SecretAccessKey string `json:"secret_access_key"`
	Bucket          string `json:"bucket"`
	UsePathStyle    bool   `json:"use_path_style"`
}

type IcebergConfig struct {
	Namespace     string `json:"namespace"`
	Table         string `json:"table"`
	Warehouse     string `json:"warehouse"`
	TableLocation string `json:"table_location"`
}

type WriterConfig struct {
	BatchMaxMessages    int   `json:"batch_max_messages"`
	BatchMaxBytes       int64 `json:"batch_max_bytes"`
	FlushIntervalMS     int   `json:"flush_interval_ms"`
	TargetFileSizeBytes int64 `json:"target_file_size_bytes"`
}

func Load(path string) (*Config, error) {
	if path == "" {
		path = os.Getenv(configPathEnv)
	}
	if path == "" {
		path = defaultConfigPath
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config %s: %w", path, err)
	}

	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("parse config %s: %w", path, err)
	}
	cfg.setDefaults()
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	return &cfg, nil
}

func (cfg *Config) setDefaults() {
	if len(cfg.Kafka.Brokers) == 0 {
		cfg.Kafka.Brokers = []string{"127.0.0.1:9092"}
	}
	if cfg.Kafka.Topic == "" {
		cfg.Kafka.Topic = "iota.edge.raw.v1"
	}
	if cfg.Kafka.GroupID == "" {
		cfg.Kafka.GroupID = "iota-storage-service"
	}
	if cfg.Kafka.MinBytes <= 0 {
		cfg.Kafka.MinBytes = 1
	}
	if cfg.Kafka.MaxBytes <= 0 {
		cfg.Kafka.MaxBytes = 10 * 1024 * 1024
	}
	if cfg.Kafka.MaxWaitMS <= 0 {
		cfg.Kafka.MaxWaitMS = 1000
	}
	if cfg.Iceberg.Namespace == "" {
		cfg.Iceberg.Namespace = "default"
	}
	if cfg.Iceberg.Table == "" {
		cfg.Iceberg.Table = "device_message"
	}
	if cfg.MinIO.Endpoint == "" {
		cfg.MinIO.Endpoint = "http://127.0.0.1:9000"
	}
	if cfg.MinIO.Region == "" {
		cfg.MinIO.Region = "us-east-1"
	}
	if cfg.MinIO.Bucket == "" {
		cfg.MinIO.Bucket = "iota"
	}
	if cfg.MinIO.AccessKeyID == "" {
		cfg.MinIO.AccessKeyID = "minioadmin"
	}
	if cfg.MinIO.SecretAccessKey == "" {
		cfg.MinIO.SecretAccessKey = "minioadmin"
	}
	if !cfg.MinIO.UsePathStyle {
		cfg.MinIO.UsePathStyle = true
	}
	if cfg.Iceberg.Warehouse == "" {
		cfg.Iceberg.Warehouse = "s3://iota/warehouse/iota"
	}
	if cfg.Iceberg.TableLocation == "" {
		cfg.Iceberg.TableLocation = strings.TrimRight(cfg.Iceberg.Warehouse, "/") + "/" + cfg.Iceberg.Namespace + "/" + cfg.Iceberg.Table
	}
	if cfg.Writer.BatchMaxMessages <= 0 {
		cfg.Writer.BatchMaxMessages = 50000
	}
	if cfg.Writer.BatchMaxBytes <= 0 {
		cfg.Writer.BatchMaxBytes = 128 * 1024 * 1024
	}
	if cfg.Writer.FlushIntervalMS <= 0 {
		cfg.Writer.FlushIntervalMS = 60000
	}
	if cfg.Writer.TargetFileSizeBytes <= 0 {
		cfg.Writer.TargetFileSizeBytes = 128 * 1024 * 1024
	}
}

func (cfg *Config) validate() error {
	if cfg.MinIO.Endpoint == "" {
		return fmt.Errorf("minio.endpoint is required")
	}
	if _, err := url.Parse(cfg.MinIO.Endpoint); err != nil {
		return fmt.Errorf("invalid minio.endpoint: %w", err)
	}
	if cfg.MinIO.Bucket == "" {
		return fmt.Errorf("minio.bucket is required")
	}
	if cfg.MinIO.AccessKeyID == "" {
		return fmt.Errorf("minio.access_key_id is required")
	}
	if cfg.MinIO.SecretAccessKey == "" {
		return fmt.Errorf("minio.secret_access_key is required")
	}
	if cfg.Iceberg.TableLocation == "" {
		return fmt.Errorf("iceberg table location is required")
	}
	return nil
}
