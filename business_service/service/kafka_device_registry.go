package service

import (
	"context"
	"errors"
	"sync/atomic"
	"time"
)

type Coroutine_Loop struct {
	flag          int32
	SleepInterval time.Duration
}

type Loop_Control interface {
	Loop(ctx context.Context) error
	Start()
	Stop()
}

func (cl *Coroutine_Loop) Loop(ctx context.Context) error {
	if !cl.IsRunning() {
		return errors.New("Coroutine_Loop flag is false, can't continue")
	}

	interval := cl.SleepInterval
	if interval <= 0 {
		interval = time.Second
	}

	for cl.IsRunning() {
		_, err := SyncKafkaDeviceConnectionToMySQL(ctx)
		if err == nil {
			continue
		}
		if errors.Is(err, context.Canceled) {
			return nil
		}
		time.Sleep(interval)
		return err
	}

	return nil
}

func (cl *Coroutine_Loop) Start() {
	atomic.StoreInt32(&cl.flag, 1)
}

func (cl *Coroutine_Loop) Stop() {
	atomic.StoreInt32(&cl.flag, 0)
}

func (cl *Coroutine_Loop) IsRunning() bool {
	return atomic.LoadInt32(&cl.flag) == 1
}

var CoroutineLoop = Coroutine_Loop{
	SleepInterval: time.Second,
}
