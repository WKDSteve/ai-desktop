#include "session.hpp"

#include <deque>
#include <map>
#include <mutex>

#include "control_secret.hpp"

namespace ai_desktop::session {

namespace {

using Clock = std::chrono::steady_clock;

struct Ticket {
    Clock::time_point expires;  // max(): the console's ticket
    bool once = true;
};

std::mutex g_mutex;
std::map<std::string, Ticket> g_tickets;
std::deque<std::string> g_tokens;
std::string g_console_ticket;

// Equal in constant time for the length of `want` (the lengths themselves are not secret: always 64 hex digits).
bool same(const std::string& got, const std::string& want) {
    if (want.empty() || got.size() != want.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        diff |= static_cast<unsigned char>(got[i] ^ want[i]);
    }
    return diff == 0;
}

void drop_expired(Clock::time_point now) {
    for (auto it = g_tickets.begin(); it != g_tickets.end();) {
        it = it->second.expires <= now ? g_tickets.erase(it) : std::next(it);
    }
}

std::string add_token_locked() {
    std::string token = control_secret::generate();
    if (token.empty()) {
        return {};
    }
    g_tokens.push_back(token);
    while (g_tokens.size() > kMaxTokens) {
        g_tokens.pop_front();
    }
    return token;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

// `path` is `prefix` or lies below it.
bool under(const std::string& path, const std::string& prefix) {
    return path == prefix || starts_with(path, prefix + "/");
}

}  // namespace

std::string new_ticket(std::chrono::seconds lifetime) {
    std::string ticket = control_secret::generate();
    if (ticket.empty()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = Clock::now();
    drop_expired(now);
    g_tickets[ticket] = Ticket{now + lifetime, true};
    return ticket;
}

std::string console_ticket() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_console_ticket.empty()) {
        g_console_ticket = control_secret::generate();
        if (!g_console_ticket.empty()) {
            g_tickets[g_console_ticket] = Ticket{Clock::time_point::max(), false};
        }
    }
    return g_console_ticket;
}

std::string redeem(const std::string& ticket) {
    std::lock_guard<std::mutex> lock(g_mutex);
    drop_expired(Clock::now());
    // Every ticket is compared (no early end, no lookup by the text): the time does not tell which one was close.
    auto found = g_tickets.end();
    for (auto it = g_tickets.begin(); it != g_tickets.end(); ++it) {
        if (same(ticket, it->first)) {
            found = it;
        }
    }
    if (found == g_tickets.end()) {
        return {};
    }
    if (found->second.once) {
        g_tickets.erase(found);
    }
    return add_token_locked();
}

std::string new_token() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return add_token_locked();
}

bool token_matches(const std::string& authorization) {
    static const std::string kPrefix = "Bearer ";
    if (!starts_with(authorization, kPrefix)) {
        return false;
    }
    const std::string got = authorization.substr(kPrefix.size());
    std::lock_guard<std::mutex> lock(g_mutex);
    bool ok = false;
    for (const auto& token : g_tokens) {
        ok = same(got, token) || ok;
    }
    return ok;
}

bool needs_proof(const std::string& method, const std::string& path) {
    if (under(path, "/jobs")) {
        if (method == "POST" && path == "/jobs") {
            return false;  // the control secret, checked by the route itself (and no Origin at all)
        }
        return true;
    }
    if (under(path, "/usage")) {
        return !(method == "POST" && path == "/usage/events");  // ai.nvim's usage events
    }
    for (const char* prefix : {"/settings", "/keys", "/models/custom", "/events", "/decision"}) {
        if (under(path, prefix)) {
            return true;
        }
    }
    return false;
}

void reset_for_tests() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tickets.clear();
    g_tokens.clear();
    g_console_ticket.clear();
}

}  // namespace ai_desktop::session
