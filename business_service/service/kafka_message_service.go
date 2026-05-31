package service

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"regexp"
	"time"

	"iota/business_service/DAO"
	"iota/business_service/model"
	"iota/business_service/mykafka"
)

type EdgeKafkaMessage struct {
	Schema          string `json:"schema"`
	IngressProtocol string `json:"ingress_protocol"`
	MacHex          string `json:"mac_hex"`
	MacAddr         string `json:"mac_addr"`
	Topic           string `json:"topic"`
	ClientID        string `json:"clientid"`
	Username        string `json:"username"`
	QoS             int    `json:"qos"`
	TimestampMS     uint64 `json:"timestamp_ms"`
	Timestamp       uint64 `json:"timestamp"`
	StreamID        uint64 `json:"stream_id"`
	Origin          string `json:"origin"`
	MessageID       string `json:"message_id"`
	MQTTMessageID   string `json:"mqtt_message_id"`
	ReceivedAtMS    int64  `json:"received_at_ms"`
}

var mqttDeviceTopicPattern = regexp.MustCompile(`(?:^|/)devices/([^/]+)/telemetry$`)

func SyncKafkaDeviceConnectionToMySQL(ctx context.Context) (*model.DeviceConnection, error) {
	kafkaMessage, err := mykafka.KafkaClientGlobal.FetchMessage(ctx)
	if err != nil {
		return nil, err
	}

	message, err := ParseKafkaDeviceConnection(kafkaMessage.Value)
	if err != nil {
		return nil, err
	}

	if err := DAO.UpsertProxyServedDevice(message.MacAddr, message.Timestamp); err != nil {
		return nil, err
	}

	if err := mykafka.KafkaClientGlobal.CommitMessage(ctx, kafkaMessage); err != nil {
		return nil, err
	}

	return message, nil
}

func ParseKafkaDeviceConnection(value []byte) (*model.DeviceConnection, error) {
	var envelope EdgeKafkaMessage
	if err := json.Unmarshal(value, &envelope); err != nil {
		return nil, fmt.Errorf("parse kafka message: %w", err)
	}
	if envelope.Schema != "" && envelope.Schema != "iota.edge.raw.v1" {
		return nil, fmt.Errorf("unsupported kafka message schema %q", envelope.Schema)
	}
	timestampMS := envelope.TimestampMS
	if timestampMS == 0 {
		timestampMS = envelope.Timestamp
	}
	if timestampMS == 0 {
		timestampMS = uint64(time.Now().UnixMilli())
	}
	if timestampMS > math.MaxInt64 {
		return nil, fmt.Errorf("timestamp_ms overflows int64: %d", timestampMS)
	}

	macAddr := envelope.MacAddr
	if macAddr == "" {
		macAddr = envelope.MacHex
	}
	if macAddr == "" {
		macAddr = macFromMQTTTopic(envelope.Topic)
	}
	if macAddr == "" {
		return nil, fmt.Errorf("missing mac address")
	}

	return &model.DeviceConnection{
		MacAddr:   macAddr,
		Timestamp: int64(timestampMS),
	}, nil
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
