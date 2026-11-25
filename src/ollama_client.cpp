// Client for Ollama's local REST API, the local model backend behind /ask and /ask/stream.

#include "ollama_client.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>

#include "catalog_parse.hpp"
#include "config.hpp"
#include "http_pool.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace ai_desktop::ollama {

using json = nlohmann::json;

namespace {

// A legitimate NDJSON chunk is at most a few KB -- this only guards against a
// malformed/adversarial response that never sends a newline, which would
// otherwise grow `buffer` in ask_stream()'s content_receiver without bound
// for the life of the request. Mirrors gemini_client.cpp's kMaxLineBufferBytes.
// Most relevant here since AI_DESKTOP_OLLAMA_HOST is user-configurable and can
// point at a non-local host, unlike the hardcoded cloud-provider hosts.
constexpr size_t kMaxLineBufferBytes = 1 << 20;  // 1 MiB

// Keep-alive client for the *current* Ollama host (see http_pool.hpp). host()
// is read on every request, so a host changed in the settings dialog takes
// effect immediately instead of staying pinned to the first request's value.
httplib::Client& client_for_thread(int timeout_ms) {
    return ai_desktop::client_for(host(), timeout_ms);
}

json build_request(const std::string& prompt,
                    const std::string& system,
                    const std::string& model,
                    bool stream) {
    json body;
    body["model"] = model.empty() ? default_model() : model;
    body["prompt"] = prompt;
    body["stream"] = stream;
    if (!system.empty()) {
        body["system"] = system;
    }
    // Without num_ctx Ollama uses a GPU-dependent default (4096 on a 12 GB card)
    // and drops the beginning of a longer prompt instead of failing. One fixed
    // value per server: a request with another one makes Ollama reload the model.
    if (int num_ctx = config::ollama_num_ctx(); num_ctx > 0) {
        body["options"] = {{"num_ctx", num_ctx}};
    }
    return body;
}

}  // namespace

// Both come from the config store, which applies AI_DESKTOP_OLLAMA_HOST /
// AI_DESKTOP_DEFAULT_MODEL first, then config.json (settings dialog), then the
// built-in default.
std::string host() {
    std::string raw = config::effective("ollama_host");
    if (raw.rfind("http://", 0) != 0 && raw.rfind("https://", 0) != 0) {
        raw = "http://" + raw;
    }
    return raw;
}

namespace {

constexpr int kTagsTimeoutMs = 1500;
constexpr auto kTagsTtl = std::chrono::seconds(15);

// The installed models, from GET /api/tags, kept for kTagsTtl (a failure too, so a dead Ollama costs one short
// timeout per window, not one per request). Keyed by host: another host in the settings is read afresh.
struct Tags {
    std::string host;
    std::chrono::steady_clock::time_point at;
    bool valid = false;      // the cache holds an answer for `host`
    bool reachable = false;  // Ollama answered with a model list
    json body;
};

Tags installed_tags() {
    static std::mutex mutex;
    static Tags cache;
    const std::string current = host();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (cache.valid && cache.host == current && std::chrono::steady_clock::now() - cache.at < kTagsTtl) {
            return cache;
        }
    }
    Tags fresh;
    fresh.host = current;
    httplib::Client client(current);
    client.set_connection_timeout(std::chrono::milliseconds(kTagsTimeoutMs));
    client.set_read_timeout(std::chrono::milliseconds(kTagsTimeoutMs));
    if (auto res = client.Get("/api/tags"); res && res->status == 200) {
        fresh.body = json::parse(res->body, nullptr, false);
        fresh.reachable = fresh.body.is_object() && fresh.body.contains("models") && fresh.body["models"].is_array();
    }
    fresh.at = std::chrono::steady_clock::now();
    fresh.valid = true;
    std::lock_guard<std::mutex> lock(mutex);
    cache = fresh;
    return fresh;
}

}  // namespace

DefaultModel default_model_info() {
    DefaultModel info;
    const std::string configured = config::effective("default_model");
    const std::string source = config::settings_json()["default_model"].value("source", "default");
    info.name = configured;
    info.source = source == "default" ? "builtin" : source;
    const Tags tags = installed_tags();
    if (!tags.reachable) {
        return info;  // installed stays unknown: Ollama is down or not an Ollama
    }
    if (source == "default") {
        // Nobody chose a model: the built-in one if it is there, else whatever is installed.
        const std::string picked = catalog::pick_installed_model(tags.body, configured);
        if (!picked.empty()) {
            info.name = picked;
            info.source = picked == configured ? "builtin" : "installed";
        }
    }
    info.installed = catalog::is_installed_model(tags.body, info.name) ? 1 : 0;
    return info;
}

std::string default_model() {
    return default_model_info().name;
}

AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms) {
    AskResult result;

    auto& cli = client_for_thread(timeout_ms);
    auto body = build_request(prompt, system, model, /*stream=*/false);

    auto res = cli.Post("/api/generate", body.dump(), "application/json");
    if (!res) {
        result.error_message =
            "ollama: " + httplib::to_string(res.error()) + " (" + host() + ")";
        return result;
    }
    if (res->status != 200) {
        result.error_message =
            "ollama: HTTP " + std::to_string(res->status) + ": " + res->body;
        return result;
    }

    auto parsed = json::parse(res->body, nullptr, false);
    if (parsed.is_discarded()) {
        result.error_message = "ollama: invalid JSON response";
        return result;
    }
    if (parsed.contains("error")) {
        result.error_message = "ollama: " + error_text(parsed["error"]);
        return result;
    }

    result.ok = true;
    result.text = parsed.value("response", "");
    result.stop_reason = parsed.value("done_reason", "");
    result.prompt_tokens = parsed.value("prompt_eval_count", 0L);
    result.completion_tokens = parsed.value("eval_count", 0L);
    return result;
}

std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta) {
    auto& cli = client_for_thread(timeout_ms);
    auto body = build_request(prompt, system, model, /*stream=*/true);

    // Ollama streams NDJSON (one JSON object per line), not SSE -- reframe
    // it into individual text deltas for the caller, who turns those into
    // this server's own SSE format. cpp-httplib's high-level Post()
    // overloads don't expose a response content_receiver, so the request is
    // built manually and sent via Client::send().
    std::string buffer;
    std::string stream_error;

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/generate";
    req.set_header("Content-Type", "application/json");
    req.body = body.dump();
    req.content_receiver = [&](const char* data, size_t len, uint64_t,
                                uint64_t) -> bool {
        buffer.append(data, len);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty()) continue;

            auto obj = json::parse(line, nullptr, false);
            if (obj.is_discarded()) continue;

            if (obj.contains("error")) {
                stream_error = "ollama: " + error_text(obj["error"]);
                return false;
            }

            std::string chunk = obj.value("response", "");
            if (!chunk.empty()) {
                on_delta(chunk);
            }
        }
        if (buffer.size() > kMaxLineBufferBytes) {
            stream_error = "ollama: streaming response line exceeded " +
                            std::to_string(kMaxLineBufferBytes) + " bytes without a newline";
            return false;
        }
        return true;
    };

    auto res = cli.send(req);

    if (!stream_error.empty()) {
        return stream_error;
    }
    if (!res) {
        return "ollama: " + httplib::to_string(res.error()) + " (" + host() + ")";
    }
    if (res->status != 200) {
        return "ollama: HTTP " + std::to_string(res->status) + ": " + res->body;
    }
    return "";
}

}  // namespace ai_desktop::ollama
