package storage

import (
	"testing"

	"github.com/segmentio/kafka-go"
)

func TestMessageBatchShouldFlush(t *testing.T) {
	var batch messageBatch
	if batch.shouldFlush(WriterConfig{BatchMaxMessages: 1}) {
		t.Fatal("empty batch should not flush")
	}

	batch.add(DeviceMessage{}, kafka.Message{Key: []byte("k"), Value: []byte("value")})
	if !batch.shouldFlush(WriterConfig{BatchMaxMessages: 1}) {
		t.Fatal("expected flush when batch_max_messages is reached")
	}

	batch.reset()
	batch.add(DeviceMessage{}, kafka.Message{Value: []byte("12345")})
	if !batch.shouldFlush(WriterConfig{BatchMaxBytes: 5}) {
		t.Fatal("expected flush when batch_max_bytes is reached")
	}
}
