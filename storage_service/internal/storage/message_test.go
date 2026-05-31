package storage

import (
	"testing"
	"time"
)

func TestParseDeviceMessage(t *testing.T) {
	createdAt := time.Date(2026, 5, 31, 10, 0, 0, 123000000, time.UTC)
	row, err := ParseDeviceMessage([]byte(`{
		"schema":"iota.edge.raw.v1",
		"mac_hex":"aabbccddeeff",
		"timestamp_ms":1780045491751,
		"payload_base64":"aGVsbG8=",
		"payload_len":5
	}`), createdAt)
	if err != nil {
		t.Fatalf("ParseDeviceMessage() error = %v", err)
	}

	if row.MacAddr != "aa:bb:cc:dd:ee:ff" {
		t.Fatalf("MacAddr = %q", row.MacAddr)
	}
	if row.TimestampMS != 1780045491751 {
		t.Fatalf("TimestampMS = %d", row.TimestampMS)
	}
	if string(row.Payload) != "hello" {
		t.Fatalf("Payload = %q", row.Payload)
	}
	if !row.CreatedAt.Equal(createdAt) {
		t.Fatalf("CreatedAt = %s", row.CreatedAt)
	}
}

func TestParseDeviceMessageFromMQTTTopic(t *testing.T) {
	row, err := ParseDeviceMessage([]byte(`{
		"schema":"iota.edge.raw.v1",
		"topic":"iota/node-a/devices/aa-bb-cc-dd-ee-ff/telemetry",
		"timestamp_ms":1780045491751
	}`), time.UnixMilli(1780045491800))
	if err != nil {
		t.Fatalf("ParseDeviceMessage() error = %v", err)
	}

	if row.MacAddr != "aa:bb:cc:dd:ee:ff" {
		t.Fatalf("MacAddr = %q", row.MacAddr)
	}
}

func TestParseDeviceMessageRejectsPayloadLenMismatch(t *testing.T) {
	_, err := ParseDeviceMessage([]byte(`{
		"schema":"iota.edge.raw.v1",
		"mac_addr":"aa:bb:cc:dd:ee:ff",
		"timestamp_ms":1780045491751,
		"payload_base64":"aGVsbG8=",
		"payload_len":6
	}`), time.Now())
	if err == nil {
		t.Fatal("expected payload_len mismatch error")
	}
}
