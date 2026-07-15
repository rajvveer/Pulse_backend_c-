// alter_ego_ai_service.cc — implementation, ports src/services/alterEgoAIService.js.
//
// Provider order with graceful degradation: Gemini -> OpenAI -> smart template.
// HTTP calls to the external AI APIs use Drogon's HttpClient. JSON bodies are
// built/parsed with JsonCpp (Json::Value) to mirror the JS request/response
// shapes exactly.
#include "pulse/services/alter_ego_ai_service.hpp"
#include "pulse/services/http_client.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <random>
#include <regex>
#include <stdexcept>

namespace pulse {

namespace {

// ── small helpers ──────────────────────────────────────────────────────────

// Trim leading/trailing ASCII whitespace (mirrors JS String.prototype.trim for
// the response text we get back from the providers).
std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string toLower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// systemPrompt.includes(substr)
bool includes(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// pick(arr) = arr[Math.floor(Math.random() * arr.length)]
const std::string& pick(const std::vector<std::string>& arr) {
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<size_t> d(0, arr.empty() ? 0 : arr.size() - 1);
  return arr[d(rng)];
}

// Serialize a Json::Value to a compact JSON string (no extra whitespace),
// matching JSON.stringify(body).
std::string toJsonString(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

bool parseJson(const std::string& body, Json::Value& out) {
  Json::CharReaderBuilder b;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  return reader->parse(body.c_str(), body.c_str() + body.size(), &out, &errs);
}

// Split an https URL into ("https://host" , "/path?query") so it can be used
// with drogon::HttpClient::newHttpClient(base) + req->setPath / query.
struct SplitUrl { std::string base; std::string path; };
SplitUrl splitUrl(const std::string& url) {
  SplitUrl r;
  const std::string sep = "://";
  size_t schemeEnd = url.find(sep);
  if (schemeEnd == std::string::npos) { r.base = url; r.path = "/"; return r; }
  size_t hostStart = schemeEnd + sep.size();
  size_t pathStart = url.find('/', hostStart);
  if (pathStart == std::string::npos) {
    r.base = url;
    r.path = "/";
  } else {
    r.base = url.substr(0, pathStart);
    r.path = url.substr(pathStart);
  }
  return r;
}

} // namespace

// ── construction / singleton ───────────────────────────────────────────────

AlterEgoAIService::AlterEgoAIService() {
  // AI_CONFIG: read from env, default to empty (JS used `|| null`).
  cfg_.geminiApiKey = pulse::config().env("GEMINI_API_KEY");
  cfg_.openaiApiKey = pulse::config().env("OPENAI_API_KEY");
  cfg_.geminiModel  = "gemini-1.5-flash";
  cfg_.openaiModel  = "gpt-3.5-turbo";
  cfg_.maxTokens    = 150;
  cfg_.temperature  = 0.8;
}

AlterEgoAIService& AlterEgoAIService::instance() {
  static AlterEgoAIService s;
  return s;
}

// ── generateAIResponse ─────────────────────────────────────────────────────

std::string AlterEgoAIService::generateAIResponse(const std::string& systemPrompt,
                                                  const std::string& userMessage,
                                                  const AlterEgoContext& context) {
  // Try providers in order of preference.
  if (!cfg_.geminiApiKey.empty()) {
    try {
      return callGemini(systemPrompt, userMessage, context);
    } catch (const std::exception& e) {
      pulse::log::error("[AlterEgoAI] Gemini failed, falling back: {}", e.what());
    }
  }

  if (!cfg_.openaiApiKey.empty()) {
    try {
      return callOpenAI(systemPrompt, userMessage, context);
    } catch (const std::exception& e) {
      pulse::log::error("[AlterEgoAI] OpenAI failed, falling back: {}", e.what());
    }
  }

  // Fallback to smart template mode.
  return generateTemplateResponse(systemPrompt, userMessage, context);
}

// ── Google Gemini ──────────────────────────────────────────────────────────

std::string AlterEgoAIService::callGemini(const std::string& systemPrompt,
                                          const std::string& userMessage,
                                          const AlterEgoContext& context) {
  // url = https://generativelanguage.googleapis.com/v1beta/models/{MODEL}:generateContent?key={KEY}
  const std::string url =
      "https://generativelanguage.googleapis.com/v1beta/models/" + cfg_.geminiModel +
      ":generateContent?key=" + cfg_.geminiApiKey;

  // contents: [...history, { role: 'user', parts: [{ text: userMessage }] }]
  Json::Value contents(Json::arrayValue);
  for (const auto& msg : context.history) {
    Json::Value item(Json::objectValue);
    item["role"] = (msg.role == "ego") ? "model" : "user";
    Json::Value parts(Json::arrayValue);
    Json::Value part(Json::objectValue);
    part["text"] = msg.content;
    parts.append(part);
    item["parts"] = parts;
    contents.append(item);
  }
  {
    Json::Value userItem(Json::objectValue);
    userItem["role"] = "user";
    Json::Value parts(Json::arrayValue);
    Json::Value part(Json::objectValue);
    part["text"] = userMessage;
    parts.append(part);
    userItem["parts"] = parts;
    contents.append(userItem);
  }

  Json::Value body(Json::objectValue);
  {
    Json::Value sys(Json::objectValue);
    Json::Value sysParts(Json::arrayValue);
    Json::Value sysPart(Json::objectValue);
    sysPart["text"] = systemPrompt;
    sysParts.append(sysPart);
    sys["parts"] = sysParts;
    body["systemInstruction"] = sys;
  }
  body["contents"] = contents;
  {
    Json::Value gen(Json::objectValue);
    gen["maxOutputTokens"] = cfg_.maxTokens;
    gen["temperature"]     = cfg_.temperature;
    gen["topP"]            = 0.9;
    body["generationConfig"] = gen;
  }
  {
    Json::Value safety(Json::arrayValue);
    const char* categories[] = {
        "HARM_CATEGORY_HARASSMENT", "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT", "HARM_CATEGORY_DANGEROUS_CONTENT"};
    for (const char* cat : categories) {
      Json::Value s(Json::objectValue);
      s["category"]  = cat;
      s["threshold"] = "BLOCK_ONLY_HIGH";
      safety.append(s);
    }
    body["safetySettings"] = safety;
  }

  // POST to URL with Content-Type: application/json.
  SplitUrl su = splitUrl(url);
  auto client = drogon::HttpClient::newHttpClient(su.base);
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Post);
  req->setPath(su.path);
  req->addHeader("Content-Type", "application/json");
  req->setBody(toJsonString(body));

  auto result = client->sendRequest(
      req, pulse::services::outboundHttpTimeoutSeconds());
  if (result.first != drogon::ReqResult::Ok || !result.second) {
    throw std::runtime_error("Gemini API error: request failed");
  }
  auto resp = result.second;
  int status = static_cast<int>(resp->getStatusCode());

  if (status < 200 || status >= 300) {
    // const errorData = await response.json().catch(() => ({}))
    std::string errorData = "{}";
    Json::Value parsed;
    if (parseJson(std::string(resp->getBody()), parsed)) {
      errorData = toJsonString(parsed);
    }
    throw std::runtime_error("Gemini API error: " + std::to_string(status) + " - " + errorData);
  }

  Json::Value data;
  if (!parseJson(std::string(resp->getBody()), data)) {
    throw std::runtime_error("Empty Gemini response");
  }

  // data?.candidates?.[0]?.content?.parts?.[0]?.text
  std::string text;
  if (data.isMember("candidates") && data["candidates"].isArray() &&
      data["candidates"].size() > 0) {
    const Json::Value& cand = data["candidates"][0];
    if (cand.isMember("content") && cand["content"].isMember("parts") &&
        cand["content"]["parts"].isArray() && cand["content"]["parts"].size() > 0) {
      const Json::Value& part = cand["content"]["parts"][0];
      if (part.isMember("text") && part["text"].isString()) {
        text = part["text"].asString();
      }
    }
  }

  if (text.empty()) throw std::runtime_error("Empty Gemini response");
  return trim(text);
}

// ── OpenAI ─────────────────────────────────────────────────────────────────

std::string AlterEgoAIService::callOpenAI(const std::string& systemPrompt,
                                          const std::string& userMessage,
                                          const AlterEgoContext& context) {
  const std::string url = "https://api.openai.com/v1/chat/completions";

  // messages = [ {system}, ...history, {user} ]
  Json::Value messages(Json::arrayValue);
  {
    Json::Value sys(Json::objectValue);
    sys["role"]    = "system";
    sys["content"] = systemPrompt;
    messages.append(sys);
  }
  for (const auto& msg : context.history) {
    Json::Value item(Json::objectValue);
    item["role"]    = (msg.role == "ego") ? "assistant" : "user";
    item["content"] = msg.content;
    messages.append(item);
  }
  {
    Json::Value usr(Json::objectValue);
    usr["role"]    = "user";
    usr["content"] = userMessage;
    messages.append(usr);
  }

  Json::Value body(Json::objectValue);
  body["model"]       = cfg_.openaiModel;
  body["messages"]    = messages;
  body["max_tokens"]  = cfg_.maxTokens;
  body["temperature"] = cfg_.temperature;

  SplitUrl su = splitUrl(url);
  auto client = drogon::HttpClient::newHttpClient(su.base);
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Post);
  req->setPath(su.path);
  req->addHeader("Content-Type", "application/json");
  req->addHeader("Authorization", "Bearer " + cfg_.openaiApiKey);
  req->setBody(toJsonString(body));

