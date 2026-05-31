package main

import (
	"context"
	"errors"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"iota/storage_service/internal/storage"
)

func main() {
	configPath := flag.String("config", "", "storage service config path")
	flag.Parse()

	cfg, err := storage.Load(*configPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	service, err := storage.NewService(cfg)
	if err != nil {
		log.Fatalf("create service: %v", err)
	}
	defer func() {
		if err := service.Close(); err != nil {
			log.Printf("close service: %v", err)
		}
	}()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	log.Printf("storage_service: consuming kafka topic %s as group %s", cfg.Kafka.Topic, cfg.Kafka.GroupID)
	log.Printf("storage_service: writing Iceberg table %s.%s at %s", cfg.Iceberg.Namespace, cfg.Iceberg.Table, cfg.Iceberg.TableLocation)

	if err := service.Run(ctx); err != nil && !errors.Is(err, context.Canceled) {
		log.Fatalf("run service: %v", err)
	}
}
