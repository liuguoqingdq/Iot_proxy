package icebergcatalog

import (
	"context"
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/apache/iceberg-go"
	"github.com/apache/iceberg-go/catalog"
	iceio "github.com/apache/iceberg-go/io"
	"github.com/apache/iceberg-go/table"
	"github.com/google/uuid"

	"iota/storage_service/internal/s3store"
)

var metadataFileName = regexp.MustCompile(`^(\d+)-([\w-]{36})(?:\.\w+)?\.metadata\.json$`)

type Catalog struct {
	identifier    table.Identifier
	tableLocation string
	client        *s3store.Client
}

func New(identifier table.Identifier, tableLocation string, client *s3store.Client) *Catalog {
	return &Catalog{
		identifier:    identifier,
		tableLocation: strings.TrimRight(tableLocation, "/"),
		client:        client,
	}
}

func (c *Catalog) LoadTable(ctx context.Context, identifier table.Identifier) (*table.Table, error) {
	if !sameIdentifier(identifier, c.identifier) {
		return nil, catalog.ErrNoSuchTable
	}
	loc, err := c.currentMetadataLocation(ctx)
	if err != nil {
		if s3store.IsNotExist(err) {
			return nil, catalog.ErrNoSuchTable
		}
		return nil, err
	}
	return table.NewFromLocation(ctx, identifier, loc, c.fsFactory(), c)
}

func (c *Catalog) CommitTable(ctx context.Context, identifier table.Identifier, reqs []table.Requirement, updates []table.Update) (table.Metadata, string, error) {
	current, err := c.LoadTable(ctx, identifier)
	if err != nil {
		return nil, "", err
	}
	for _, req := range reqs {
		if err := req.Validate(current.Metadata()); err != nil {
			return nil, "", err
		}
	}

	updated, err := table.UpdateTableMetadata(current.Metadata(), updates, current.MetadataLocation())
	if err != nil {
		return nil, "", err
	}
	if updated.Equals(current.Metadata()) {
		return current.Metadata(), current.MetadataLocation(), nil
	}

	provider, err := table.LoadLocationProvider(updated.Location(), updated.Properties())
	if err != nil {
		return nil, "", err
	}
	newVersion := parseMetadataVersion(current.MetadataLocation()) + 1
	newLocation, err := provider.NewTableMetadataFileLocation(newVersion)
	if err != nil {
		return nil, "", err
	}
	if err := c.writeMetadata(ctx, updated, newLocation, newVersion); err != nil {
		return nil, "", err
	}
	return updated, newLocation, nil
}

func (c *Catalog) CreateOrLoadTable(ctx context.Context, schema *iceberg.Schema, props iceberg.Properties) (*table.Table, error) {
	tbl, err := c.LoadTable(ctx, c.identifier)
	if err == nil {
		return tbl, nil
	}
	if err != catalog.ErrNoSuchTable {
		return nil, err
	}

	meta, err := table.NewMetadata(schema, iceberg.UnpartitionedSpec, table.UnsortedSortOrder, c.tableLocation, props)
	if err != nil {
		return nil, err
	}
	provider, err := table.LoadLocationProvider(meta.Location(), meta.Properties())
	if err != nil {
		return nil, err
	}
	location, err := provider.NewTableMetadataFileLocation(0)
	if err != nil {
		return nil, err
	}
	if err := c.writeMetadata(ctx, meta, location, 0); err != nil {
		return nil, err
	}
	return table.New(c.identifier, meta, location, c.fsFactory(), c), nil
}

func (c *Catalog) fsFactory() table.FSysF {
	return func(ctx context.Context) (iceio.IO, error) {
		return s3store.NewFileSystem(ctx, c.client), nil
	}
}

func (c *Catalog) writeMetadata(ctx context.Context, meta table.Metadata, location string, version int) error {
	data, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		return err
	}
	fs := s3store.NewFileSystem(ctx, c.client)
	if err := fs.WriteFile(location, append(data, '\n')); err != nil {
		return err
	}
	return c.client.WriteFile(ctx, c.versionHintPath(), []byte(strconv.Itoa(version)+"\n"))
}

func (c *Catalog) currentMetadataLocation(ctx context.Context) (string, error) {
	data, err := c.client.ReadFile(ctx, c.versionHintPath())
	if err != nil {
		return "", err
	}
	version, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil {
		return "", fmt.Errorf("parse version hint: %w", err)
	}
	files, err := c.client.List(ctx, c.metadataDir())
	if err != nil {
		return "", err
	}
	sort.Strings(files)
	prefix := fmt.Sprintf("%05d-", version)
	for _, name := range files {
		if strings.HasPrefix(name, prefix) && metadataFileName.MatchString(name) {
			return c.metadataDir() + "/" + name, nil
		}
	}
	return "", fmt.Errorf("metadata version %d not found under %s", version, c.metadataDir())
}

func (c *Catalog) versionHintPath() string {
	return c.metadataDir() + "/version-hint.text"
}

func (c *Catalog) metadataDir() string {
	return c.tableLocation + "/metadata"
}

func parseMetadataVersion(location string) int {
	base := location[strings.LastIndex(location, "/")+1:]
	matches := metadataFileName.FindStringSubmatch(base)
	if len(matches) != 3 {
		return -1
	}
	if _, err := uuid.Parse(matches[2]); err != nil {
		return -1
	}
	version, err := strconv.Atoi(matches[1])
	if err != nil {
		return -1
	}
	return version
}

func sameIdentifier(a table.Identifier, b table.Identifier) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
