package mykafka

import (
	"context"
	"fmt"
	"sync"
	"time"

	appconfig "iota/business_service/config"

	"github.com/segmentio/kafka-go"
)

type KafkaClient struct {
	reader *kafka.Reader
}

var KafkaClientGlobal KafkaClient
var once sync.Once

func (kafkaClient *KafkaClient) InitKafka() error {
	cfg, err := appconfig.LoadDefault()
	if err != nil {
		return err
	}
	return kafkaClient.InitKafkaWithConfig(cfg.Kafka)
}

func (kafkaClient *KafkaClient) InitKafkaWithConfig(cfg appconfig.KafkaConfig) error {
	var initErr error
	once.Do(func() {
		if len(cfg.Brokers) == 0 || cfg.Topic == "" || cfg.GroupID == "" {
			initErr = fmt.Errorf("kafka brokers, topic, and group_id are required")
			return
		}

		kafkaClient.reader = kafka.NewReader(kafka.ReaderConfig{
			Brokers:  cfg.Brokers,
			Topic:    cfg.Topic,
			GroupID:  cfg.GroupID,
			MinBytes: cfg.MinBytes,
			MaxBytes: cfg.MaxBytes,
			MaxWait:  time.Duration(cfg.MaxWaitMS) * time.Millisecond,
		})
	})

	if initErr != nil {
		return initErr
	}
	if kafkaClient.reader == nil {
		return fmt.Errorf("kafka reader is not initialized")
	}

	fmt.Println("Kafka reader is initialized")
	return nil
}

func (kafkaClient *KafkaClient) CloseKafka() error {
	if kafkaClient.reader == nil {
		return nil
	}
	err := kafkaClient.reader.Close()
	kafkaClient.reader = nil
	if err == nil {
		fmt.Println("Kafka reader is closed")
	}
	return err
}

func (kafkaClient *KafkaClient) FetchMessage(ctx context.Context) (kafka.Message, error) {
	if kafkaClient.reader == nil {
		return kafka.Message{}, fmt.Errorf("kafka reader is not initialized")
	}
	return kafkaClient.reader.FetchMessage(ctx)
}

func (kafkaClient *KafkaClient) CommitMessage(ctx context.Context, message kafka.Message) error {
	if kafkaClient.reader == nil {
		return fmt.Errorf("kafka reader is not initialized")
	}
	return kafkaClient.reader.CommitMessages(ctx, message)
}
