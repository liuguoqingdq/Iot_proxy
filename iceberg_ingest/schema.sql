CREATE TABLE iota.default.device_message (
  mac_addr STRING NOT NULL,
  timestamp_ms BIGINT NOT NULL,
  event_time TIMESTAMP NOT NULL,
  payload BINARY NOT NULL,
  created_at TIMESTAMP NOT NULL
)
USING iceberg;
