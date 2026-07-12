// smtp_service.hpp — SMTP / email transport, ports src/config/smtp.js.
//
// The Node code used nodemailer + nodemailer-brevo-transport (a Brevo HTTP
// transport dressed up as an SMTP transporter). There is no nodemailer in C++,
// so this unit reimplements the tiny surface the backend relied on directly on
// the Brevo transactional-email HTTP API (POST https://api.brevo.com/v3/smtp/email)
// via Drogon's HttpClient. Behavior, env keys, and log messages mirror the JS
// source exactly.
//
// JS surface being ported (`module.exports = new SMTPConfig()`):
//   initialize()       — read EMAIL_API_KEY; if missing, warn and return null
//                        (no transport). Otherwise mark configured, log success,
//                        return the transport. Never throws (logs + returns null).
//   sendMail(opts)     — auto-initialize if not ready (throws if still not ready),
//                        then POST the email through Brevo. Resolves with the API
//                        response info; throws on transport / non-2xx error.
//   getTransporter()   — accessor (here: the api-key string / availability).
//
// Plus the two helpers this port is asked to expose:
//   isAvailable()      — isConfigured && transport present (mirrors `isConfigured`).
//   close()            — tear the transport down (nodemailer transporter.close());
//                        clears state so the next sendMail re-initializes.
//
// Exposed as a class with a process-wide singleton accessor (`pulse::smtp()`),
// mirroring the JS module singleton.
#pragma once
#include <string>
#include <map>
#include <stdexcept>
#include <json/json.h>

namespace pulse {

// Thrown by sendMail on the failure paths the JS service surfaced via
// `reject(err)` / `throw new Error(...)`. Message strings mirror the JS source
// ("Email service failed to initialize") or describe the Brevo transport error.
class SmtpError : public std::runtime_error {
public:
  explicit SmtpError(const std::string& m) : std::runtime_error(m) {}
};

// Mail options, mirroring the `mailOptions` object passed to
// nodemailer's transporter.sendMail({ from, to, subject, html, text }).
// `from` is optional — when empty the Brevo verified sender default is used
// (matching the JS callers which pass an explicit "Name <email>" from string).
struct MailOptions {
  std::string from;     // e.g. "Pulsee <rajveershekhawat626@gmail.com>" (optional)
  std::string to;       // recipient email address (required)
  std::string subject;  // email subject
  std::string html;     // HTML body (mailOptions.html)
  std::string text;     // plain-text body (mailOptions.text) — optional
};

class SMTPConfig {
public:
  // Singleton accessor (mirrors `new SMTPConfig()` module singleton). The JS
  // constructor only zeroed `transporter`/`isConfigured`; construction here does
  // the same and never reads env (initialize() does, lazily).
  static SMTPConfig& instance();

  // initialize(): read EMAIL_API_KEY from env. If missing, warn and return false
  // (the JS returned null). Otherwise flip isConfigured, log success, return true
  // (the JS returned the transporter). Never throws — matches the JS try/catch
  // that logs and returns null on any failure.
  bool initialize();

  // sendMail(mailOptions): auto-initialize if not ready; if still not ready,
  // throw SmtpError("Email service failed to initialize"). Otherwise POST the
  // message via the Brevo HTTP API and return the parsed JSON response info
  // (the JS resolved with nodemailer's `info`). Throws SmtpError on transport
  // failure or a non-2xx Brevo response.
  Json::Value sendMail(const MailOptions& mailOptions);

  // getTransporter(): the JS returned the nodemailer transporter object (or
  // null). There is no transport object here; expose availability + the api-key
  // surface the transport wrapped. Returns isAvailable().
  bool getTransporter() const { return isAvailable(); }

  // isConfigured && a transport (api key) present. Mirrors the JS `isConfigured`
  // flag, requested explicitly by this port.
  bool isAvailable() const { return isConfigured_ && !apiKey_.empty(); }

  // close(): tear the transport down (nodemailer transporter.close()). Clears
  // state so the next sendMail() re-initializes. Never throws.
  void close();

private:
  SMTPConfig() = default;

  // POST a transactional email through the Brevo HTTP API (Drogon HttpClient).
  // Returns the parsed JSON response. Throws SmtpError on transport / non-2xx.
  Json::Value sendBrevoEmail(const MailOptions& mailOptions);

  bool        isConfigured_ = false;   // JS `this.isConfigured`
  std::string apiKey_;                 // EMAIL_API_KEY — the "transport" (JS `this.transporter`)
};

// Convenience free function matching the JS `smtpConfig` singleton usage.
inline SMTPConfig& smtp() { return SMTPConfig::instance(); }

} // namespace pulse
