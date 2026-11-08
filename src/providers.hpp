#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

// The cloud providers ai-desktop knows, as one table: what a provider is called,
// which key it needs, which model names route to it, where its model list is
// and how that list is read, and what is suggested while it has no key. A new
// cloud provider is one entry here (plus its client and its entry in the
// backends of main.cpp); nothing else compares provider names.
//
// "ollama" (local, the fallback of routing) and "openai-compatible" (a custom
// model with its own address) are not cloud providers: they have no key name,
// no prefixes and no list of their own here.
namespace ai_desktop::providers {

inline constexpr std::string_view kLocal = "ollama";
inline constexpr std::string_view kCompatible = "openai-compatible";

struct Cloud {
    std::string name;        // "anthropic"
    std::string key_name;    // the secret it needs: "ANTHROPIC_API_KEY"
    std::string prefix_env;  // overrides the prefixes, comma separated: "AI_DESKTOP_PREFIXES_CLAUDE"
    std::vector<std::string> default_prefixes;
    // GET <list_origin><list_path> lists the models. Each header value may hold
    // "{key}", replaced by the key; the key never appears anywhere else.
    std::string list_origin;
    std::string list_path;
    std::vector<std::pair<std::string, std::string>> list_headers;
    nlohmann::json (*parse_list)(const nlohmann::json& response) = nullptr;
    // Suggested while there is no key: {id, name}.
    std::vector<std::pair<std::string, std::string>> presets;
};

// The cloud providers in routing order: the first whose prefix matches a model
// name gets it.
const std::vector<Cloud>& cloud();

// The cloud provider of this name, or nullptr.
const Cloud* find(std::string_view name);

// Whether `name` is a provider a custom model may name: a cloud provider,
// "ollama" or "openai-compatible".
bool known(std::string_view name);

// The headers of the model-list request of `provider`, with the key put in.
std::vector<std::pair<std::string, std::string>> list_headers(const Cloud& provider, const std::string& key);

}  // namespace ai_desktop::providers
