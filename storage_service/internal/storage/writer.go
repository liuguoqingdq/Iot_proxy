package storage

import (
	"context"
	"fmt"
	"strconv"

	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/iceberg-go"
	"github.com/apache/iceberg-go/table"

	"iota/storage_service/internal/icebergcatalog"
	"iota/storage_service/internal/s3store"
)

type IcebergWriter struct {
	catalog    *icebergcatalog.Catalog
	identifier table.Identifier
	props      iceberg.Properties
}

func NewIcebergWriter(cfg *Config) (*IcebergWriter, error) {
	if cfg == nil {
		return nil, fmt.Errorf("config is required")
	}

	client, err := s3store.NewClient(s3store.Config{
		Endpoint:        cfg.MinIO.Endpoint,
		Region:          cfg.MinIO.Region,
		AccessKeyID:     cfg.MinIO.AccessKeyID,
		SecretAccessKey: cfg.MinIO.SecretAccessKey,
		Bucket:          cfg.MinIO.Bucket,
		UsePathStyle:    cfg.MinIO.UsePathStyle,
	})
	if err != nil {
		return nil, fmt.Errorf("create minio client: %w", err)
	}

	identifier := table.Identifier{cfg.Iceberg.Namespace, cfg.Iceberg.Table}
	props := iceberg.Properties{
		table.PropertyFormatVersion:       strconv.Itoa(table.DefaultFormatVersion),
		table.WriteTargetFileSizeBytesKey: strconv.FormatInt(cfg.Writer.TargetFileSizeBytes, 10),
		"write.format.default":            "parquet",
	}

	return &IcebergWriter{
		catalog:    icebergcatalog.New(identifier, cfg.Iceberg.TableLocation, client),
		identifier: identifier,
		props:      props,
	}, nil
}

func (w *IcebergWriter) Append(ctx context.Context, rows []DeviceMessage) error {
	if len(rows) == 0 {
		return nil
	}

	tbl, err := w.catalog.CreateOrLoadTable(ctx, DeviceMessageSchema(), w.props)
	if err != nil {
		return fmt.Errorf("create or load iceberg table: %w", err)
	}

	record, err := DeviceMessagesToRecord(rows)
	if err != nil {
		return fmt.Errorf("build arrow record: %w", err)
	}
	defer record.Release()

	reader, err := array.NewRecordReader(record.Schema(), []arrow.RecordBatch{record})
	if err != nil {
		return fmt.Errorf("create arrow record reader: %w", err)
	}
	defer reader.Release()

	if _, err := tbl.Append(ctx, reader, nil); err != nil {
		return fmt.Errorf("append iceberg rows: %w", err)
	}
	return nil
}
