#include <string>
#include <thread>

#include "minitest.hpp"
#include "session.hpp"

namespace session = ai_desktop::session;

TEST(session_a_ticket_gives_one_token_once) {
    session::reset_for_tests();
    const std::string ticket = session::new_ticket();
    CHECK_EQ(ticket.size(), static_cast<size_t>(64));
    const std::string token = session::redeem(ticket);
    CHECK_EQ(token.size(), static_cast<size_t>(64));
    CHECK(token != ticket);
    CHECK(session::token_matches("Bearer " + token));
    CHECK_EQ(session::redeem(ticket), std::string(""));  // used up
    CHECK(!session::token_matches("Bearer " + ticket));  // a ticket is no token
}

TEST(session_refuses_unknown_tickets_and_tokens) {
    session::reset_for_tests();
    CHECK_EQ(session::redeem(""), std::string(""));
    CHECK_EQ(session::redeem(std::string(64, 'a')), std::string(""));
    const std::string token = session::new_token();
    CHECK(!session::token_matches(token));                    // no "Bearer "
    CHECK(!session::token_matches("Bearer "));
    CHECK(!session::token_matches("Bearer " + token + "0"));  // longer
    CHECK(!session::token_matches("Bearer " + token.substr(1)));
    CHECK(!session::token_matches("bearer " + token));
}

TEST(session_a_ticket_expires) {
    session::reset_for_tests();
    const std::string ticket = session::new_ticket(std::chrono::seconds(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(session::redeem(ticket), std::string(""));
}

TEST(session_the_console_ticket_works_again) {
    session::reset_for_tests();
    const std::string ticket = session::console_ticket();
    CHECK_EQ(session::console_ticket(), ticket);
    const std::string a = session::redeem(ticket);
    const std::string b = session::redeem(ticket);
    CHECK(!a.empty() && !b.empty() && a != b);
    CHECK(session::token_matches("Bearer " + a) && session::token_matches("Bearer " + b));
}

TEST(session_keeps_the_newest_tokens) {
    session::reset_for_tests();
    const std::string first = session::new_token();
    for (std::size_t i = 0; i < session::kMaxTokens; ++i) {
        session::new_token();
    }
    CHECK(!session::token_matches("Bearer " + first));
}

TEST(session_names_the_routes_that_want_a_proof) {
    for (const auto& [method, path] : std::initializer_list<std::pair<const char*, const char*>>{
             {"GET", "/jobs"},
             {"POST", "/jobs/0123456789abcdef/run"},
             {"POST", "/jobs/0123456789abcdef/cancel"},
             {"DELETE", "/jobs/0123456789abcdef"},
             {"GET", "/jobs/0123456789abcdef"},
             {"GET", "/jobs/0123456789ABCDEF"},
             {"GET", "/jobs/"},
             {"GET", "/settings"},
             {"PUT", "/settings"},
             {"GET", "/settings/"},
             {"GET", "/keys"},
             {"PUT", "/keys/openai"},
             {"DELETE", "/keys/openai"},
             {"GET", "/models/custom"},
             {"POST", "/models/custom"},
             {"DELETE", "/models/custom/x"},
             {"GET", "/usage"},
             {"GET", "/usage/events"},
             {"GET", "/events"},
             {"POST", "/decision"},
         }) {
        if (!session::needs_proof(method, path)) {
            minitest::fail(__FILE__, __LINE__, std::string(method) + " " + path + " should want a proof");
        }
    }
    for (const auto& [method, path] : std::initializer_list<std::pair<const char*, const char*>>{
             {"GET", "/"},
             {"GET", "/app.js"},
             {"GET", "/health"},
             {"GET", "/models"},
             {"GET", "/models/info"},
             {"POST", "/ask"},
             {"POST", "/ask/stream"},
             {"POST", "/usage/events"},
             {"POST", "/session"},
             {"POST", "/session/ticket"},
             {"POST", "/jobs"},
             {"GET", "/jobsx"},
         }) {
        if (session::needs_proof(method, path)) {
            minitest::fail(__FILE__, __LINE__, std::string(method) + " " + path + " should be open");
        }
    }
}
