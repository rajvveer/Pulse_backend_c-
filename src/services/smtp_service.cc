// smtp_service.cc — implementation. Ports src/config/smtp.js.
//
// The Node code wrapped nodemailer-brevo-transport (Brevo's HTTP transport
// behind nodemailer's SMTP-style API). There is no nodemailer in C++, so the
// small surface the backend used is reimplemented directly on the Brevo
// transactional-email HTTP API (POST https://api.brevo.com/v3/smtp/email) via
// Drogon's HttpClient. Behavior, env keys, and log messages mirror the JS source.
#include "pulse/services/smtp_service.hpp"

#include "pulse/config.hpp"
#include "pulse/logger.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoopThread.h>

#include <future>
#include <regex>
#include <string>
#include <utility>

namespace pulse {

namespace {

// One-shot synchronous HTTP call on a dedicated trantor loop, so we don't depend
// on Drogon's app loop running (SMTP may be used outside a request — e.g. boot).
// Returns the response (may be null on transport failure). Mirrors the helper in
// custom_otp_service.cc so the two Brevo call sites behave identically.
drogon::HttpResponsePtr sendHttpSync(const std::string& baseUrl,
                                     const drogon::HttpRequestPtr& req) {
  trantor::EventLoopThread loopThread;
  loopThread.run();
  auto loop = loopThread.getLoop();

  auto client = drogon::HttpClient::newHttpClient(baseUrl, loop);

  std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> prom;
  auto fut = prom.get_future();
  client->sendRequest(
      req, [&prom](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
        prom.set_value({r, resp});
      });
  auto [result, resp] = fut.get();
  loop->quit();
  if (result != drogon::ReqResult::Ok) {
    const char* reason = "Unknown";
    switch (result) {
      case drogon::ReqResult::BadResponse:       reason = "BadResponse"; break;
      case drogon::ReqResult::NetworkFailure:    reason = "NetworkFailure"; break;
      case drogon::ReqResult::BadServerAddress:  reason = "BadServerAddress"; break;
      case drogon::ReqResult::Timeout:           reason = "Timeout"; break;
      case drogon::ReqResult::HandshakeError:    reason = "HandshakeError"; break;
      case drogon::ReqResult::InvalidCertificate:reason = "InvalidCertificate"; break;
      case drogon::ReqResult::EncryptionFailure: reason = "EncryptionFailure"; break;
      default: break;
    }
    pulse::log::error("sendHttpSync transport failure to {}: ReqResult={}", baseUrl, reason);
    return nullptr;
  }
  return resp;
}

// Split a "Display Name <email@host>" string into its parts. nodemailer accepted
// either a bare address or a formatted "Name <addr>"; Brevo's API wants them
// separate (sender: { name, email }). Returns {name, email}; name may be empty.
std::pair<std::string, std::string> parseAddress(const std::string& from) {
  std::smatch m;
  // Match "Name <addr>" (name optional/trimmed), else treat the whole as email.
  static const std::regex re(R"(^\s*(.*?)\s*<\s*([^<>]+?)\s*>\s*$)");
  if (std::regex_match(from, m, re)) {
    return {m[1].str(), m[2].str()};
  }
  return {std::string(), from};
}

} // namespace

// -----------------------------------------------------------------------------
// Singleton
// -----------------------------------------------------------------------------
SMTPConfig& SMTPConfig::instance() {
  static SMTPConfig s;
  return s;
}

// -----------------------------------------------------------------------------
// initialize() — ports SMTPConfig.initialize()
// -----------------------------------------------------------------------------
bool SMTPConfig::initialize() {
  try {
    // 1. Get the API Key from environment variables (process.env.EMAIL_API_KEY).
    const std::string apiKey = config().env("EMAIL_API_KEY");

    if (apiKey.empty()) {
      // JS: console.warn + return null.
      pulse::log::warn("\xE2\x9A\xA0\xEF\xB8\x8F Email API Key (EMAIL_API_KEY) is missing in .env");
      return false;
    }

    // 2. Initialize the Brevo HTTP transport (here: just retain the api key — the
    //    transport itself is the per-call Brevo HTTP request).
    apiKey_ = apiKey;

    // 3. Mark configured (Brevo validates the key on the first send). JS logged
    //    "Email Service Configured (via Brevo HTTP)" and returned the transport.
    isConfigured_ = true;
    pulse::log::info("\xE2\x9C\x85 Email Service Configured (via Brevo HTTP)");

    return true;
  } catch (const std::exception& error) {
    // JS: console.error + return null. Never crashes the caller.
    pulse::log::error("\xE2\x9D\x8C Email initialization failed: {}", error.what());
    return false;
  }
}

// -----------------------------------------------------------------------------
// sendMail() — ports SMTPConfig.sendMail()
// -----------------------------------------------------------------------------
Json::Value SMTPConfig::sendMail(const MailOptions& mailOptions) {
  // Auto-initialize if not ready (JS: re-run initialize(); throw if still not).
  if (!isConfigured_ || apiKey_.empty()) {
    pulse::log::info("\xE2\x9A\xA0\xEF\xB8\x8F SMTP not ready, attempting to initialize...");
    initialize();
    if (apiKey_.empty()) {
      throw SmtpError("Email service failed to initialize");
    }
  }

  try {
    Json::Value info = sendBrevoEmail(mailOptions);
    // JS: console.log(`Email sent successfully to ${mailOptions.to}`).
    pulse::log::info("\xE2\x9C\x85 Email sent successfully to {}", mailOptions.to);
    return info;
  } catch (const SmtpError& err) {
    // JS: console.error + reject(err).
    pulse::log::error("\xE2\x9D\x8C Email send error: {}", err.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// close() — tear down the transport (nodemailer transporter.close())
// -----------------------------------------------------------------------------
void SMTPConfig::close() {
  // The JS transport had no explicit close in smtp.js; nodemailer transporters
  // expose .close(). Here closing simply drops the retained transport so the
  // next sendMail() re-initializes. Best-effort, never throws.
  apiKey_.clear();
  isConfigured_ = false;
}

// -----------------------------------------------------------------------------
// Brevo HTTP send (Drogon HttpClient)
// -----------------------------------------------------------------------------
Json::Value SMTPConfig::sendBrevoEmail(const MailOptions& mailOptions) {
  // Brevo (Sendinblue) transactional email API — the HTTP equivalent of the
  // nodemailer-brevo-transport transport. POST /v3/smtp/email.
  Json::Value body(Json::objectValue);

  // sender: derived from mailOptions.from ("Name <addr>"), falling back to the
  // verified Pulsee sender the JS callers used.
  Json::Value sender(Json::objectValue);
  if (!mailOptions.from.empty()) {
    auto [name, email] = parseAddress(mailOptions.from);
    if (!name.empty()) sender["name"] = name;
    sender["email"] = email;
  } else {
    sender["name"]  = "Pulsee";
    sender["email"] = "rajveershekhawat626@gmail.com";  // MUST match verified sender
  }
  body["sender"] = sender;

  Json::Value toArr(Json::arrayValue);
  Json::Value toObj(Json::objectValue);
  toObj["email"] = mailOptions.to;
  toArr.append(toObj);
  body["to"]      = toArr;
  body["subject"] = mailOptions.subject;
  if (!mailOptions.html.empty()) body["htmlContent"] = mailOptions.html;
  if (!mailOptions.text.empty()) body["textContent"] = mailOptions.text;

  auto req = drogon::HttpRequest::newHttpJsonRequest(body);
  req->setMethod(drogon::Post);
  req->setPath("/v3/smtp/email");
  req->addHeader("api-key", apiKey_);
  req->addHeader("accept", "application/json");

  auto resp = sendHttpSync("https://api.brevo.com", req);
  if (!resp) {
    throw SmtpError("Email transport failed (no response from Brevo)");
  }
  int status = static_cast<int>(resp->getStatusCode());
  if (status < 200 || status >= 300) {
    throw SmtpError("Brevo email send failed with status " + std::to_string(status) +
                    ": " + std::string(resp->getBody()));
  }

  // Resolve with the API response info (nodemailer resolved with `info`). Brevo
  // returns { "messageId": "<...>" }; surface the parsed JSON (or the raw body
  // wrapped) so callers get the transport result like the JS `info`.
  auto jsonPtr = resp->getJsonObject();
  if (jsonPtr) return *jsonPtr;
  Json::Value info(Json::objectValue);
  info["response"] = std::string(resp->getBody());
  return info;
}

} // namespace pulse
