#pragma once

// The program was called loomAI before it became ai-desktop. A setting the person made under the old name keeps
// working: the environment variables LOOMAI_*, the configuration folder and the Credential Manager entries are looked
// up under the new name first and under the old one when the new one is not there. Nothing is copied or deleted, so a
// person who goes back to an old build still finds everything where it was.

#include <cstdlib>
#include <cstring>
#include <string>

namespace ai_desktop::legacy {

// The old name of a variable AI_DESKTOP_X (LOOMAI_X); empty for any other name.
inline std::string old_env_name(const char* name) {
    static constexpr char kNew[] = "AI_DESKTOP_";
    static constexpr size_t kNewLength = sizeof(kNew) - 1;
    if (!name || std::strncmp(name, kNew, kNewLength) != 0) {
        return std::string();
    }
    return std::string("LOOMAI_") + (name + kNewLength);
}

// Non-empty value of an environment variable, or nullptr (an empty variable counts as unset).
inline const char* env_nonempty(const char* name) {
    const char* value = name ? std::getenv(name) : nullptr;
    return (value && *value) ? value : nullptr;
}

// The variable under its new name, else under the old one (LOOMAI_X for AI_DESKTOP_X); nullptr when neither is set.
inline const char* env(const char* name) {
    if (const char* value = env_nonempty(name)) {
        return value;
    }
    const std::string old_name = old_env_name(name);
    return old_name.empty() ? nullptr : env_nonempty(old_name.c_str());
}

}  // namespace ai_desktop::legacy
