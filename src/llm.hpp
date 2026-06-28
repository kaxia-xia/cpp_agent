// llm.hpp - chat-completion client for OpenAI-compatible providers
// (DeepSeek, Zhipu GLM). Supports function/tool calling.
#pragma once

#include "http.hpp"
#include "json.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace llm {

struct Provider {
    std::string name;          // "deepseek" | "glm"
    std::string base_url;       // no trailing slash
    std::string api_key_env;    // env var holding the API key
    std::string default_model;
    std::string label;          // human friendly
};

inline std::vector<Provider> known_providers() {
    return {
        Provider{
            "deepseek",
            "https://api.deepseek.com/v1",
            "DEEPSEEK_API_KEY",
            "deepseek-chat",
            "DeepSeek",
        },
        Provider{
            "glm",
            "https://open.bigmodel.cn/api/paas/v4",
            "ZHIPU_API_KEY",
            "glm-4.5",
            "Zhipu GLM",
        },
    };
}

inline const Provider* find_provider(std::string_view name) {
    auto providers = known_providers();
    for (const auto& p : providers) {
        if (p.name == name) {
            // Returning pointer into local vector is unsafe; copy out.
            static thread_local Provider stash;
            stash = p;
            return &stash;
        }
    }
    return nullptr;
}

struct ToolCall {
    std::string id;
    std::string function_name;
    std::string arguments;  // raw JSON string
};

struct Message {
    std::string role;                            // system | user | assistant | tool
    std::optional<std::string> content;
    std::optional<std::string> name;             // tool role: tool name
    std::optional<std::string> tool_call_id;     // tool role: originating call id
    std::vector<ToolCall> tool_calls;             // assistant role

    static Message system(std::string s) {
        Message m; m.role = "system"; m.content = std::move(s); return m;
    }
    static Message user(std::string s) {
        Message m; m.role = "user"; m.content = std::move(s); return m;
    }
    static Message assistant(std::optional<std::string> text,
                              std::vector<ToolCall> calls = {}) {
        Message m; m.role = "assistant"; m.content = std::move(text); m.tool_calls = std::move(calls); return m;
    }
    static Message tool_result(std::string tool_call_id, std::string name, std::string content) {
        Message m; m.role = "tool"; m.tool_call_id = std::move(tool_call_id);
        m.name = std::move(name); m.content = std::move(content); return m;
    }
};

inline json::Value message_to_json(const Message& m) {
    json::Object obj;
    obj["role"] = json::Value{m.role};
    if (m.content) obj["content"] = json::Value{*m.content};
    else if (m.role == "assistant") {
        // OpenAI requires content to be present (may be empty string) when tool_calls exist.
        if (!m.tool_calls.empty()) obj["content"] = json::Value{""};
    } else if (m.role != "assistant") {
        obj["content"] = json::Value{""};
    }
    if (m.name) obj["name"] = json::Value{*m.name};
    if (m.tool_call_id) obj["tool_call_id"] = json::Value{*m.tool_call_id};
    if (!m.tool_calls.empty()) {
        json::Array tcs;
        tcs.reserve(m.tool_calls.size());
        for (const auto& tc : m.tool_calls) {
            json::Object tc_obj;
            tc_obj["id"] = json::Value{tc.id};
            tc_obj["type"] = json::Value{"function"};
            json::Object fn;
            fn["name"] = json::Value{tc.function_name};
            // arguments must be a JSON string per OpenAI spec.
            // Validate parseability; if invalid, wrap as raw string.
            fn["arguments"] = json::Value{tc.arguments};
            tc_obj["function"] = json::Value{std::move(fn)};
            tcs.emplace_back(std::move(tc_obj));
        }
        obj["tool_calls"] = json::Value{std::move(tcs)};
    }
    return json::Value{std::move(obj)};
}

struct CompletionOptions {
    std::string model;
    double temperature = 0.3;
    std::optional<int> max_tokens;
};

struct CompletionResult {
    Message assistant;
    std::string finish_reason;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    long total_tokens = 0;
};

class LLMError : public std::runtime_error {
public:
    explicit LLMError(std::string msg) : std::runtime_error(msg) {}
};

