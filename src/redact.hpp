#pragma once

#include <regex>
#include <string>

namespace ai_desktop {

// `text` with anything shaped like a provider key (sk-..., AIza...) replaced by "<key>", masked or not. For the
// error text of a provider, which may echo the key it refused: OpenAI answers a wrong key with "Incorrect API key
// provided: sk-proj-****...XMoA", the masked key still showing its last characters, and that message reaches the
// dashboard, /health and the answer of /ask. Anthropic (sk-ant-) and Gemini (AIza) keys get the same treatment in
// case a provider ever echoes one. `\b`: a word that merely contains "sk-" (risk-free) stays.
inline std::string redact_keys(const std::string& text) {
    static const std::regex key(R"(\b(sk-|AIza)[A-Za-z0-9_*\-]+)");
    return std::regex_replace(text, key, "<key>");
}

}  // namespace ai_desktop
