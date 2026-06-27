// http.hpp - thin libcurl wrapper for JSON POST requests.
#pragma once
 
#include <curl/curl.h>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
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
 
    Response post(std::string_view url, std::string_view json_body, long timeout_seconds = 120) {
        std::string body_buf;
        std::string header_dump;
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
 
        CURLcode rc = curl_easy_perform(curl_);
        if (rc != CURLE_OK) {
            throw RequestError(std::format("HTTP request failed: {} ({})",
                                           errbuf[0] ? errbuf : curl_easy_strerror(rc),
                                           static_cast<int>(rc)));
        }
 
        Response resp;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &resp.status);
        resp.body = std::move(body_buf);
        // Reset for next call: clear POSTFIELDS by clearing option.
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
        return resp;
    }
 
    // Convenience: build headers fresh each call.
    Response post_json(std::string_view url, std::string_view json_body,
                       std::string_view bearer, long timeout = 120) {
        reset_headers();
        set_header("Content-Type", "application/json");
        set_header("Accept", "application/json");
        if (!bearer.empty()) set_bearer_token(bearer);
        return post(url, json_body, timeout);
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
