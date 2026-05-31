SELECT
  'iota.edge.raw.v1' AS schema,
  'mqtt' AS ingress_protocol,
  topic,
  clientid,
  username,
  qos,
  timestamp,
  id AS mqtt_message_id,
  base64_encode(payload) AS payload_base64
FROM
  "iota/+/devices/+/telemetry"
