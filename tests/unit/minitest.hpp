#pragma once

// Tiny test harness: no dependency, self-registering TEST() cases, CHECK()
// macros that report and keep going. Enough for the pure-logic units; the HTTP
// layer is tested from Node (tests/*.test.mjs).

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace minitest {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { cases().push_back({name, std::move(body)}); }
};

inline void fail(const char* file, int line, const std::string& message) {
    ++failures();
    std::cerr << "    FAIL " << file << ":" << line << "  " << message << "\n";
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream out;
        out << expr_a << " == " << expr_b << "  (got '" << a << "' vs '" << b << "')";
        fail(file, line, out.str());
    }
}

inline void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());  // an empty value removes the variable
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

// Removes a directory tree, also files that are read-only (git writes its objects so on Windows); never throws.
inline void remove_tree(const std::filesystem::path& dir) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code ignored;
        std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ignored);
    }
    std::filesystem::remove_all(dir, ec);
}

// Fresh, empty directory under the system temp dir; removed by the caller (remove_tree). The name holds the time of
// this run, so a directory a run before could not remove (the tests share one machine with the CI runner) is never
// the one of this run.
inline std::filesystem::path make_temp_dir(const std::string& tag) {
    static int counter = 0;
    static const auto run = std::chrono::system_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               ("ai-desktop-unit-" + tag + "-" + std::to_string(run) + "-" + std::to_string(++counter));
    remove_tree(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace minitest

#define TEST(name)                                                                  \
    static void test_##name();                                                      \
    static minitest::Registrar registrar_##name(#name, test_##name);                \
    static void test_##name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) minitest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");        \
    } while (0)

#define CHECK_EQ(a, b) minitest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
