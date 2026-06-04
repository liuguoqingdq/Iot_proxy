package s3store

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/url"
	"path"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/smithy-go"
)

type Config struct {
	Endpoint        string
	Region          string
	AccessKeyID     string
	SecretAccessKey string
	Bucket          string
	UsePathStyle    bool
}

type Client struct {
	bucket string
	s3     *s3.Client
}

func NewClient(cfg Config) (*Client, error) {
	if cfg.Endpoint == "" {
		return nil, fmt.Errorf("minio.endpoint is required")
	}
	if _, err := url.Parse(cfg.Endpoint); err != nil {
		return nil, fmt.Errorf("invalid minio.endpoint: %w", err)
	}
	if cfg.Region == "" {
		cfg.Region = "us-east-1"
	}
	if cfg.Bucket == "" {
		return nil, fmt.Errorf("minio.bucket is required")
	}
	if cfg.AccessKeyID == "" {
		return nil, fmt.Errorf("minio.access_key_id is required")
	}
	if cfg.SecretAccessKey == "" {
		return nil, fmt.Errorf("minio.secret_access_key is required")
	}

	awsCfg := aws.Config{
		Region: cfg.Region,
		Credentials: aws.NewCredentialsCache(
			credentials.NewStaticCredentialsProvider(
				cfg.AccessKeyID,
				cfg.SecretAccessKey,
				"",
			),
		),
	}
	client := s3.NewFromConfig(awsCfg, func(o *s3.Options) {
		o.BaseEndpoint = aws.String(strings.TrimRight(cfg.Endpoint, "/"))
		o.UsePathStyle = cfg.UsePathStyle
	})
	return &Client{bucket: cfg.Bucket, s3: client}, nil
}

func (c *Client) ReadFile(ctx context.Context, name string) ([]byte, error) {
	key := objectKey(name)
	out, err := c.s3.GetObject(ctx, &s3.GetObjectInput{
		Bucket: aws.String(c.bucket),
		Key:    aws.String(key),
	})
	if err != nil {
		if IsNotExist(err) {
			return nil, fs.ErrNotExist
		}
		return nil, fmt.Errorf("s3 get %s: %w", key, err)
	}
	defer out.Body.Close()
	return io.ReadAll(out.Body)
}

func (c *Client) WriteFile(ctx context.Context, name string, data []byte) error {
	return c.Write(ctx, name, bytes.NewReader(data), int64(len(data)))
}

func (c *Client) Write(ctx context.Context, name string, r io.Reader, size int64) error {
	key := objectKey(name)
	input := &s3.PutObjectInput{
		Bucket: aws.String(c.bucket),
		Key:    aws.String(key),
		Body:   r,
	}
	if size >= 0 {
		input.ContentLength = aws.Int64(size)
	}
	if _, err := c.s3.PutObject(ctx, input); err != nil {
		return fmt.Errorf("s3 put %s: %w", key, err)
	}
	return nil
}

func (c *Client) Delete(ctx context.Context, name string) error {
	key := objectKey(name)
	if _, err := c.s3.DeleteObject(ctx, &s3.DeleteObjectInput{
		Bucket: aws.String(c.bucket),
		Key:    aws.String(key),
	}); err != nil {
		return fmt.Errorf("s3 delete %s: %w", key, err)
	}
	return nil
}

func (c *Client) List(ctx context.Context, dir string) ([]string, error) {
	prefix := objectPrefix(dir)
	out, err := c.s3.ListObjectsV2(ctx, &s3.ListObjectsV2Input{
		Bucket:    aws.String(c.bucket),
		Prefix:    aws.String(prefix),
		Delimiter: aws.String("/"),
	})
	if err != nil {
		return nil, fmt.Errorf("s3 list %s: %w", prefix, err)
	}
	if len(out.Contents) == 0 && len(out.CommonPrefixes) == 0 {
		return nil, fs.ErrNotExist
	}
	names := make([]string, 0, len(out.Contents))
	for _, item := range out.Contents {
		if item.Key == nil || *item.Key == prefix {
			continue
		}
		names = append(names, path.Base(*item.Key))
	}
	return names, nil
}

func IsNotExist(err error) bool {
	if errors.Is(err, fs.ErrNotExist) {
		return true
	}
	var apiErr smithy.APIError
	if errors.As(err, &apiErr) {
		code := apiErr.ErrorCode()
		return code == "NoSuchKey" ||
			code == "NoSuchBucket" ||
			code == "NotFound"
	}
	return false
}

func objectKey(name string) string {
	if parsed, err := url.Parse(name); err == nil && parsed.Scheme != "" {
		name = strings.TrimPrefix(parsed.Path, "/")
	}
	return strings.TrimPrefix(path.Clean("/"+name), "/")
}

func objectPrefix(name string) string {
	key := objectKey(name)
	if key == "" || key == "." {
		return ""
	}
	return strings.TrimRight(key, "/") + "/"
}
