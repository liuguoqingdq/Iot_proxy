package storage

import (
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/iceberg-go"
	"github.com/apache/iceberg-go/table"
)

const (
	fieldIDMacAddr     = 1
	fieldIDTimestampMS = 2
	fieldIDEventTime   = 3
	fieldIDPayload     = 4
	fieldIDCreatedAt   = 5
)

func DeviceMessageSchema() *iceberg.Schema {
	return iceberg.NewSchema(0,
		iceberg.NestedField{ID: fieldIDMacAddr, Name: "mac_addr", Type: iceberg.PrimitiveTypes.String, Required: true},
		iceberg.NestedField{ID: fieldIDTimestampMS, Name: "timestamp_ms", Type: iceberg.PrimitiveTypes.Int64, Required: true},
		iceberg.NestedField{ID: fieldIDEventTime, Name: "event_time", Type: iceberg.PrimitiveTypes.Timestamp, Required: true},
		iceberg.NestedField{ID: fieldIDPayload, Name: "payload", Type: iceberg.PrimitiveTypes.Binary, Required: true},
		iceberg.NestedField{ID: fieldIDCreatedAt, Name: "created_at", Type: iceberg.PrimitiveTypes.Timestamp, Required: true},
	)
}

func DeviceMessageArrowSchema() (*arrow.Schema, error) {
	return table.SchemaToArrowSchema(DeviceMessageSchema(), nil, true, false)
}
