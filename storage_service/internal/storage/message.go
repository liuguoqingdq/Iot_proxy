package storage

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math"
	"regexp"
	"strings"
	"time"
)

const edgeKafkaSchema = "iota.edge.raw.v1"

var (
	mqttDeviceTopicPattern = regexp.MustCompile(`(?:^|/)devices/([^/]+)/telemetry$`)
	macHexPattern          = regexp.MustCompile(`^[0-9a-fA-F]{12}$`)
)

type EdgeKafkaMessage struct {
	Schema        string `json:"schema"`
	MacHex        string `json:"mac_hex"`
	MacAddr       string `json:"mac_addr"`
	Topic         string `json:"topic"`
	TimestampMS   uint64 `json:"timestamp_ms"`
	Timestamp     uint64 `json:"timestamp"`
	PayloadBase64 string `json:"payload_base64"`
	PayloadLen    *int   `json:"payload_len"`
}

type DeviceMessage struct {
	MacAddr     string
	TimestampMS int64
	EventTime   time.Time
	Payload     []byte
	CreatedAt   time.Time
}

func ParseDeviceMessage(value []byte, createdAt time.Time) (DeviceMessage, error) {
	var envelope EdgeKafkaMessage
	if err := json.Unmarshal(value, &envelope); err != nil {
		return DeviceMessage{}, fmt.Errorf("parse kafka message: %w", err)
	}
	if envelope.Schema != "" && envelope.Schema != edgeKafkaSchema {
		return DeviceMessage{}, fmt.Errorf("unsupported kafka message schema %q", envelope.Schema)
	}

	timestampMS := envelope.TimestampMS
	if timestampMS == 0 {
		timestampMS = envelope.Timestamp
	}
	if timestampMS == 0 {
		timestampMS = uint64(createdAtOrNow(createdAt).UnixMilli())
	}
	if timestampMS > math.MaxInt64 {
		return DeviceMessage{}, fmt.Errorf("timestamp_ms overflows int64: %d", timestampMS)
	}

	macAddr := normalizeMacAddr(envelope.MacAddr)
	if macAddr == "" {
		macAddr = normalizeMacAddr(envelope.MacHex)
	}
	if macAddr == "" {
		macAddr = normalizeMacAddr(macFromMQTTTopic(envelope.Topic))
	}
	if macAddr == "" {
		return DeviceMessage{}, fmt.Errorf("missing mac address")
	}

	payload, err := decodePayload(envelope.PayloadBase64)
	if err != nil {
		return DeviceMessage{}, err
	}
	if envelope.PayloadLen != nil {
		if *envelope.PayloadLen < 0 {
			return DeviceMessage{}, fmt.Errorf("payload_len cannot be negative: %d", *envelope.PayloadLen)
		}
		if *envelope.PayloadLen != len(payload) {
			return DeviceMessage{}, fmt.Errorf("payload_len mismatch: declared %d, decoded %d", *envelope.PayloadLen, len(payload))
		}
	}

	createdAt = createdAtOrNow(createdAt)
	return DeviceMessage{
		MacAddr:     macAddr,
		TimestampMS: int64(timestampMS),
		EventTime:   time.UnixMilli(int64(timestampMS)).UTC(),
		Payload:     payload,
		CreatedAt:   createdAt,
	}, nil
}

func decodePayload(encoded string) ([]byte, error) {
	encoded = strings.TrimSpace(encoded)
	if encoded == "" {
		return []byte{}, nil
	}
	payload, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil, fmt.Errorf("decode payload_base64: %w", err)
	}
	return payload, nil
}

func normalizeMacAddr(mac string) string {
	mac = strings.ToLower(strings.TrimSpace(mac))
	if mac == "" {
		return ""
	}
	compact := strings.NewReplacer(":", "", "-", "").Replace(mac)
	if macHexPattern.MatchString(compact) {
		parts := make([]string, 0, 6)
		for i := 0; i < len(compact); i += 2 {
			parts = append(parts, compact[i:i+2])
		}
		return strings.Join(parts, ":")
	}
	return mac
}

func macFromMQTTTopic(topic string) string {
	if topic == "" {
		return ""
	}
	matches := mqttDeviceTopicPattern.FindStringSubmatch(topic)
	if len(matches) != 2 {
		return ""
	}
	return matches[1]
}

func createdAtOrNow(createdAt time.Time) time.Time {
	if createdAt.IsZero() {
		return time.Now().UTC()
	}
	return createdAt.UTC()
}
