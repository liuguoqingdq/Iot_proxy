package storage

import (
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

func DeviceMessagesToRecord(rows []DeviceMessage) (arrow.Record, error) {
	schema, err := DeviceMessageArrowSchema()
	if err != nil {
		return nil, err
	}

	builder := array.NewRecordBuilder(memory.DefaultAllocator, schema)
	defer builder.Release()

	macAddr := builder.Field(0).(*array.StringBuilder)
	timestampMS := builder.Field(1).(*array.Int64Builder)
	eventTime := builder.Field(2).(*array.TimestampBuilder)
	payload := builder.Field(3).(*array.BinaryBuilder)
	createdAt := builder.Field(4).(*array.TimestampBuilder)

	for _, row := range rows {
		macAddr.Append(row.MacAddr)
		timestampMS.Append(row.TimestampMS)
		eventTime.Append(arrow.Timestamp(row.EventTime.UTC().UnixMicro()))
		payload.Append(row.Payload)
		createdAt.Append(arrow.Timestamp(row.CreatedAt.UTC().UnixMicro()))
	}

	return builder.NewRecord(), nil
}
