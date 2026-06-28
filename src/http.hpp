// http.hpp - thin libcurl wrapper for JSON POST requests, with automatic
// retry on transient connection failures.
#pragma once

#include <chrono>
#include <curl/curl.h>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace http {

struct Response {
    long status{0};
    std::string body;
};

class RequestError : public std::runtime_error {
public:
    explicit RequestError(std::string msg) : std::runtime_error(msg) {}
};

// Whether a libcurl result code represents a transient/network-layer failure
// worth retrying (couldn't reach the server, TLS handshake failed, timed out,
// got reset mid-stream, etc.). HTTP 5xx and 429 are also retriable at the
// caller layer; this helper only covers the transport-level cases.
inline bool is_retriable_curlcode(CURLcode rc) {
    switch (rc) {
        // Could not reach / resolve / connect to the host.
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_CONNECT:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_GOT_NOTHING:
        // TLS handshake layer.
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_ENGINE_NOTFOUND:
        // Network read/write errors during transfer.
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_HTTP2_STREAM:  // stream reset (e.g. RST_STREAM)
        // Operation timed out at connect or whole-request level.
        case CURLE_OPERATION_TIMEDOUT:
            return true;
        default:
            return false;
    }
}

// Whether an HTTP status code should trigger a retry (server errors and
// rate-limiting). 4xx (other than 429) are not retried: they indicate the
// request itself is bad (auth, schema, ...) and retrying won't help.
inline bool is_retriable_http_status(long status) {
    if (status == 429) return true;
    if (status >= 500 && status <= 599) return true;
    return false;
}

// Default retry policy: at least 3 retries, each waiting >= 30 seconds.
struct RetryPolicy {
    int  max_attempts     = 4;   // initial attempt + retries (so >=3 retries)
    int  min_backoff_sec  = 30;  // first retry waits this long
    int  max_backoff_sec  = 120; // cap exponential growth
    bool verbose          = true; // print retry progress to stderr
};

class Client {
public:
    Client() {
        curl_ = curl_easy_init();
        if (!curl_) throw RequestError("failed to initialize libcurl");
        headers_ = nullptr;
    }
    ~Client() {
        if (headers_) curl_slist_free_all(headers_);
        if (curl_) curl_easy_cleanup(curl_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept = delete;
    Client& operator=(Client&&) noexcept = delete;

    void set_header(std::string name, std::string value) {
        std::string h = std::format("{}: {}", std::move(name), std::move(value));
        headers_ = curl_slist_append(headers_, h.c_str());
    }
    void set_bearer_token(std::string_view token) {
        set_header("Authorization", std::format("Bearer {}", token));
    }

    // Single attempt. Returns Response + CURLcode so the caller can decide
    // whether to retry based on the specific failure.
    struct AttemptResult {
        Response response;
        CURLcode curl_code = CURLE_OK;
        std::string error_text;
    };

    AttemptResult post_once(std::string_view url, std::string_view json_body,
                            long timeout_seconds = 120) {
        AttemptResult out;
        std::string& body_buf = out.response.body;
        char errbuf[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl_, CURLOPT_URL, std::string(url).c_str());
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        std::string body_str(json_body);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Client::write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body_buf);
        curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, errbuf);
        // TLS verification stays on for safety.

        out.curl_code = curl_easy_perform(curl_);
        if (out.curl_code == CURLE_OK) {
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &out.response.status);
        } else {
            out.error_text = errbuf[0] ? errbuf : curl_easy_strerror(out.curl_code);
        }
        // Reset POSTFIELDS so a subsequent GET won't reuse stale body.
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
        return out;
    }

    // Retry wrapper. Retries on:
    //   - transport errors CURLE_COULDNT_CONNECT / TIMEDOUT / SSL_CONNECT_ERROR
    //     / SEND_ERROR / RECV_ERROR / etc. (see is_retriable_curlcode)
    //   - HTTP 5xx and 429
    // Throws RequestError only after exhausting all attempts.
    Response post(std::string_view url, std::string_view json_body,
                  long timeout_seconds = 120,
                  const RetryPolicy& policy = RetryPolicy{}) {
        AttemptResult last;
        for (int attempt = 1; attempt <= policy.max_attempts; ++attempt) {
            last = post_once(url, json_body, timeout_seconds);

            // Success: HTTP 2xx and no transport error.
            if (last.curl_code == CURLE_OK &&
                last.response.status >= 200 && last.response.status < 300) {
                return std::move(last.response);
            }

            // Decide if this failure is worth retrying.
            bool transport_retriable = is_retriable_curlcode(last.curl_code);
            bool http_retriable     = (last.curl_code == CURLE_OK &&
                                       is_retriable_http_status(last.response.status));

            // If not retriable at all, fail fast.
            if (!transport_retriable && !http_retriable) {
                if (last.curl_code != CURLE_OK) {
                    throw RequestError(std::format("HTTP request failed: {} ({})",
                                                   last.error_text,
                                                   static_cast<int>(last.curl_code)));
                }
                // Non-retriable HTTP status — return it to the caller so it
                // can surface the API error message. Don't throw here.
                return std::move(last.response);
            }

            // Retriable failure. If we have attempts left, wait and retry.
            if (attempt < policy.max_attempts) {
                // Exponential backoff capped at max_backoff_sec.
                int delay = policy.min_backoff_sec;
                for (int i = 1; i < attempt; ++i) delay = std::min(delay * 2, policy.max_backoff_sec);
                if (policy.verbose) {
                    std::string reason = last.curl_code != CURLE_OK
                        ? std::format("transport error: {} ({})",
                                      last.error_text, static_cast<int>(last.curl_code))
                        : std::format("HTTP {}", last.response.status);
                    std::cerr << std::format(
                        "[retry] attempt {}/{} failed ({}); waiting {}s before retry...\n",
                        attempt, policy.max_attempts, reason, delay);
                }
                std::this_thread::sleep_for(std::chrono::seconds(delay));
            } else {
                // Final attempt failed too.
                if (last.curl_code != CURLE_OK) {
                    throw RequestError(std::format(
                        "HTTP request failed after {} attempts: {} ({})",
                        policy.max_attempts, last.error_text,
                        static_cast<int>(last.curl_code)));
                }
                // Retriable HTTP status but out of attempts — return it so the
                // caller can parse the error body.
                return std::move(last.response);
            }
        }
        // Unreachable: loop either returns or throws.
        throw RequestError("post(): unreachable");
    }

    // Convenience: build headers fresh each call, then retry.
    Response post_json(std::string_view url, std::string_view json_body,
                       std::string_view bearer, long timeout = 120,
                       const RetryPolicy& policy = RetryPolicy{}) {
        reset_headers();
        set_header("Content-Type", "application/json");
        set_header("Accept", "application/json");
        if (!bearer.empty()) set_bearer_token(bearer);
        return post(url, json_body, timeout, policy);
    }

    void reset_headers() {
        if (headers_) { curl_slist_free_all(headers_); headers_ = nullptr; }
    }

private:
    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        size_t bytes = size * nmemb;
        static_cast<std::string*>(userdata)->append(ptr, bytes);
        return bytes;
    }

    CURL* curl_{nullptr};
    curl_slist* headers_{nullptr};
};

} // namespace http
