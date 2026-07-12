// firebase_service.cc — implementation of the Firebase Admin surface used by the
// backend. Ports src/config/firebase.js. There is no firebase-admin C++ SDK, so
// the small set of methods the app relied on are reimplemented on OpenSSL/jwt-cpp
// (RS256) and Drogon's HttpClient (Google REST endpoints). Behavior, field names,
// and error messages mirror the JS source exactly.
#include "pulse/services/firebase_service.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
// jwt-cpp built with JWT_DISABLE_PICOJSON in vcpkg; use the JsonCpp traits.
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/defaults.h>

#include <chrono>
#include <mutex>

namespace pulse {

namespace {

// Replace the literal two-character sequence "\n" with a real newline, matching
// config/index.js: FIREBASE_PRIVATE_KEY?.replace(/\\n/g, '\n').
std::string unescapeNewlines(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
      out += '\n';
      ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

long long nowEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Synchronous HTTP GET/POST against an absolute https URL via Drogon's
// HttpClient. Returns the response (may be null on transport failure).
drogon::HttpResponsePtr httpSend(const std::string& url,
                                 drogon::HttpMethod method,
                                 const std::string& body = "",
                                 const std::string& contentType = "",
                                 const std::map<std::string, std::string>& headers = {}) {
  // Split scheme+host from path. HttpClient wants the origin; the request keeps
  // the path/query.
  std::string origin = url;
  std::string path = "/";
  // find "://"
  auto schemeEnd = url.find("://");
  if (schemeEnd != std::string::npos) {
    auto pathStart = url.find('/', schemeEnd + 3);
    if (pathStart != std::string::npos) {
      origin = url.substr(0, pathStart);
      path = url.substr(pathStart);
    }
  }
  auto client = drogon::HttpClient::newHttpClient(origin);
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method);
  req->setPath(path);
  for (const auto& [k, v] : headers) req->addHeader(k, v);
  if (!body.empty()) {
    req->setBody(body);
    if (!contentType.empty()) req->setContentTypeString(contentType);
  }
  auto [result, resp] = client->sendRequest(req);
  if (result != drogon::ReqResult::Ok) return nullptr;
  return resp;
}

} // namespace

FirebaseService& FirebaseService::instance() {
  static FirebaseService s;
  return s;
}

bool FirebaseService::initialize() {
  // 1. Already initialized: return current availability (JS returned the app).
  if (isInitialized_) {
    pulse::log::info("🔥 Firebase already initialized");
    return isAvailable();
  }

  auto& cfg = config();

  // 2. Read Firebase config from env — the SAME keys config/index.js mapped
  //    under config.get('firebase').
  projectId_    = cfg.env("FIREBASE_PROJECT_ID");
  privateKeyId_ = cfg.env("FIREBASE_PRIVATE_KEY_ID");
  privateKey_   = unescapeNewlines(cfg.env("FIREBASE_PRIVATE_KEY"));
  clientEmail_  = cfg.env("FIREBASE_CLIENT_EMAIL");
  clientId_     = cfg.env("FIREBASE_CLIENT_ID");
  authUri_      = cfg.env("FIREBASE_AUTH_URI");
  tokenUri_     = cfg.env("FIREBASE_TOKEN_URI");

  // 3. Incomplete config: warn, disable Firebase features, return null/false.
  if (projectId_.empty() || privateKey_.empty() || clientEmail_.empty()) {
    pulse::log::warn("⚠️  Firebase configuration incomplete - Firebase features will be disabled");
    return false;
  }

  // Apply the same auth_uri/token_uri defaults the JS service account used.
  if (authUri_.empty())  authUri_  = "https://accounts.google.com/o/oauth2/auth";
  if (tokenUri_.empty()) tokenUri_ = "https://oauth2.googleapis.com/token";

  // 4/5/6/7. (The JS step built a serviceAccount object and called
  //          admin.initializeApp(); here the credentials above ARE the
  //          service account.) Flip the flags.
  isInitialized_  = true;
  authConfigured_ = true;

  pulse::log::info("✅ Firebase Admin SDK initialized successfully");
  pulse::log::info("📍 Project ID: {}", projectId_);

  // 8. Return availability (the JS returned the app or null; never crashes).
  return true;
}

FirebaseDecodedToken FirebaseService::verifyIdToken(const std::string& idToken) {
  // 1. Check if initialized, throw if not.
  if (!isInitialized_ || !authConfigured_) {
    throw FirebaseError("Firebase not initialized");
  }

  try {
    // 2. Verify the Google-signed Firebase ID token.
    //    Firebase ID tokens are RS256, signed by Google's "securetoken" service
    //    account. Public x509 certs live at the well-known Google endpoint,
    //    keyed by the token's "kid" header. Issuer/audience are pinned to the
    //    project. (firebase-admin does exactly this under the hood.)
    auto decoded = jwt::decode(idToken);

    const std::string kid =
        decoded.has_header_claim("kid") ? decoded.get_header_claim("kid").as_string() : "";
    if (kid.empty()) throw FirebaseError("Invalid Firebase token");

    static const char* kCertUrl =
        "https://www.googleapis.com/robot/v1/metadata/x509/securetoken@system.gserviceaccount.com";
    auto resp = httpSend(kCertUrl, drogon::Get);
    if (!resp || resp->statusCode() != drogon::k200OK) {
      throw FirebaseError("Invalid Firebase token");
    }

    Json::Value certs;
    {
      Json::CharReaderBuilder b;
      std::string errs;
      const std::string body(resp->getBody());
      std::unique_ptr<Json::CharReader> reader(b.newCharReader());
      if (!reader->parse(body.data(), body.data() + body.size(), &certs, &errs)) {
        throw FirebaseError("Invalid Firebase token");
      }
    }
    if (!certs.isMember(kid)) throw FirebaseError("Invalid Firebase token");
    const std::string cert = certs[kid].asString();

    const std::string expectedIssuer = "https://securetoken.google.com/" + projectId_;
    auto verifier = jwt::verify<jwt::traits::open_source_parsers_jsoncpp>()
                        .allow_algorithm(jwt::algorithm::rs256(cert, "", "", ""))
                        .with_issuer(expectedIssuer)
                        .with_audience(projectId_);
    verifier.verify(decoded);

    // 3. Extract and return uid, email, emailVerified, name, picture, provider.
    auto claim = [&](const char* k) -> std::string {
      return decoded.has_payload_claim(k) ? decoded.get_payload_claim(k).as_string() : "";
    };

    FirebaseDecodedToken out;
    // Firebase ID tokens carry the uid in "sub" (== "user_id").
    out.uid = !claim("sub").empty() ? claim("sub") : claim("user_id");
    out.email = claim("email");
    if (decoded.has_payload_claim("email_verified")) {
      Json::Value v = decoded.get_payload_claim("email_verified").to_json();
      out.emailVerified = v.isBool() ? v.asBool() : false;
    }
    out.name = claim("name");
    out.picture = claim("picture");
    // provider = decodedToken.firebase.sign_in_provider
    if (decoded.has_payload_claim("firebase")) {
      Json::Value fb = decoded.get_payload_claim("firebase").to_json();
      if (fb.isObject() && fb.isMember("sign_in_provider") &&
          fb["sign_in_provider"].isString()) {
        out.provider = fb["sign_in_provider"].asString();
      }
    }
    return out;

  } catch (const FirebaseError&) {
    // 4. Throws if token invalid (matching JS, which logs then rethrows).
    pulse::log::error("Firebase token verification error");
    throw FirebaseError("Invalid Firebase token");
  } catch (const std::exception&) {
    pulse::log::error("Firebase token verification error");
    throw FirebaseError("Invalid Firebase token");
  }
}

std::string FirebaseService::createCustomToken(const std::string& userId,
                                               const Json::Value& additionalClaims) {
  // 1. Check if initialized, throw if not.
  if (!isInitialized_ || !authConfigured_) {
    throw FirebaseError("Firebase not initialized");
  }

  try {
    // 2. Mint a Firebase custom token (RS256, signed by the service account).
    //    This is the exact shape admin.auth().createCustomToken() produces:
    //    iss/sub = clientEmail, aud = the Identity Toolkit custom-token audience,
    //    uid = userId, optional developer "claims".
    static const char* kAud =
        "https://identitytoolkit.googleapis.com/google.identity.identitytoolkit.v1.IdentityToolkit";

    auto now = std::chrono::system_clock::now();
    auto builder = jwt::create<jwt::traits::open_source_parsers_jsoncpp>()
                       .set_issuer(clientEmail_)
                       .set_subject(clientEmail_)
                       .set_audience(kAud)
                       .set_issued_at(now)
                       .set_expires_at(now + std::chrono::hours(1))
                       .set_payload_claim("uid", jwt::claim(userId));

    // Merge additionalClaims into a "claims" object (firebase-admin behavior).
    // With the JsonCpp traits, a jwt::claim wraps a Json::Value directly.
    if (additionalClaims.isObject() && !additionalClaims.empty()) {
      builder.set_payload_claim("claims", jwt::claim(additionalClaims));
    }

    return builder.sign(jwt::algorithm::rs256("", privateKey_, "", ""));

  } catch (const std::exception&) {
    pulse::log::error("Custom token creation error");
    throw FirebaseError("Failed to create custom token");
  }
}

std::string FirebaseService::getAccessToken(const std::string& scope) {
  long long now = nowEpochSeconds();
  // Reuse a cached token while it has > 60s of life left.
  if (!cachedToken_.empty() && cachedTokenExpiry_ - 60 > now) {
    return cachedToken_;
  }

  // Service-account JWT-bearer grant (RFC 7523): sign an assertion with the
  // private key, POST it to the token endpoint, receive an OAuth2 access token.
  auto issued = std::chrono::system_clock::now();
  std::string assertion =
      jwt::create<jwt::traits::open_source_parsers_jsoncpp>()
          .set_issuer(clientEmail_)
          .set_audience(tokenUri_)
          .set_issued_at(issued)
          .set_expires_at(issued + std::chrono::hours(1))
          .set_payload_claim("scope", jwt::claim(scope))
          .sign(jwt::algorithm::rs256("", privateKey_, "", ""));

  std::string body =
      "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=" + assertion;
  auto resp = httpSend(tokenUri_, drogon::Post, body,
                       "application/x-www-form-urlencoded");
  if (!resp || resp->statusCode() != drogon::k200OK) {
    throw FirebaseError("Failed to acquire Firebase access token");
  }

  Json::Value json;
  {
    Json::CharReaderBuilder b;
    std::string errs;
    const std::string respBody(resp->getBody());
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    if (!reader->parse(respBody.data(), respBody.data() + respBody.size(), &json, &errs)) {
      throw FirebaseError("Failed to acquire Firebase access token");
    }
  }
  if (!json.isMember("access_token")) {
    throw FirebaseError("Failed to acquire Firebase access token");
  }
  cachedToken_ = json["access_token"].asString();
  long long expiresIn = json.get("expires_in", 3600).asInt64();
  cachedTokenExpiry_ = now + expiresIn;
  return cachedToken_;
}

Json::Value FirebaseService::getUser(const std::string& uid) {
  // 1. Check if initialized, throw if not.
  if (!isInitialized_ || !authConfigured_) {
    throw FirebaseError("Firebase not initialized");
  }

  try {
    // 2. Look the user up via the Identity Toolkit accounts:lookup REST API
    //    (what admin.auth().getUser() calls), authorized with a service-account
    //    OAuth2 bearer token.
    const std::string token =
        getAccessToken("https://www.googleapis.com/auth/identitytoolkit "
                       "https://www.googleapis.com/auth/firebase");

    const std::string url =
        "https://identitytoolkit.googleapis.com/v1/projects/" + projectId_ +
        "/accounts:lookup";

    Json::Value reqBody(Json::objectValue);
    Json::Value ids(Json::arrayValue);
    ids.append(uid);
    reqBody["localId"] = ids;

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    const std::string bodyStr = Json::writeString(w, reqBody);

    auto resp = httpSend(url, drogon::Post, bodyStr, "application/json",
                         {{"Authorization", "Bearer " + token}});
    if (!resp || resp->statusCode() != drogon::k200OK) {
      throw FirebaseError("User not found in Firebase");
    }

    Json::Value json;
    {
      Json::CharReaderBuilder b;
      std::string errs;
      const std::string respBody(resp->getBody());
      std::unique_ptr<Json::CharReader> reader(b.newCharReader());
      if (!reader->parse(respBody.data(), respBody.data() + respBody.size(), &json, &errs)) {
        throw FirebaseError("User not found in Firebase");
      }
    }

    if (!json.isMember("users") || !json["users"].isArray() || json["users"].empty()) {
      throw FirebaseError("User not found in Firebase");
    }
    const Json::Value& rec = json["users"][0];

    // 3. Extract and return the same field set the JS getUser() returned.
    Json::Value out(Json::objectValue);
    out["uid"]           = rec.get("localId", "").asString();
    out["email"]         = rec.get("email", "").asString();
    out["emailVerified"] = rec.get("emailVerified", false).asBool();
    out["displayName"]   = rec.get("displayName", "").asString();
    out["photoURL"]      = rec.get("photoUrl", "").asString();
    out["disabled"]      = rec.get("disabled", false).asBool();

    // metadata: { creationTime, lastSignInTime } (UserRecord.metadata shape).
    Json::Value metadata(Json::objectValue);
    if (rec.isMember("createdAt"))     metadata["creationTime"]   = rec["createdAt"].asString();
    if (rec.isMember("lastLoginAt"))   metadata["lastSignInTime"] = rec["lastLoginAt"].asString();
    out["metadata"] = metadata;

    // providerData: passthrough of providerUserInfo (UserRecord.providerData).
    out["providerData"] = rec.isMember("providerUserInfo") ? rec["providerUserInfo"]
                                                           : Json::Value(Json::arrayValue);
    return out;

  } catch (const FirebaseError&) {
    pulse::log::error("Get Firebase user error");
    throw FirebaseError("User not found in Firebase");
  } catch (const std::exception&) {
    pulse::log::error("Get Firebase user error");
    throw FirebaseError("User not found in Firebase");
  }
}

} // namespace pulse

// ── Adapter namespace consumed by authService (see firebase_service.hpp) ──────
namespace pulse::services::firebase {

bool isAvailable() {
  return pulse::firebase().isAvailable();
}

Json::Value verifyIdToken(const std::string& idToken) {
  // Delegate to the singleton, then reshape the struct into the Json::Value
  // (uid/email/name/picture/...) that authService reads back. The singleton
  // throws FirebaseError on failure, which authService catches like the JS did.
  pulse::FirebaseDecodedToken d = pulse::firebase().verifyIdToken(idToken);
  Json::Value out(Json::objectValue);
  out["uid"]           = d.uid;
  out["email"]         = d.email;
  out["emailVerified"] = d.emailVerified;
  out["name"]          = d.name;
  out["picture"]       = d.picture;
  out["provider"]      = d.provider;
  return out;
}

}  // namespace pulse::services::firebase
