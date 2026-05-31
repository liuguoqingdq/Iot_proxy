package storage

import (
	"context"
	"errors"
	"fmt"
	"log"
	"time"

	"github.com/segmentio/kafka-go"
)

type Service struct {
	cfg    *Config
	reader *kafka.Reader
	writer *IcebergWriter
}

type messageBatch struct {
	rows     []DeviceMessage
	messages []kafka.Message
	bytes    int64
}

func NewService(cfg *Config) (*Service, error) {
	if cfg == nil {
		return nil, fmt.Errorf("config is required")
	}

	writer, err := NewIcebergWriter(cfg)
	if err != nil {
		return nil, err
	}

	reader := kafka.NewReader(kafka.ReaderConfig{
		Brokers:  cfg.Kafka.Brokers,
		Topic:    cfg.Kafka.Topic,
		GroupID:  cfg.Kafka.GroupID,
		MinBytes: cfg.Kafka.MinBytes,
		MaxBytes: cfg.Kafka.MaxBytes,
		MaxWait:  time.Duration(cfg.Kafka.MaxWaitMS) * time.Millisecond,
	})

	return &Service{
		cfg:    cfg,
		reader: reader,
		writer: writer,
	}, nil
}

func (s *Service) Close() error {
	if s == nil || s.reader == nil {
		return nil
	}
	return s.reader.Close()
}

func (s *Service) Run(ctx context.Context) error {
	if s == nil {
		return fmt.Errorf("service is nil")
	}

	var batch messageBatch
	flushInterval := time.Duration(s.cfg.Writer.FlushIntervalMS) * time.Millisecond

	for {
		fetchCtx, cancel := context.WithTimeout(ctx, flushInterval)
		message, err := s.reader.FetchMessage(fetchCtx)
		cancel()
		if err != nil {
			if errors.Is(err, context.DeadlineExceeded) && ctx.Err() == nil {
				if err := s.flush(ctx, &batch); err != nil {
					return err
				}
				continue
			}
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return fmt.Errorf("fetch kafka message: %w", err)
		}

		row, err := ParseDeviceMessage(message.Value, time.Now().UTC())
		if err != nil {
			log.Printf("storage_service: skip invalid kafka message partition=%d offset=%d: %v", message.Partition, message.Offset, err)
			if err := s.reader.CommitMessages(ctx, message); err != nil {
				return fmt.Errorf("commit skipped kafka message: %w", err)
			}
			continue
		}

		batch.add(row, message)
		if batch.shouldFlush(s.cfg.Writer) {
			if err := s.flush(ctx, &batch); err != nil {
				return err
			}
		}
	}
}

func (s *Service) flush(ctx context.Context, batch *messageBatch) error {
	if len(batch.rows) == 0 {
		return nil
	}

	if err := s.writer.Append(ctx, batch.rows); err != nil {
		return err
	}
	if err := s.reader.CommitMessages(ctx, batch.messages...); err != nil {
		return fmt.Errorf("commit kafka messages: %w", err)
	}

	log.Printf("storage_service: wrote %d messages to Iceberg table %s.%s", len(batch.rows), s.cfg.Iceberg.Namespace, s.cfg.Iceberg.Table)
	batch.reset()
	return nil
}

func (b *messageBatch) add(row DeviceMessage, message kafka.Message) {
	b.rows = append(b.rows, row)
	b.messages = append(b.messages, message)
	b.bytes += int64(len(message.Key) + len(message.Value))
}

func (b *messageBatch) shouldFlush(cfg WriterConfig) bool {
	if len(b.rows) == 0 {
		return false
	}
	if cfg.BatchMaxMessages > 0 && len(b.rows) >= cfg.BatchMaxMessages {
		return true
	}
	return cfg.BatchMaxBytes > 0 && b.bytes >= cfg.BatchMaxBytes
}

func (b *messageBatch) reset() {
	b.rows = b.rows[:0]
	b.messages = b.messages[:0]
	b.bytes = 0
}
