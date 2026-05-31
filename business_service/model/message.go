package model

type DeviceConnection struct {
	MacAddr   string
	Timestamp int64
}

type ProxyServedDevice struct {
	ID        uint64 `gorm:"column:id;primaryKey;autoIncrement"`
	MacAddr   string `gorm:"column:mac_addr;type:varchar(17);not null;uniqueIndex:uk_proxy_served_device_mac"`
	Timestamp int64  `gorm:"column:timestamp;not null;index"`
}

func (ProxyServedDevice) TableName() string {
	return "proxy_served_device"
}
