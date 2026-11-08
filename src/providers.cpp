#include "providers.hpp"

#include "catalog_parse.hpp"

namespace ai_desktop::providers {

const std::vector<Cloud>& cloud() {
    static const std::vector<Cloud> table = {
        {"anthropic",
         "ANTHROPIC_API_KEY",
         "AI_DESKTOP_PREFIXES_CLAUDE",
         {"claude-"},
         "https://api.anthropic.com",
         "/v1/models?limit=1000",
         {{"x-api-key", "{key}"}, {"anthropic-version", "2023-06-01"}},
         &catalog::parse_anthropic_models,
         {{"claude-opus-5-5", "Claude Opus 5.5"},
          {"claude-sonnet-5-5", "Claude Sonnet 5.5"},
          {"claude-haiku-4-5-20251001", "Claude Haiku 4.5"}}},
        {"openai",
         "OPENAI_API_KEY",
         "AI_DESKTOP_PREFIXES_OPENAI",
         {"gpt-", "o1-", "o3-"},
         "https://api.openai.com",
         "/v1/models",
         {{"Authorization", "Bearer {key}"}},
         &catalog::parse_openai_models,
         {{"gpt-4o", "GPT-4o"}, {"gpt-4o-mini", "GPT-4o mini"}, {"o3-mini", "o3-mini"}}},
        {"gemini",
         "GEMINI_API_KEY",
         "AI_DESKTOP_PREFIXES_GEMINI",
         {"gemini-"},
         "https://generativelanguage.googleapis.com",
         "/v1beta/models?pageSize=1000",
         {{"x-goog-api-key", "{key}"}},
         &catalog::parse_gemini_models,
         {{"gemini-2.5-pro", "Gemini 2.5 Pro"}, {"gemini-2.5-flash", "Gemini 2.5 Flash"}}},
    };
    return table;
}

const Cloud* find(std::string_view name) {
    for (const auto& provider : cloud()) {
        if (provider.name == name) {
            return &provider;
        }
    }
    return nullptr;
}

bool known(std::string_view name) {
    return name == kLocal || name == kCompatible || find(name) != nullptr;
}

std::vector<std::pair<std::string, std::string>> list_headers(const Cloud& provider, const std::string& key) {
    static const std::string placeholder = "{key}";
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(provider.list_headers.size());
    for (const auto& [name, value] : provider.list_headers) {
        std::string filled = value;
        const auto at = filled.find(placeholder);
        if (at != std::string::npos) {
            filled.replace(at, placeholder.size(), key);
        }
        out.emplace_back(name, std::move(filled));
    }
    return out;
}

}  // namespace ai_desktop::providers