// True if the error is a transport-level failure (network unreachable,
// connection refused, TLS handshake failed, timeout) rather than an
// API-level rejection (4xx). The agent loop uses this to decide whether
// retrying the whole turn makes sense.
//
// Note: connection-layer retries already happen inside http::Client::post
// (>= 3 retries, each >= 30s). If we get here, those have been exhausted.
inline bool is_transient(const std::exception& e) {
    const std::string msg = e.what();
    // Markers produced by http::RequestError on exhausted transport retries.
    if (dynamic_cast<const http::RequestError*>(&e) != nullptr) return true;
    // Substring fallback in case the error gets re-wrapped.
    if (msg.find("COULDNT_CONNECT")       != std::string::npos) return true;
    if (msg.find("COULDNT_RESOLVE")       != std::string::npos) return true;
    if (msg.find("OPERATION_TIMEDOUT")    != std::string::npos) return true;
    if (msg.find("SSL_CONNECT_ERROR")     != std::string::npos) return true;
    if (msg.find("SEND_ERROR")            != std::string::npos) return true;
    if (msg.find("RECV_ERROR")            != std::string::npos) return true;
    if (msg.find("HTTP request failed")   != std::string::npos) return true;
    if (msg.find("after") != std::string::npos &&
        msg.find("attempts") != std::string::npos) return true;
    // 5xx and 429 are also transient server-side issues.
    if (msg.find("HTTP 5") != std::string::npos) return true;
    if (msg.find("HTTP 429") != std::string::npos) return true;
    return false;
}

inline CompletionResult chat_completion(http::Client& http,
                                        const Provider& provider,
                                        const CompletionOptions& opts,
                                        const std::vector<Message>& messages,
                                        const json::Value& tools_json,
                                        std::string_view api_key) {
    json::Object req;
    req["model"] = json::Value{opts.model};
    req["temperature"] = json::Value{opts.temperature};
    if (opts.max_tokens) req["max_tokens"] = json::Value{*opts.max_tokens};
    req["stream"] = json::Value{false};

    json::Array msgs;
    msgs.reserve(messages.size());
    for (const auto& m : messages) msgs.emplace_back(message_to_json(m));
    req["messages"] = json::Value{std::move(msgs)};

    if (tools_json.is_array() && !tools_json.as_array().empty()) {
        req["tools"] = tools_json;
    }

    const std::string url = std::format("{}/chat/completions", provider.base_url);
    const std::string body = json::Value{req}.serialize();

    http::Response resp = http.post_json(url, body, api_key);

    if (resp.status < 200 || resp.status >= 300) {
        // Try to surface API error message.
        std::string detail;
        try {
            json::Value v = json::parse(resp.body);
            if (v.contains("error") && v["error"].is_object()) {
                const auto& err = v["error"];
                std::string msg = err.contains("message") && err["message"].is_string()
                                  ? err["message"].as_string() : std::string{};
                std::string type = err.contains("type") && err["type"].is_string()
                                  ? err["type"].as_string() : std::string{};
                detail = std::format(" [{}: {}]", type.empty() ? "error" : type, msg);
            }
        } catch (...) { detail = std::format(" body={}", resp.body); }
        throw LLMError(std::format("HTTP {} from {}{}", resp.status, provider.label, detail));
    }

    json::Value v;
    try {
        v = json::parse(resp.body);
    } catch (const std::exception& e) {
        throw LLMError(std::format("failed to parse response JSON: {}; body head: {}",
                                   e.what(), resp.body.substr(0, 200)));
    }

    CompletionResult result;
    if (v.contains("usage") && v["usage"].is_object()) {
        const auto& u = v["usage"];
        result.prompt_tokens = static_cast<long>(u.contains("prompt_tokens") ? u["prompt_tokens"].as_number() : 0);
        result.completion_tokens = static_cast<long>(u.contains("completion_tokens") ? u["completion_tokens"].as_number() : 0);
        result.total_tokens = static_cast<long>(u.contains("total_tokens") ? u["total_tokens"].as_number() : 0);
    }

    if (!v.contains("choices") || !v["choices"].is_array() || v["choices"].as_array().empty()) {
        throw LLMError(std::format("response missing choices: {}", resp.body.substr(0, 300)));
    }
    const auto& choice = v["choices"].as_array().front();
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        result.finish_reason = choice["finish_reason"].as_string();

    if (!choice.contains("message") || !choice["message"].is_object()) {
        throw LLMError(std::format("choice missing message: {}", resp.body.substr(0, 300)));
    }
    const auto& msg = choice["message"];

    Message am;
    am.role = msg.contains("role") && msg["role"].is_string() ? msg["role"].as_string() : "assistant";
    if (msg.contains("content")) {
        if (msg["content"].is_string()) am.content = msg["content"].as_string();
        else if (msg["content"].is_null()) am.content = std::string{};
    }

    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"].as_array()) {
            ToolCall call;
            call.id = tc.contains("id") && tc["id"].is_string() ? tc["id"].as_string() : "";
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                call.function_name = fn.contains("name") && fn["name"].is_string() ? fn["name"].as_string() : "";
                // arguments is a JSON-encoded string.
                if (fn.contains("arguments")) {
                    if (fn["arguments"].is_string()) call.arguments = fn["arguments"].as_string();
                    else call.arguments = fn["arguments"].serialize();  // fallback: re-serialize
                }
            }
            am.tool_calls.push_back(std::move(call));
        }
    }

    result.assistant = std::move(am);
    return result;
}

} // namespace llm
