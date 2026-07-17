// http.hpp - thin libcurl wrapper for JSON POST requests, with automatic
// retry on transient connection failures.  Supports external interrupt
// via an atomic flag (for Ctrl+C cancellation).
#pragma once

#include <atomic>
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
// worth retrying.
inline bool is_retriable_curlcode(CURLcode rc) {
    switch (rc) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_CONNECT:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_GOT_NOTHING:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_ENGINE_NOTFOUND:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_HTTP2_STREAM:
        case CURLE_OPERATION_TIMEDOUT:
            return true;
        default:
            return false;
    }
}

inline bool is_retriable_http_status(long status) {
    if (status == 429) return true;
    if (status >= 500 && status <= 599) return true;
    return false;
}

struct RetryPolicy {
    int  max_attempts     = 4;
    int  min_backoff_sec  = 30;
    int  max_backoff_sec  = 120;
    bool verbose          = true;
};

// Progress callback used by libcurl to poll for external interrupt.
// libcurl calls this frequently during data transfer.  Returning a
// non-zero value causes curl to abort the transfer with
// CURLE_ABORTED_BY_CALLBACK.
// The parameter names are kept to match the libcurl callback signature.
inline int progress_callback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* flag = static_cast<std::atomic<bool>*>(clientp);
    if (flag && flag->load(std::memory_order_relaxed)) {
        return 1;  // abort
    }
    return 0;
}

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

    // Set an atomic flag that libcurl will poll during transfers.
    // When the flag becomes true, the in-flight transfer is aborted
    // with CURLE_ABORTED_BY_CALLBACK.  This enables Ctrl+C to
    // interrupt a long-running LLM request.
    void set_interrupt_flag(std::atomic<bool>* flag) {
        interrupt_flag_ = flag;
    }

    struct AttemptResult {
        Response response;
        CURLcode curl_code = CURLE_OK;
        std::string error_text;
    };

    AttemptResult get_once(std::string_view url,
                           long timeout_seconds = 120) {
        AttemptResult out;
        std::string& body_buf = out.response.body;
        char errbuf[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl_, CURLOPT_URL, std::string(url).c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 60L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Client::write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body_buf);
        curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        // Install progress callback for interrupt support.
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, interrupt_flag_);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);

        out.curl_code = curl_easy_perform(curl_);
        if (out.curl_code == CURLE_OK) {
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &out.response.status);
        } else {
            out.error_text = errbuf[0] ? errbuf : curl_easy_strerror(out.curl_code);
        }
        return out;
    }

    Response get(std::string_view url,
                 long timeout_seconds = 120,
                 const RetryPolicy& policy = RetryPolicy{}) {
        AttemptResult last;
        for (int attempt = 1; attempt <= policy.max_attempts; ++attempt) {
            // Check for interrupt before each retry attempt.
            if (interrupt_flag_ && interrupt_flag_->load(std::memory_order_relaxed)) {
                throw RequestError("interrupted by user");
            }

            last = get_once(url, timeout_seconds);

            if (last.curl_code == CURLE_OK &&
                last.response.status >= 200 && last.response.status < 300) {
                return std::move(last.response);
            }

            // If the transfer was aborted by our progress callback (Ctrl+C),
            // do not retry.
            if (last.curl_code == CURLE_ABORTED_BY_CALLBACK) {
                throw RequestError("interrupted by user");
            }

            bool transport_retriable = is_retriable_curlcode(last.curl_code);
            bool http_retriable     = (last.curl_code == CURLE_OK &&
                                       is_retriable_http_status(last.response.status));

            if (!transport_retriable && !http_retriable) {
                if (last.curl_code != CURLE_OK) {
                    throw RequestError(std::format("HTTP GET failed: {} ({})",
                                                   last.error_text,
                                                   static_cast<int>(last.curl_code)));
                }
                return std::move(last.response);
            }

            if (attempt < policy.max_attempts) {
                int delay = policy.min_backoff_sec;
                for (int i = 1; i < attempt; ++i) delay = std::min(delay * 2, policy.max_backoff_sec);
                if (policy.verbose) {
                    std::string reason = last.curl_code != CURLE_OK
                        ? std::format("transport error: {} ({})",
                                      last.error_text, static_cast<int>(last.curl_code))
                        : std::format("HTTP {}", last.response.status);
                    std::cerr << std::format(
                        "[retry] GET attempt {}/{} failed ({}); waiting {}s before retry...\n",
                        attempt, policy.max_attempts, reason, delay);
                }
                std::this_thread::sleep_for(std::chrono::seconds(delay));
            } else {
                if (last.curl_code != CURLE_OK) {
                    throw RequestError(std::format(
                        "HTTP GET failed after {} attempts: {} ({})",
                        policy.max_attempts, last.error_text,
                        static_cast<int>(last.curl_code)));
                }
                return std::move(last.response);
            }
        }
        throw RequestError("get(): unreachable");
    }

    AttemptResult post_once(std::string_view url,
                            long timeout_seconds = 120) {
        AttemptResult out;
        std::string& body_buf = out.response.body;
        char errbuf[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(curl_, CURLOPT_URL, std::string(url).c_str());
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_body_.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body_.size()));
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 60L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Client::write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body_buf);
        curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        // Install progress callback for interrupt support.
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, interrupt_flag_);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);

        out.curl_code = curl_easy_perform(curl_);
        if (out.curl_code == CURLE_OK) {
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &out.response.status);
        } else {
            out.error_text = errbuf[0] ? errbuf : curl_easy_strerror(out.curl_code);
        }
        return out;
    }

    Response post(std::string_view url, std::string_view json_body,
                  long timeout_seconds = 120,
                  const RetryPolicy& policy = RetryPolicy{}) {
        post_body_ = std::string(json_body);

        AttemptResult last;
        for (int attempt = 1; attempt <= policy.max_attempts; ++attempt) {
            // Check for interrupt before each retry attempt.
            if (interrupt_flag_ && interrupt_flag_->load(std::memory_order_relaxed)) {
                throw RequestError("interrupted by user");
            }

            last = post_once(url, timeout_seconds);

            if (last.curl_code == CURLE_OK &&
                last.response.status >= 200 && last.response.status < 300) {
                return std::move(last.response);
            }

            // If the transfer was aborted by our progress callback (Ctrl+C),
            // do not retry.
            if (last.curl_code == CURLE_ABORTED_BY_CALLBACK) {
                throw RequestError("interrupted by user");
            }

            bool transport_retriable = is_retriable_curlcode(last.curl_code);
            bool http_retriable     = (last.curl_code == CURLE_OK &&
                                       is_retriable_http_status(last.response.status));

            if (!transport_retriable && !http_retriable) {
                if (last.curl_code != CURLE_OK) {
                    throw RequestError(std::format("HTTP request failed: {} ({})",
                                                   last.error_text,
                                                   static_cast<int>(last.curl_code)));
                }
                return std::move(last.response);
            }

            if (attempt < policy.max_attempts) {
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
                if (last.curl_code != CURLE_OK) {
                    throw RequestError(std::format(
                        "HTTP request failed after {} attempts: {} ({})",
                        policy.max_attempts, last.error_text,
                        static_cast<int>(last.curl_code)));
                }
                return std::move(last.response);
            }
        }
        throw RequestError("post(): unreachable");
    }

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
    std::string post_body_;
    std::atomic<bool>* interrupt_flag_{nullptr};
};

} // namespace http