  auto result = client->sendRequest(
      req, pulse::services::outboundHttpTimeoutSeconds());
  if (result.first != drogon::ReqResult::Ok || !result.second) {
    throw std::runtime_error("OpenAI API error: request failed");
  }
  auto resp = result.second;
  int status = static_cast<int>(resp->getStatusCode());

  if (status < 200 || status >= 300) {
    throw std::runtime_error("OpenAI API error: " + std::to_string(status));
  }

  Json::Value data;
  if (!parseJson(std::string(resp->getBody()), data)) {
    throw std::runtime_error("Empty OpenAI response");
  }

  // data?.choices?.[0]?.message?.content
  std::string text;
  if (data.isMember("choices") && data["choices"].isArray() &&
      data["choices"].size() > 0) {
    const Json::Value& choice = data["choices"][0];
    if (choice.isMember("message") && choice["message"].isMember("content") &&
        choice["message"]["content"].isString()) {
      text = choice["message"]["content"].asString();
    }
  }

  if (text.empty()) throw std::runtime_error("Empty OpenAI response");
  return trim(text);
}

// ── Smart template fallback ────────────────────────────────────────────────

std::string AlterEgoAIService::generateTemplateResponse(const std::string& systemPrompt,
                                                        const std::string& message,
                                                        const AlterEgoContext& /*context*/) {
  const std::string lower = toLower(message);

  // Personality-aware responses.
  const bool isFromFunny =
      includes(systemPrompt, "funny") || includes(systemPrompt, "witty");
  const bool isFromChill =
      includes(systemPrompt, "chill") || includes(systemPrompt, "relaxed");
  const bool isFromProfessional =
      includes(systemPrompt, "professional") || includes(systemPrompt, "formal");
  const bool isFromMysterious =
      includes(systemPrompt, "mysterious") || includes(systemPrompt, "enigmatic");

  // Intent regexes (case-insensitive against the original `message`; we match on
  // the lowercased copy with case-insensitive flag to mirror the JS /.../i).
  static const std::regex reGreeting(
      R"(\b(hi|hey|hello|sup|yo|what'?s up)\b)", std::regex::icase);
  static const std::regex reThanks(
      R"(\b(thanks|thank you|thx|ty)\b)", std::regex::icase);
  static const std::regex rePositive(
      R"(\b(love|amazing|awesome|great|perfect|beautiful)\b)", std::regex::icase);
  static const std::regex reNegative(
      R"(\b(sad|miss|lonely|bad day|tough)\b)", std::regex::icase);

  // Greeting
  if (std::regex_search(lower, reGreeting)) {
    if (isFromFunny)
      return pick({"Yooo what's good! \xF0\x9F\x98\x84",
                   "Hey hey! Ready to drop some vibes? \xF0\x9F\x94\xA5",
                   "Sup! You caught me mid-vibe \xF0\x9F\x98\x8E"});
    if (isFromChill)
      return pick({"Heyyy, what's up \xF0\x9F\x8C\x8A",
                   "Yo, good vibes only \xE2\x9C\x8C\xEF\xB8\x8F",
                   "Hey! Chillin' as always \xF0\x9F\x98\x8C"});
    if (isFromProfessional)
      return pick({"Hello! Great to hear from you.", "Hi there! How can I help?",
                   "Good to connect!"});
    if (isFromMysterious)
      return pick({"Ah, you've arrived... \xF0\x9F\x8C\x99",
                   "The stars brought you here \xE2\x9C\xA8",
                   "I sensed your presence..."});
    return pick({"Hey! \xF0\x9F\x91\x8B", "What's up!", "Hey there! How's it going?"});
  }

  // Thanks
  if (std::regex_search(lower, reThanks)) {
    if (isFromFunny)
      return pick({"Don't mention it! Well, you just did \xF0\x9F\x98\x82",
                   "Anytime, legend! \xF0\x9F\x8F\x86"});
    if (isFromChill)
      return pick({"No worries at all \xE2\x9C\x8C\xEF\xB8\x8F",
                   "You're good, fam \xF0\x9F\x92\xAB"});
    return pick({"No problem! \xF0\x9F\x98\x8A", "Anytime!", "You got it!"});
  }

  // Question
  if (lower.find('?') != std::string::npos) {
    if (isFromFunny)
      return pick({"Hmm, that's a galaxy brain question \xF0\x9F\xA7\xA0",
                   "Ooh spicy question! Let me think... \xF0\x9F\x8C\xB6\xEF\xB8\x8F"});
    if (isFromMysterious)
      return pick({"The answer lies within... \xF0\x9F\x94\xAE",
                   "Some questions are better left unanswered... or are they? \xF0\x9F\x8C\x99"});
    return pick({"Good question! Let me think about that \xF0\x9F\xA4\x94",
                 "Hmm, interesting question!", "That's a great one!"});
  }

  // Positive
  if (std::regex_search(lower, rePositive)) {
    if (isFromFunny)
      return pick({"Right?! That slaps! \xF0\x9F\x94\xA5",
                   "Absolutely fire! No cap \xF0\x9F\x92\xAF"});
    if (isFromChill)
      return pick({"Totally agree, positive vibes \xF0\x9F\x8C\x9F",
                   "Love that energy \xE2\x9C\xA8"});
    return pick({"That's awesome! \xF0\x9F\x99\x8C", "I love that!",
                 "So true! \xF0\x9F\x92\xAB"});
  }

  // Negative
  if (std::regex_search(lower, reNegative)) {
    if (isFromFunny)
      return pick({"Hey, even the best vibes have off days! But you're still a legend \xF0\x9F\x92\xAA",
                   "That's rough, but you know what? Tomorrow's gonna slap \xF0\x9F\x94\xA5"});
    if (isFromChill)
      return pick({"Hey, it's okay to feel that way. Take your time \xF0\x9F\x92\x99",
                   "Sending good vibes your way \xF0\x9F\x8C\x8A\xE2\x9C\xA8"});
    return pick({"I hear you. It'll get better! \xF0\x9F\x92\x99",
                 "That's tough, but you got this! \xF0\x9F\x92\xAA"});
  }

  // Default
  if (isFromFunny)
    return pick({"Ha, I vibe with that! \xF0\x9F\x98\x84",
                 "That's giving main character energy \xF0\x9F\x92\x85",
                 "Not me agreeing 100% \xF0\x9F\x98\x82"});
  if (isFromChill)
    return pick({"I feel that \xF0\x9F\xAB\xB6", "Totally \xF0\x9F\x8C\x8A",
                 "Vibes \xE2\x9C\xA8"});
  if (isFromProfessional)
    return pick({"I appreciate your perspective.", "That's a solid point.",
                 "Interesting take!"});
  if (isFromMysterious)
    return pick({"Interesting... very interesting \xF0\x9F\x94\xAE",
                 "The universe agrees \xE2\x9C\xA8", "I see... \xF0\x9F\x8C\x99"});

  return pick({"That's cool! \xE2\x9C\xA8", "I hear you!", "Totally!",
               "I feel that! \xF0\x9F\x99\x8C"});
}

// ── getActiveProvider ──────────────────────────────────────────────────────

std::string AlterEgoAIService::getActiveProvider() const {
  if (!cfg_.geminiApiKey.empty()) return "gemini";
  if (!cfg_.openaiApiKey.empty()) return "openai";
  return "template";
}

} // namespace pulse
