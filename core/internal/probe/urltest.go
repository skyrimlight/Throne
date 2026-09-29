package probe

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/sagernet/sing-box/adapter"
)

var URLReporter resultBuffer[URLTestResult]

const (
	URLTestTimeout   = 4 * time.Second
	FallbackTestURL1 = "http://www.google.com/generate_204"
	FallbackTestURL2 = "http://cp.cloudflare.com/"
	DefaultUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)

type URLTestResult struct {
	Duration time.Duration
	Tag      string
	Error    error
}

func BatchURLTest(ctx context.Context, i Box, outboundTags []string, url string, maxConcurrency int, twice bool, timeout time.Duration) []*URLTestResult {
	if timeout <= 0 {
		timeout = URLTestTimeout
	}

	results := runBatch(ctx, i, outboundTags, maxConcurrency, batchProbe[URLTestResult]{
		run: func(ctx context.Context, tag string, outbound adapter.Outbound) *URLTestResult {
			if err := awaitTunnels(ctx, i, tag); err != nil {
				return &URLTestResult{Tag: tag, Error: err}
			}
			client, closeClient := outboundHTTPClient(ctx, outbound)
			defer closeClient()

			testTimeout := firstRequestTimeout(i, tag, twice, timeout)
			duration, err := executeUrlTestWithFallback(ctx, client, url, testTimeout)
			if err == nil && twice {
				duration, err = executeUrlTestWithFallback(ctx, client, url, timeout)
			}
			return &URLTestResult{Duration: duration, Tag: tag, Error: err}
		},
		fail: func(tag string, err error) *URLTestResult {
			return &URLTestResult{Tag: tag, Error: err}
		},
		publish: URLReporter.AddResult,
	})
	URLReporter.Reclaim(results)
	return results
}

func executeUrlTestWithFallback(ctx context.Context, client *http.Client, targetUrl string, timeout time.Duration) (time.Duration, error) {
	if strings.TrimSpace(targetUrl) == "" {
		targetUrl = FallbackTestURL2
	}

	// 1. Primary URL test with browser User-Agent and status code verification
	duration, err := singleUrlTest(ctx, client, targetUrl, timeout)
	if err == nil {
		return duration, nil
	}

	// 2. If primary test fails, attempt a reliable fallback probe to eliminate false negatives
	// (e.g. Cloudflare rate-limiting/blocking datacenter IPs or specific domain routing issue)
	fallbackUrl := FallbackTestURL1
	if strings.Contains(targetUrl, "google") {
		fallbackUrl = FallbackTestURL2
	}

	fallbackTimeout := timeout
	if fallbackTimeout > 2500*time.Millisecond {
		fallbackTimeout = 2500 * time.Millisecond
	}

	fallbackDuration, fallbackErr := singleUrlTest(ctx, client, fallbackUrl, fallbackTimeout)
	if fallbackErr == nil {
		return fallbackDuration, nil
	}

	// Both failed: report the original error
	return 0, err
}

func singleUrlTest(ctx context.Context, client *http.Client, url string, timeout time.Duration) (time.Duration, error) {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	begin := time.Now()
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("User-Agent", DefaultUserAgent)
	req.Header.Set("Accept", "*/*")
	req.Header.Set("Connection", "close")

	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	_ = resp.Body.Close()

	// Strict status code validation:
	// Only 2xx and 3xx (200, 204, 301, 302, etc.) are considered working connections.
	// 4xx (403, 400) or 5xx (502 Bad Gateway, 503, 504) indicate proxy failure or target blocking.
	if resp.StatusCode < 200 || resp.StatusCode >= 400 {
		return 0, fmt.Errorf("HTTP %d", resp.StatusCode)
	}

	return time.Since(begin), nil
}
