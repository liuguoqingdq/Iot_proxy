package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"iota/business_service/database"
	"iota/business_service/mykafka"
	"iota/business_service/service"
)

func main() {
	err := mykafka.KafkaClientGlobal.InitKafka()
	if err != nil {
		panic(err)
	}

	defer func() {
		if err := mykafka.KafkaClientGlobal.CloseKafka(); err != nil {
			fmt.Println(err)
		}
	}()

	err = database.GormClientGlobal.InitGorm()
	if err != nil {
		panic(err)
	}
	defer func() {
		if err := database.GormClientGlobal.CloseGorm(); err != nil {
			fmt.Println(err)
		}
	}()

	service.CoroutineLoop.Start()
	defer service.CoroutineLoop.Stop()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	errCh := make(chan error, 1)
	go func() {
		errCh <- service.CoroutineLoop.Loop(ctx)
	}()

	signalCh := make(chan os.Signal, 1)
	signal.Notify(signalCh, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(signalCh)

	select {
	case err := <-errCh:
		if err != nil {
			panic(err)
		}
	case <-signalCh:
		service.CoroutineLoop.Stop()
		cancel()
		if err := <-errCh; err != nil {
			fmt.Println(err)
		}
	}
}
