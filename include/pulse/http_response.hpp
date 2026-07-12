// http_response.hpp — standard JSON response builders matching the Node API.
//
// Every Pulse endpoint returns one of two shapes:
//   success: { "success": true, ...payload }
//   error:   { "success": false, "error": "<message>", "code": "<CODE>" }
// (some success responses also carry "message"/"data"). These helpers centralize
// that contract so controllers read like the JS res.status(x).json({...}).
#pragma once
#include <string>
#include <drogon/HttpResponse.h>
#include <json/json.h>

namespace pulse::http {

using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using drogon::HttpStatusCode;

// Build a JSON response with an explicit status code.
inline HttpResponsePtr json(HttpStatusCode code, Json::Value body) {
  auto resp = HttpResponse::newHttpJsonResponse(std::move(body));
  resp->setStatusCode(code);
  return resp;
}

// Success: { success: true, ...extra }. `extra` is merged at the top level.
inline HttpResponsePtr success(Json::Value extra = Json::Value(Json::objectValue),
                               HttpStatusCode code = drogon::k200OK) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  if (extra.isObject()) {
    for (const auto& key : extra.getMemberNames()) body[key] = extra[key];
  }
  return json(code, std::move(body));
}

// Success with a top-level "data" payload.
inline HttpResponsePtr ok(Json::Value data, HttpStatusCode code = drogon::k200OK) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["data"] = std::move(data);
  return json(code, std::move(body));
}

// Error: { success: false, error, code }.
inline HttpResponsePtr error(HttpStatusCode status, const std::string& message,
                             const std::string& errCode) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["error"] = message;
  body["code"] = errCode;
  return json(status, std::move(body));
}

// Common shorthands matching frequent call sites.
inline HttpResponsePtr badRequest(const std::string& m, const std::string& c = "BAD_REQUEST") {
  return error(drogon::k400BadRequest, m, c);
}
inline HttpResponsePtr unauthorized(const std::string& m, const std::string& c = "UNAUTHORIZED") {
  return error(drogon::k401Unauthorized, m, c);
}
inline HttpResponsePtr forbidden(const std::string& m, const std::string& c = "FORBIDDEN") {
  return error(drogon::k403Forbidden, m, c);
}
inline HttpResponsePtr notFound(const std::string& m = "Not found", const std::string& c = "NOT_FOUND") {
  return error(drogon::k404NotFound, m, c);
}
inline HttpResponsePtr conflict(const std::string& m, const std::string& c = "CONFLICT") {
  return error(drogon::k409Conflict, m, c);
}
inline HttpResponsePtr serverError(const std::string& m = "An unexpected internal server error occurred.",
                                   const std::string& c = "INTERNAL_ERROR") {
  return error(drogon::k500InternalServerError, m, c);
}

} // namespace pulse::http
