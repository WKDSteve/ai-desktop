#include <set>

#include "catalog_parse.hpp"
#include "minitest.hpp"
#include "providers.hpp"

namespace providers = ai_desktop::providers;

TEST(the_cloud_providers_are_one_table_in_routing_order) {
    const auto& table = providers::cloud();
    CHECK_EQ(table.size(), size_t{3});
    CHECK_EQ(table[0].name, std::string("anthropic"));
    CHECK_EQ(table[1].name, std::string("openai"));
    CHECK_EQ(table[2].name, std::string("gemini"));
    std::set<std::string> names;
    for (const auto& cloud : table) {
        names.insert(cloud.name);
        // Every entry is complete: a key, prefixes, a model list and how to read it.
        CHECK(!cloud.key_name.empty());
        CHECK(!cloud.prefix_env.empty());
        CHECK(!cloud.default_prefixes.empty());
        CHECK(cloud.list_origin.rfind("https://", 0) == 0);
        CHECK(!cloud.list_path.empty());
        CHECK(cloud.parse_list != nullptr);
        CHECK(!cloud.presets.empty());
    }
    CHECK_EQ(names.size(), table.size());
}

TEST(find_and_known_name_the_providers_a_model_may_have) {
    CHECK_EQ(providers::find("openai")->key_name, std::string("OPENAI_API_KEY"));
    CHECK(providers::find("ollama") == nullptr);
    CHECK(providers::find("openai-compatible") == nullptr);
    CHECK(providers::find("") == nullptr);
    for (const char* name : {"ollama", "openai-compatible", "anthropic", "openai", "gemini"}) {
        CHECK(providers::known(name));
    }
    for (const char* name : {"", "OpenAI", "mistral", "ollama "}) {
        CHECK(!providers::known(name));
    }
}

TEST(the_key_goes_only_where_the_header_asks_for_it) {
    const auto& anthropic = *providers::find("anthropic");
    const auto headers = providers::list_headers(anthropic, "sk-test");
    CHECK_EQ(headers.size(), size_t{2});
    CHECK_EQ(headers[0].first, std::string("x-api-key"));
    CHECK_EQ(headers[0].second, std::string("sk-test"));
    CHECK_EQ(headers[1].second, std::string("2023-06-01"));
    const auto openai = providers::list_headers(*providers::find("openai"), "k");
    CHECK_EQ(openai[0].second, std::string("Bearer k"));
}

TEST(presets_come_from_the_table) {
    const auto list = ai_desktop::catalog::presets("anthropic");
    CHECK_EQ(list.size(), providers::find("anthropic")->presets.size());
    CHECK_EQ(list[0]["provider"].get<std::string>(), std::string("anthropic"));
    CHECK(list[0]["details"]["preset"].get<bool>());
    CHECK(!list[0]["available"].get<bool>());
    CHECK(ai_desktop::catalog::presets("ollama").empty());
}
