package database

import (
	"fmt"
	"sync"
	"time"

	appconfig "iota/business_service/config"

	mysqlDriver "gorm.io/driver/mysql"
	"gorm.io/gorm"
)

type GormClient struct {
	DB *gorm.DB
}
type GormControl interface {
	InitGorm() error
	CloseGorm() error
}

var GormClientGlobal GormClient
var Gorm_Client = &GormClientGlobal
var once sync.Once

func (gormClient *GormClient) InitGorm() error {
	cfg, err := appconfig.LoadDefault()
	if err != nil {
		return err
	}
	return gormClient.InitGormWithConfig(cfg.MySQL)
}

func (gormClient *GormClient) InitGormWithConfig(cfg appconfig.MySQLConfig) error {
	var gormErr error
	once.Do(func() {
		db, err := gorm.Open(mysqlDriver.Open(cfg.DSN()), &gorm.Config{})
		if err != nil {
			gormErr = err
			return
		}

		sqlDB, err := db.DB()
		if err != nil {
			gormErr = err
			return
		}

		sqlDB.SetMaxOpenConns(20)
		sqlDB.SetMaxIdleConns(10)
		sqlDB.SetConnMaxLifetime(time.Hour)

		if err := sqlDB.Ping(); err != nil {
			gormErr = err
			return
		}

		gormClient.DB = db
		fmt.Println("GORM MySQL is connected")
	})

	return gormErr
}

func (gormClient *GormClient) CloseGorm() error {
	if gormClient.DB == nil {
		return nil
	}

	sqlDB, err := gormClient.DB.DB()
	if err != nil {
		return err
	}

	gormClient.DB = nil
	return sqlDB.Close()
}
