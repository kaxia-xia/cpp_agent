// llm.hpp - chat-completion client for OpenAI-compatible providers
// (DeepSeek, Zhipu GLM). Supports function/tool calling with streaming.
#pragma once

#include "http.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace llm {

struct Provider {
    std::string name;
    std::string base_url;
    std::string api_key_env;
    std::string default_model;
    std::string label;
};

inline std::vector<Provider> known_providers() {
    return {
        Provider{"deepseek", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY", "deepseek-chat", "DeepSeek"},
        Provider{"glm", "https://open.bigmodel.cn/api/paas/v4", "ZHIPU_API_KEY", "glm-4-flash", "Zhipu GLM"},
    };
}

inline const Provider* find_provider(std::string_view name) {
    auto providers = known_providers();
    for (const auto& p : providers) {
        if (p.name == name) {
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
    std::string arguments;
};

struct Message {
    std::string role;
    std::optional<std::string> content;
    std::optional<std::string> name;
    std::optional<std::string> tool_call_id;
    std::vector<ToolCall> tool_calls;

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

inline bool is_transient(const std::exception& e) {
    const std::string msg = e.what();
    if (dynamic_cast<const http::RequestError*>(&e) != nullptr) return true;
    if (msg.find("COULDNT_CONNECT")       != std::string::npos) return true;
    if (msg.find("COULDNT_RESOLVE")       != std::string::npos) return true;
    if (msg.find("OPERATION_TIMEDOUT")    != std::string::npos) return true;
    if (msg.find("SSL_CONNECT_ERROR")     != std::string::npos) return true;
    if (msg.find("SEND_ERROR")            != std::string::npos) return true;
    if (msg.find("RECV_ERROR")            != std::string::npos) return true;
    if (msg.find("HTTP request failed")   != std::string::npos) return true;
    if (msg.find("after") != std::string::npos &&
        msg.find("attempts") != std::string::npos) return true;
    if (msg.find("HTTP 5") != std::string::npos) return true;
    if (msg.find("HTTP 429") != std::string::npos) return true;
    return false;
}

// ── Streaming chat completion ────────────────────────────────────────

using StreamCallback = std::function<void(
    std::string_view text_delta,
    const std::vector<ToolCall>& tool_calls_so_far
)>;

inline CompletionResult chat_completion_stream(
    http::Client& http,
    const Provider& provider,
    const CompletionOptions& opts,
    const std::vector<Message>& messages,
    const json::Value& tools_json,
    std::string_view api_key,
    StreamCallback on_chunk = nullptr) {

    json::Object req;
    req["model"] = json::Value{opts.model};
    req["temperature"] = json::Value{opts.temperature};
    if (opts.max_tokens) req["max_tokens"] = json::Value{*opts.max_tokens};
    req["stream"] = json::Value{true};

    json::Array msgs;
    msgs.reserve(messages.size());
    for (const auto& m : messages) msgs.emplace_back(message_to_json(m));
    req["messages"] = json::Value{std::move(msgs)};

    if (tools_json.is_array() && !tools_json.as_array().empty()) {
        req["tools"] = tools_json;
    }

    const std::string url = std::format("{}/chat/completions", provider.base_url);
    const std::string body = json::Value{req}.serialize();

    // Streaming requests can take a long time (reasoning models may think
    // for 30+ seconds before emitting the first token). Use generous timeouts.
    http::RetryPolicy stream_policy;
    stream_policy.max_attempts = 2;
    stream_policy.min_backoff_sec = 15;
    stream_policy.max_backoff_sec = 30;

    // 600s = 10 minutes total timeout for the entire streaming response.
    http::Response resp = http.post_json(url, body, api_key, 600, stream_policy);

    if (resp.status < 200 || resp.status >= 300) {
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
            } else if (v.contains("error") && v["error"].is_string()) {
                detail = std::format(" [error: {}]", v["error"].as_string());
            } else if (v.contains("error_description") && v["error_description"].is_string()) {
                detail = std::format(" [error_description: {}]", v["error_description"].as_string());
            }
        } catch (...) { detail = std::format(" body={}", resp.body); }
        throw LLMError(std::format("HTTP {} from {}{}", resp.status, provider.label, detail));
    }

    // Parse SSE response body.
    CompletionResult result;
    Message am;
    am.role = "assistant";
    std::string full_content;
    std::vector<ToolCall> accumulated_tool_calls;
    bool has_tool_calls = false;

    struct PartialToolCall {
        std::string id;
        std::string function_name;
        std::string arguments;
        int index = 0;
    };
    std::vector<PartialToolCall> partial_tcs;

    auto flush_partial_tool_calls = [&]() {
        accumulated_tool_calls.clear();
        for (const auto& ptc : partial_tcs) {
            ToolCall tc;
            tc.id = ptc.id;
            tc.function_name = ptc.function_name;
            tc.arguments = ptc.arguments;
            accumulated_tool_calls.push_back(std::move(tc));
        }
    };

    std::string buf = resp.body;
    size_t pos = 0;
    while (pos < buf.size()) {
        size_t data_start = buf.find("data: ", pos);
        if (data_start == std::string::npos) break;

        size_t line_end = buf.find('\n', data_start);
        if (line_end == std::string::npos) line_end = buf.size();

        std::string line = buf.substr(data_start + 6, line_end - data_start - 6);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        pos = line_end + 1;

        if (line.empty() || line == "[DONE]") continue;

        json::Value chunk;
        try {
            chunk = json::parse(line);
        } catch (...) {
            continue;
        }

        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            const auto& u = chunk["usage"];
            result.prompt_tokens = static_cast<long>(u.contains("prompt_tokens") ? u["prompt_tokens"].as_number() : 0);
            result.completion_tokens = static_cast<long>(u.contains("completion_tokens") ? u["completion_tokens"].as_number() : 0);
            result.total_tokens = static_cast<long>(u.contains("total_tokens") ? u["total_tokens"].as_number() : 0);
        }

        if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].as_array().empty())
            continue;

        const auto& choice = chunk["choices"].as_array().front();

        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            std::string fr = choice["finish_reason"].as_string();
            if (!fr.empty() && fr != "null") result.finish_reason = fr;
        }

        if (!choice.contains("delta") || !choice["delta"].is_object())
            continue;

        const auto& delta = choice["delta"];

        if (delta.contains("content") && delta["content"].is_string()) {
            std::string text = delta["content"].as_string();
            full_content += text;
            if (on_chunk) on_chunk(text, accumulated_tool_calls);
        }

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            has_tool_calls = true;
            for (const auto& tc_delta : delta["tool_calls"].as_array()) {
                int idx = 0;
                if (tc_delta.contains("index") && tc_delta["index"].is_number())
                    idx = static_cast<int>(tc_delta["index"].as_number());

                while (static_cast<int>(partial_tcs.size()) <= idx)
                    partial_tcs.push_back({});

                auto& ptc = partial_tcs[idx];

                if (tc_delta.contains("id") && tc_delta["id"].is_string())
                    ptc.id = tc_delta["id"].as_string();

                if (tc_delta.contains("function") && tc_delta["function"].is_object()) {
                    const auto& fn = tc_delta["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                        ptc.function_name = fn["name"].as_string();
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                        ptc.arguments += fn["arguments"].as_string();
                }

                flush_partial_tool_calls();
                if (on_chunk) on_chunk("", accumulated_tool_calls);
            }
        }
    }

    if (has_tool_calls) {
        am.tool_calls = std::move(accumulated_tool_calls);
        am.content = full_content.empty() ? std::optional<std::string>{} : std::optional<std::string>(full_content);
    } else {
        am.content = full_content.empty() ? std::optional<std::string>{} : std::optional<std::string>(full_content);
    }

    result.assistant = std::move(am);
    return result;
}

inline CompletionResult chat_completion(
    http::Client& http,
    const Provider& provider,
    const CompletionOptions& opts,
    const std::vector<Message>& messages,
    const json::Value& tools_json,
    std::string_view api_key) {

    return chat_completion_stream(http, provider, opts, messages, tools_json, api_key, nullptr);
}

} // namespace llm
