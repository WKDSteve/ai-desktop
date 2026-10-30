#include "api.hpp"

#include "catalog.hpp"
#include "config.hpp"
#include "models.hpp"
#include "secrets.hpp"
#include "usage.hpp"

#include <cstdlib>

using json = nlohmann::json;

namespace ai_desktop::api {

namespace {

// Parses the request body as a JSON object; answers 400 itself when it is not one.
bool parse_object(const httplib::Request& req, httplib::Response& res, json& out) {
    out = json::parse(req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        send_error(res, 400, "the request body must be a JSON object");
        return false;
    }
    return true;
}

// The price table sits beside the string settings: the dashboard's settings
// view lists the latter, the usage view reads the former.
json settings_response() {
    return {{"settings", config::settings_json()}, {"config_dir", config::directory()}, {"prices", config::prices()}};
}

// A usage event from a client is small by construction; anything bigger is not one.
constexpr size_t kMaxUsageEventBody = 4 * 1024;
constexpr size_t kDefaultUsageLimit = 200;
constexpr size_t kMaxUsageLimit = 2000;

// Parses ?limit=; false (after answering 400) for anything that is not a plain positive integer.
bool parse_limit(const httplib::Request& req, httplib::Response& res, size_t& limit) {
    limit = kDefaultUsageLimit;
    if (!req.has_param("limit")) {
        return true;
    }
    const std::string text = req.get_param_value("limit");
    if (text.empty() || text.size() > 6) {
        send_error(res, 400, "limit: expected a positive integer");
        return false;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            send_error(res, 400, "limit: expected a positive integer");
            return false;
        }
    }
    const long value = std::strtol(text.c_str(), nullptr, 10);
    if (value < 1) {
        send_error(res, 400, "limit: expected a positive integer");
        return false;
    }
    limit = static_cast<size_t>(value) > kMaxUsageLimit ? kMaxUsageLimit : static_cast<size_t>(value);
    return true;
}

// Reads ?since= / ?until= into `out`; false (after answering 400) for a value that
// is not the stored timestamp shape, since any other spelling would compare wrongly.
bool parse_timestamp_param(const httplib::Request& req, httplib::Response& res, const char* name, std::string& out) {
    out.clear();
    if (!req.has_param(name)) {
        return true;
    }
    out = req.get_param_value(name);
    if (!usage::is_timestamp(out)) {
        send_error(res, 400, std::string(name) + ": expected a UTC timestamp like 2026-10-30T12:34:56Z");
        return false;
    }
    return true;
}

// The three provider keys are managed here; the key of a custom model belongs to
// the model and is set through /models/custom.
bool is_provider_key(const std::string& name) {
    return secrets::valid_name(name) && name.rfind("custom:", 0) != 0;
}

}  // namespace

void register_routes(httplib::Server& server) {
    // ----- models ----------------------------------------------------------

    // Model listing: local (Ollama), cloud (provider list endpoints, only with a
    // key), custom. ?refresh=1 bypasses the cache.
    server.Get("/models", [](const httplib::Request& req, httplib::Response& res) {
        const std::string refresh = req.get_param_value("refresh");
        send_json(res, 200, catalog::list(refresh == "1" || refresh == "true"));
    });

    // Details for the hover popup: GET /models/info?id=<model id>
    server.Get("/models/info", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("id")) {
            send_error(res, 400, "query parameter 'id' is required");
            return;
        }
        auto result = catalog::info(req.get_param_value("id"));
        send_json(res, result.http, result.body);
    });

    // ----- custom models ----------------------------------------------------

    server.Get("/models/custom", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, {{"models", models::custom_list()}});
    });

    server.Post("/models/custom", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        json created;
        config::Status st = models::custom_add(body, created);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 201, created);
    });

    // The id may contain '/' and ':' (URL-decoded before routing), hence `.+`.
    server.Delete(R"(/models/custom/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        config::Status st = models::custom_remove(req.matches[1].str());
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        res.status = 204;
    });

    // ----- settings ---------------------------------------------------------

    server.Get("/settings", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, settings_response());
    });

    // Flat partial update, e.g. {"theme":"dark"}; null resets a key.
    server.Put("/settings", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        config::Status st = config::update_settings(body);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 200, settings_response());
    });

    // ----- usage (metadata only, never text) ---------------------------------

    // GET /usage?provider=<id>&since=<ISO>&until=<ISO>&limit=<n>: the newest
    // matching events plus totals, by provider and by model, priced with the
    // table from the settings. since/until take exactly "YYYY-MM-DDTHH:MM:SSZ".
    server.Get("/usage", [](const httplib::Request& req, httplib::Response& res) {
        size_t limit = 0;
        if (!parse_limit(req, res, limit)) {
            return;
        }
        usage::Filter filter;
        filter.provider = req.get_param_value("provider");
        if (!parse_timestamp_param(req, res, "since", filter.since) ||
            !parse_timestamp_param(req, res, "until", filter.until)) {
            return;
        }
        send_json(res, 200, usage::Store::for_config_dir().query(filter, limit, config::prices()));
    });

    // POST /usage/events: an event from another client (ai.nvim). The server
    // stamps the time and defaults the source; a body that carries a text field
    // (prompt, messages, ...) is refused, see usage::parse_incoming.
    server.Post("/usage/events", [](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() > kMaxUsageEventBody) {
            send_error(res, 413, "a usage event must not exceed 4 KiB");
            return;
        }
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        usage::Event event;
        config::Status st = usage::parse_incoming(body, event);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        std::string error;
        if (!usage::Store::for_config_dir().append(event, &error)) {
            send_error(res, 500, "cannot record the usage event: " + error);
            return;
        }
        send_json(res, 200, json{{"ok", true}});
    });

    // ----- keys (presence only, never a value) ------------------------------

    server.Get("/keys", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, secrets::keys_json());
    });

    server.Put(R"(/keys/([A-Za-z0-9_:.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1].str();
        if (!is_provider_key(name)) {
            send_error(res, 400, "not a provider key name (OPENAI_API_KEY, ANTHROPIC_API_KEY, GEMINI_API_KEY)");
            return;
        }
        json body;
        if (!parse_object(req, res, body)) {
            return;
        }
        if (!body.contains("value") || !body["value"].is_string()) {
            send_error(res, 400, "'value' (string) is required");
            return;
        }
        config::Status st = secrets::set(name, body["value"].get<std::string>());
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 200, secrets::keys_json());
    });

    server.Delete(R"(/keys/([A-Za-z0-9_:.\-]+))", [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1].str();
        if (!is_provider_key(name)) {
            send_error(res, 400, "not a provider key name (OPENAI_API_KEY, ANTHROPIC_API_KEY, GEMINI_API_KEY)");
            return;
        }
        config::Status st = secrets::remove(name);
        if (!st.ok()) {
            send_error(res, st.http, st.message);
            return;
        }
        send_json(res, 200, secrets::keys_json());
    });
}

}  // namespace ai_desktop::api
