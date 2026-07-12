// alter_ego_ai_service.hpp — ports src/services/alterEgoAIService.js (1:1).
//
// AlterEgo AI Service — real AI integration for Alter Ego 2.0.
//
// Generates an AI response based on an alter ego's personality. Providers are
// tried in order with graceful degradation:
//   Gemini (GEMINI_API_KEY) -> OpenAI (OPENAI_API_KEY) -> smart template.
//
// Mirrors the JS module singleton: a process-wide AlterEgoAIService::instance()
// with a convenience free function pulse::alterEgoAI(). Prompt construction and
// persona-detection logic are preserved exactly from the JS source.
#pragma once
#include <string>
#include <vector>

namespace pulse {

// One turn of conversation history (context.history[i] in the JS).
struct AlterEgoMessage {
  std::string role;     // "ego" maps to the assistant/model role; anything else => user
  std::string content;  // message text
};

// Additional context for a generation call (context = {} in the JS).
struct AlterEgoContext {
  std::vector<AlterEgoMessage> history;
};

class AlterEgoAIService {
public:
  static AlterEgoAIService& instance();

  // Mirrors AI_CONFIG from the JS source.
  struct Config {
    std::string geminiApiKey;                          // GEMINI_API_KEY || null
    std::string openaiApiKey;                          // OPENAI_API_KEY || null
    std::string geminiModel  = "gemini-1.5-flash";     // GEMINI_MODEL
    std::string openaiModel  = "gpt-3.5-turbo";        // OPENAI_MODEL
    int         maxTokens    = 150;                    // MAX_TOKENS
    double      temperature  = 0.8;                    // TEMPERATURE
  };

  const Config& config() const { return cfg_; }

  // generateAIResponse(systemPrompt, userMessage, context = {}) -> string.
  // Tries Gemini, then OpenAI, then the template fallback (never throws).
  std::string generateAIResponse(const std::string& systemPrompt,
                                 const std::string& userMessage,
                                 const AlterEgoContext& context = {});

  // getActiveProvider() -> "gemini" | "openai" | "template".
  std::string getActiveProvider() const;

private:
  AlterEgoAIService();

  // callGemini / callOpenAI — throw std::runtime_error on failure (caught by
  // generateAIResponse so it can fall through to the next provider).
  std::string callGemini(const std::string& systemPrompt,
                         const std::string& userMessage,
                         const AlterEgoContext& context);
  std::string callOpenAI(const std::string& systemPrompt,
                         const std::string& userMessage,
                         const AlterEgoContext& context);

  // generateTemplateResponse — rule-based fallback, never throws.
  std::string generateTemplateResponse(const std::string& systemPrompt,
                                       const std::string& message,
                                       const AlterEgoContext& context);

  Config cfg_;
};

// Convenience free function matching the JS module singleton usage.
inline AlterEgoAIService& alterEgoAI() { return AlterEgoAIService::instance(); }

} // namespace pulse
