#pragma once

#include <functional>
#include <string>

#include "model_client.hpp"

// Minimal client for Ollama's local REST API (POST /api/generate), used to
// back ai-desktop's /ask and /ask/stream endpoints (phase 1 of the
// agent framework: a plain Ollama REST client in C++).
//
// Returns the shared ai_desktop::AskResult (see model_client.hpp) rather than a
// private struct: main.cpp's model-name-prefix router dispatches to this or
// one of the cloud clients (openai/anthropic/gemini) and needs one uniform
// result shape to build its HTTP response from, not four near-identical
// ones.
namespace ai_desktop::ollama {

// Base URL of the local Ollama daemon. Reads AI_DESKTOP_OLLAMA_HOST, falls back
// to http://127.0.0.1:11434 (Ollama's own default). Deliberately not named
// OLLAMA_HOST -- that variable already means "address the Ollama *server*
// binds to" (e.g. commonly "0.0.0.0:11434") on machines running Ollama,
// which is a different thing from "address a client should connect to".
std::string host();

// Model used when the caller doesn't specify one. Reads
// AI_DESKTOP_DEFAULT_MODEL, falls back to a small local model.
std::string default_model();

// Blocking, non-streaming call to /api/generate.
ai_desktop::AskResult ask(const std::string& prompt,
              const std::string& system,
              const std::string& model,
              int timeout_ms);

// Streaming call to /api/generate. Invokes on_delta once per text chunk as
// it arrives. Returns an empty string on success, or an error message.
std::string ask_stream(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        int timeout_ms,
                        const std::function<void(const std::string&)>& on_delta);

}  // namespace ai_desktop::ollama
