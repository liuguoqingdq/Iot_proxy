package DAO

import (
	"errors"
	"fmt"
	"strings"

	"iota/business_service/database"
	model "iota/business_service/model"

	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

var ErrDatabaseNotInitialized = errors.New("gorm database is not initialized")

func UpsertProxyServedDevice(macAddr string, timestamp int64) error {
	db, err := messageDB()
	if err != nil {
		return err
	}

	normalizedMAC, err := normalizeMAC(macAddr)
	if err != nil {
		return err
	}

	record := &model.ProxyServedDevice{
		MacAddr:   normalizedMAC,
		Timestamp: timestamp,
	}
	return db.Clauses(clause.OnConflict{
		Columns: []clause.Column{{Name: "mac_addr"}},
		DoUpdates: clause.Assignments(map[string]interface{}{
			"timestamp": timestamp,
		}),
	}).Create(record).Error
}

func messageDB() (*gorm.DB, error) {
	if database.GormClientGlobal.DB == nil {
		return nil, ErrDatabaseNotInitialized
	}
	return database.GormClientGlobal.DB, nil
}

func normalizeMAC(macAddr string) (string, error) {
	clean := strings.NewReplacer(":", "", "-", "", ".", "").Replace(macAddr)
	clean = strings.ToLower(clean)
	if len(clean) != 12 {
		return "", fmt.Errorf("invalid mac address %q", macAddr)
	}

	for _, ch := range clean {
		if !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) {
			return "", fmt.Errorf("invalid mac address %q", macAddr)
		}
	}

	var builder strings.Builder
	builder.Grow(17)
	for i := 0; i < len(clean); i += 2 {
		if i != 0 {
			builder.WriteByte(':')
		}
		builder.WriteString(clean[i : i+2])
	}
	return builder.String(), nil
}
