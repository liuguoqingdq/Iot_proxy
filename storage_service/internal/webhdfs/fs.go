package webhdfs

import (
	"bytes"
	"context"
	"io"
	"io/fs"
	"os"
	"path"
	"time"

	iceio "github.com/apache/iceberg-go/io"
)

type FileSystem struct {
	client *Client
	ctx    context.Context
}

func NewFileSystem(ctx context.Context, client *Client) *FileSystem {
	return &FileSystem{client: client, ctx: ctx}
}

func (f *FileSystem) Open(name string) (iceio.File, error) {
	data, err := f.client.ReadFile(f.ctx, name)
	if err != nil {
		return nil, err
	}
	return &memoryFile{
		name:   path.Base(hdfsPath(name)),
		reader: bytes.NewReader(data),
		size:   int64(len(data)),
	}, nil
}

func (f *FileSystem) ReadFile(name string) ([]byte, error) {
	return f.client.ReadFile(f.ctx, name)
}

func (f *FileSystem) Remove(name string) error {
	return f.client.Delete(f.ctx, name, false)
}

func (f *FileSystem) Create(name string) (iceio.FileWriter, error) {
	tmp, err := os.CreateTemp("", "iota-webhdfs-*")
	if err != nil {
		return nil, err
	}
	return &fileWriter{
		ctx:    f.ctx,
		client: f.client,
		name:   name,
		tmp:    tmp,
	}, nil
}

func (f *FileSystem) WriteFile(name string, p []byte) error {
	return f.client.WriteFile(f.ctx, name, p, true)
}

type memoryFile struct {
	name   string
	reader *bytes.Reader
	size   int64
}

func (m *memoryFile) Close() error { return nil }
func (m *memoryFile) Read(p []byte) (int, error) {
	return m.reader.Read(p)
}
func (m *memoryFile) ReadAt(p []byte, off int64) (int, error) {
	return m.reader.ReadAt(p, off)
}
func (m *memoryFile) Seek(offset int64, whence int) (int64, error) {
	return m.reader.Seek(offset, whence)
}
func (m *memoryFile) Stat() (fs.FileInfo, error) {
	return fileInfo{name: m.name, size: m.size}, nil
}

type fileInfo struct {
	name string
	size int64
}

func (f fileInfo) Name() string       { return f.name }
func (f fileInfo) Size() int64        { return f.size }
func (f fileInfo) Mode() fs.FileMode  { return 0644 }
func (f fileInfo) ModTime() time.Time { return time.Time{} }
func (f fileInfo) IsDir() bool        { return false }
func (f fileInfo) Sys() any           { return nil }

type fileWriter struct {
	ctx    context.Context
	client *Client
	name   string
	tmp    *os.File
	closed bool
}

func (w *fileWriter) Write(p []byte) (int, error) {
	return w.tmp.Write(p)
}

func (w *fileWriter) ReadFrom(r io.Reader) (int64, error) {
	return io.Copy(w.tmp, r)
}

func (w *fileWriter) Close() error {
	if w.closed {
		return nil
	}
	w.closed = true
	name := w.tmp.Name()
	if err := w.tmp.Close(); err != nil {
		_ = os.Remove(name)
		return err
	}
	defer os.Remove(name)
	in, err := os.Open(name)
	if err != nil {
		return err
	}
	defer in.Close()
	stat, err := in.Stat()
	if err != nil {
		return err
	}
	return w.client.Write(w.ctx, w.name, in, stat.Size(), false)
}
