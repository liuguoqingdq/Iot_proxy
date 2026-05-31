package webhdfs

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"net/url"
	"path"
	"strconv"
	"strings"
)

type Client struct {
	endpoint   *url.URL
	user       string
	httpClient *http.Client
}

func NewClient(endpoint string, user string) (*Client, error) {
	parsed, err := url.Parse(endpoint)
	if err != nil {
		return nil, err
	}
	parsed.Path = strings.TrimRight(parsed.Path, "/")
	return &Client{
		endpoint:   parsed,
		user:       user,
		httpClient: http.DefaultClient,
	}, nil
}

func (c *Client) ReadFile(ctx context.Context, name string) ([]byte, error) {
	u := c.opURL(name, "OPEN", nil)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, fs.ErrNotExist
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return nil, fmt.Errorf("webhdfs open %s: status %d: %s", name, resp.StatusCode, string(body))
	}
	return io.ReadAll(resp.Body)
}

func (c *Client) WriteFile(ctx context.Context, name string, data []byte, overwrite bool) error {
	return c.Write(ctx, name, bytes.NewReader(data), int64(len(data)), overwrite)
}

func (c *Client) Write(ctx context.Context, name string, r io.Reader, size int64, overwrite bool) error {
	if err := c.Mkdirs(ctx, path.Dir(hdfsPath(name))); err != nil {
		return err
	}

	q := url.Values{}
	q.Set("overwrite", strconv.FormatBool(overwrite))
	createURL := c.opURL(name, "CREATE", q)
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, createURL.String(), nil)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Length", "0")
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusTemporaryRedirect {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("webhdfs create %s: status %d: %s", name, resp.StatusCode, string(body))
	}
	location, err := resp.Location()
	if err != nil {
		return fmt.Errorf("webhdfs create %s: missing redirect location: %w", name, err)
	}

	putReq, err := http.NewRequestWithContext(ctx, http.MethodPut, location.String(), r)
	if err != nil {
		return err
	}
	if size >= 0 {
		putReq.ContentLength = size
	}
	putResp, err := c.httpClient.Do(putReq)
	if err != nil {
		return err
	}
	defer putResp.Body.Close()
	if putResp.StatusCode < 200 || putResp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(putResp.Body, 4096))
		return fmt.Errorf("webhdfs write %s: status %d: %s", name, putResp.StatusCode, string(body))
	}
	return nil
}

func (c *Client) Mkdirs(ctx context.Context, dir string) error {
	if dir == "." || dir == "/" || dir == "" {
		return nil
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, c.opURL(dir, "MKDIRS", nil).String(), nil)
	if err != nil {
		return err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("webhdfs mkdirs %s: status %d: %s", dir, resp.StatusCode, string(body))
	}
	return nil
}

func (c *Client) Delete(ctx context.Context, name string, recursive bool) error {
	q := url.Values{}
	q.Set("recursive", strconv.FormatBool(recursive))
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete, c.opURL(name, "DELETE", q).String(), nil)
	if err != nil {
		return err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return fs.ErrNotExist
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("webhdfs delete %s: status %d: %s", name, resp.StatusCode, string(body))
	}
	return nil
}

func (c *Client) List(ctx context.Context, dir string) ([]string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.opURL(dir, "LISTSTATUS", nil).String(), nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, fs.ErrNotExist
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return nil, fmt.Errorf("webhdfs list %s: status %d: %s", dir, resp.StatusCode, string(body))
	}
	var listing struct {
		FileStatuses struct {
			FileStatus []struct {
				PathSuffix string `json:"pathSuffix"`
			} `json:"FileStatus"`
		} `json:"FileStatuses"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&listing); err != nil {
		return nil, err
	}
	out := make([]string, 0, len(listing.FileStatuses.FileStatus))
	for _, item := range listing.FileStatuses.FileStatus {
		out = append(out, item.PathSuffix)
	}
	return out, nil
}

func IsNotExist(err error) bool {
	return errors.Is(err, fs.ErrNotExist)
}

func (c *Client) opURL(name string, op string, extra url.Values) *url.URL {
	u := *c.endpoint
	u.Path = strings.TrimRight(u.Path, "/") + hdfsPath(name)
	q := u.Query()
	q.Set("op", op)
	q.Set("user.name", c.user)
	for key, values := range extra {
		for _, value := range values {
			q.Add(key, value)
		}
	}
	u.RawQuery = q.Encode()
	return &u
}

func hdfsPath(name string) string {
	if parsed, err := url.Parse(name); err == nil && parsed.Scheme != "" {
		if parsed.Path != "" {
			name = parsed.Path
		}
	}
	if name == "" {
		return "/"
	}
	return "/" + strings.TrimPrefix(path.Clean(name), "/")
}
