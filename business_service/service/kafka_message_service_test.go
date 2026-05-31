package service

import "testing"

func TestParseKafkaDeviceConnection(t *testing.T) {
	value := []byte(`{
		"schema":"iota.edge.raw.v1",
		"mac_hex":"aabbccddeeff",
		"mac_addr":"aa:bb:cc:dd:ee:ff",
		"timestamp_ms":1780045491751,
		"payload_base64":"aGVsbG8=",
		"payload_len":5
	}`)

	message, err := ParseKafkaDeviceConnection(value)
	if err != nil {
		t.Fatalf("ParseKafkaDeviceConnection returned error: %v", err)
	}
	if message.MacAddr != "aa:bb:cc:dd:ee:ff" {
		t.Fatalf("MacAddr = %q", message.MacAddr)
	}
	if message.Timestamp != 1780045491751 {
		t.Fatalf("Timestamp = %d", message.Timestamp)
	}
}

func TestParseKafkaDeviceConnectionDoesNotRequirePayload(t *testing.T) {
	value := []byte(`{
		"schema":"iota.edge.raw.v1",
		"mac_hex":"aabbccddeeff",
		"timestamp_ms":1780045491751
	}`)

	message, err := ParseKafkaDeviceConnection(value)
	if err != nil {
		t.Fatalf("ParseKafkaDeviceConnection returned error: %v", err)
	}
	if message.MacAddr != "aabbccddeeff" {
		t.Fatalf("MacAddr = %q", message.MacAddr)
	}
}

func TestParseKafkaDeviceConnectionFromEMQXBridge(t *testing.T) {
	value := []byte(`{
		"schema":"iota.edge.raw.v1",
		"ingress_protocol":"mqtt",
		"topic":"iota/node-a/devices/aabbccddeeff/telemetry",
		"timestamp":1780045491751,
		"payload_base64":"bXF0dA==",
		"qos":1,
		"clientid":"device-aabbccddeeff"
	}`)

	message, err := ParseKafkaDeviceConnection(value)
	if err != nil {
		t.Fatalf("ParseKafkaDeviceConnection returned error: %v", err)
	}
	if message.MacAddr != "aabbccddeeff" {
		t.Fatalf("MacAddr = %q", message.MacAddr)
	}
	if message.Timestamp != 1780045491751 {
		t.Fatalf("Timestamp = %d", message.Timestamp)
	}
}
