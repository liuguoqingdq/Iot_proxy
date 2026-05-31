package config

import (
	"encoding/json"
	"fmt"
	"net"
	"net/url"
	"os"
	"strconv"
)

const (
	defaultConfigPath = "config/config.json"
	configPathEnv     = "IOTA_BUSINESS_CONFIG"
)

type Config struct {
	MySQL MySQLConfig `json:"mysql"`
	Kafka KafkaConfig `json:"kafka"`
}

type MySQLConfig struct {
	Host      string `json:"host"`
	Port      int    `json:"port"`
	User      string `json:"user"`
	Password  string `json:"password"`
	Database  string `json:"database"`
	Charset   string `json:"charset"`
	ParseTime bool   `json:"parse_time"`
	Loc       string `json:"loc"`
}

type KafkaConfig struct {
	Brokers   []string `json:"brokers"`
	Topic     string   `json:"topic"`
	GroupID   string   `json:"group_id"`
	MinBytes  int      `json:"min_bytes"`
	MaxBytes  int      `json:"max_bytes"`
	MaxWaitMS int      `json:"max_wait_ms"`
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
	return &cfg, nil
}

func LoadDefault() (*Config, error) {
	return Load("")
}

func (cfg *Config) setDefaults() {
	if cfg.MySQL.Port == 0 {
		cfg.MySQL.Port = 3306
	}
	if cfg.MySQL.Charset == "" {
		cfg.MySQL.Charset = "utf8mb4"
	}
	if cfg.MySQL.Loc == "" {
		cfg.MySQL.Loc = "Local"
	}
	if len(cfg.Kafka.Brokers) == 0 {
		cfg.Kafka.Brokers = []string{"127.0.0.1:9092"}
	}
	if cfg.Kafka.Topic == "" {
		cfg.Kafka.Topic = "iota.edge.raw.v1"
	}
	if cfg.Kafka.GroupID == "" {
		cfg.Kafka.GroupID = "iota-business-service"
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
}

func (cfg MySQLConfig) Address() string {
	return net.JoinHostPort(cfg.Host, strconv.Itoa(cfg.Port))
}

func (cfg MySQLConfig) DSN() string {
	parseTime := "False"
	if cfg.ParseTime {
		parseTime = "True"
	}

	return fmt.Sprintf(
		"%s:%s@tcp(%s)/%s?charset=%s&parseTime=%s&loc=%s",
		cfg.User,
		cfg.Password,
		cfg.Address(),
		cfg.Database,
		cfg.Charset,
		parseTime,
		url.QueryEscape(cfg.Loc),
	)
}
