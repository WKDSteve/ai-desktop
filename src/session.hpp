#pragma once

#include <chrono>
#include <string>

// The login of the management API (docs/JOBS.md, "Threat model"). 127.0.0.1 is reachable by every account of the
// machine, and a program can send any Origin, so a route with an effect (or one that reads jobs or settings) wants a
// proof only the person can have:
//
// - a program sends the control secret (`Authorization: Bearer <control.secret>`), as for POST /jobs;
// - the dashboard sends a session token (`Authorization: Bearer <token>`), which it got for a ticket.
//
// A ticket comes from ai-desktop itself (in the address it opens for a new job, in the link it prints on its console)
// or from a program with the control secret (POST /session/ticket: the hub's tile, a family app's dashboard link). It
// travels in the fragment of the address (`#ticket=<hex>`), which a browser never sends to a server, and the page takes
// it out of the address once it has its token. The token is kept in the page's localStorage: unlike a cookie, that is
// separate per port, so a server of another account on another port of 127.0.0.1 never gets it.
//
// Everything here is new at every start of the server (tokens and tickets live in memory only) and compared in
// constant time.
namespace ai_desktop::session {

// How long a ticket of POST /session/ticket or of a job's address may wait for the page.
constexpr std::chrono::seconds kTicketLifetime{120};
// The most tokens kept at once; the oldest goes first (every opened dashboard tab takes one).
constexpr std::size_t kMaxTokens = 32;

// A one-time ticket, valid for `lifetime`; "" when the system gives no random bytes.
std::string new_ticket(std::chrono::seconds lifetime = kTicketLifetime);

// The ticket of the console link: made once, valid as long as the server runs, more than once (the person may click
// the link again after closing the browser). It is only printed to a console, never to a file.
std::string console_ticket();

// Gives a new session token for a valid ticket (a one-time ticket is used up), else "".
std::string redeem(const std::string& ticket);

// A new session token without a ticket: for a caller that proved itself with the control secret.
std::string new_token();

// Is `authorization` ("Bearer <hex>") one of the session tokens?
bool token_matches(const std::string& authorization);

// Does a request of `method` on `path` want the control secret or a session token? The routes of the ai.nvim
// contract (/health, /ask, /ask/stream, /usage/events), the model list, the static files of the dashboard and the
// session routes themselves do not; POST /jobs checks the control secret itself.
bool needs_proof(const std::string& method, const std::string& path);

// Forgets every ticket and token (the unit tests).
void reset_for_tests();

}  // namespace ai_desktop::session
