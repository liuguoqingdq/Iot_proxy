CREATE TABLE IF NOT EXISTS proxy_served_device (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  mac_addr VARCHAR(17) NOT NULL,
  timestamp BIGINT NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_proxy_served_device_mac (mac_addr),
  KEY idx_proxy_served_device_timestamp (timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
