package storage

import (
	"testing"
	"time"
)

func TestDeviceMessagesToRecord(t *testing.T) {
	record, err := DeviceMessagesToRecord([]DeviceMessage{{
		MacAddr:     "aa:bb:cc:dd:ee:ff",
		TimestampMS: 1780045491751,
		EventTime:   time.UnixMilli(1780045491751).UTC(),
		Payload:     []byte("hello"),
		CreatedAt:   time.Unix(0, 0).UTC(),
	}})
	if err != nil {
		t.Fatalf("DeviceMessagesToRecord() error = %v", err)
	}
	defer record.Release()

	if record.NumRows() != 1 {
		t.Fatalf("NumRows = %d", record.NumRows())
	}
	if record.NumCols() != 5 {
		t.Fatalf("NumCols = %d", record.NumCols())
	}

	field, ok := record.Schema().Field(0).Metadata.GetValue("PARQUET:field_id")
	if !ok || field != "1" {
		t.Fatalf("field id metadata = %q, %v", field, ok)
	}
}
