// firebase_service.hpp — Firebase Admin config/init, ports src/config/firebase.js.
//
// The Node code used the firebase-admin SDK; there is no equivalent C++ SDK, so
// this unit reimplements the small surface the backend actually relied on on top
// of OpenSSL/jwt-cpp (RS256) and Drogon's HttpClient (Google REST endpoints):
//
//   initialize()        — read the service-account credentials from env (the same
//                         FIREBASE_* keys config/index.js mapped), validate they
//                         are complete, and flip isInitialized. Never throws /
//                         never crashes the process (returns "available?" bool).
//   verifyIdToken(t)    — verify a Google-signed Firebase ID token (RS256 against
//                         Google's public x509 certs, issuer/audience pinned to
//                         the project), returning the decoded identity fields.
//   createCustomToken() — mint a Firebase custom token (RS256, signed by the
//                         service-account private key).
//   getUser(uid)        — look a user up via the Identity Toolkit REST API using
//                         a service-account OAuth2 bearer token.
//   isAvailable()       — isInitialized && auth configured.
//
// Exposed as a class with a process-wide singleton accessor (`pulse::firebase()`)
// mirroring the JS `module.exports = new FirebaseConfig()` singleton.
#pragma once
#include <string>
#include <map>
#include <stdexcept>
#include <json/json.h>

namespace pulse {

// Thrown on verification / lookup failure. Messages mirror the JS source
// ("Firebase not initialized", "Invalid Firebase token",
// "Failed to create custom token", "User not found in Firebase").
class FirebaseError : public std::runtime_error {
public:
  explicit FirebaseError(const std::string& m) : std::runtime_error(m) {}
};

// Result of verifyIdToken — same field names the JS method returned.
struct FirebaseDecodedToken {
  std::string uid;
  std::string email;
  bool        emailVerified = false;
  std::string name;
  std::string picture;
  std::string provider;     // firebase.sign_in_provider
};

class FirebaseService {
public:
  // Singleton accessor (mirrors `new FirebaseConfig()` module singleton).
  static FirebaseService& instance();

  // Initialize from env. Returns true if Firebase is available afterwards,
  // false if the config is incomplete or initialization failed. Never throws —
  // matching the JS `initialize()` which logs and returns null instead of
  // crashing the app (even in production).
  bool initialize();

  // Verify a Firebase ID token. Throws FirebaseError on any failure.
  FirebaseDecodedToken verifyIdToken(const std::string& idToken);

  // Create a Firebase custom token for `userId`. additionalClaims is merged into
  // the "claims" field. Throws FirebaseError on failure.
  std::string createCustomToken(const std::string& userId,
                                const Json::Value& additionalClaims = Json::Value(Json::objectValue));

  // Look up a Firebase user by uid. Throws FirebaseError if not initialized or
  // not found. Returns the same field set as the JS getUser().
  Json::Value getUser(const std::string& uid);

  // isInitialized && auth configured.
  bool isAvailable() const { return isInitialized_ && authConfigured_; }

  // Accessors mirroring getApp()/getAuth(); here they surface the project id /
  // initialization state rather than SDK handles (no C++ SDK objects exist).
  const std::string& getApp() const { return projectId_; }   // "" when uninitialized
  bool getAuth() const { return authConfigured_; }

private:
  FirebaseService() = default;

  // Acquire (and cache) a Google OAuth2 access token for the service account via
  // the JWT-bearer grant, scoped for the Identity Toolkit. Throws on failure.
  std::string getAccessToken(const std::string& scope);

  bool isInitialized_  = false;
  bool authConfigured_ = false;

  // Service-account fields (from FIREBASE_* env, mapped exactly like config.js).
  std::string projectId_;
  std::string privateKeyId_;
  std::string privateKey_;     // PEM, with literal \n already unescaped
  std::string clientEmail_;
  std::string clientId_;
  std::string authUri_;
  std::string tokenUri_;

  // Cached service-account access token (value + unix-epoch expiry seconds).
  std::string cachedToken_;
  long long   cachedTokenExpiry_ = 0;
};

// Convenience free function matching the JS `firebaseConfig` singleton usage.
inline FirebaseService& firebase() { return FirebaseService::instance(); }

} // namespace pulse

// ── Adapter namespace consumed by authService ────────────────────────────────
// auth_service.cc calls `firebase::isAvailable()` / `firebase::verifyIdToken()`
// (resolved as pulse::services::firebase::* from inside pulse::services) and
// reads the decoded token back as a Json::Value with keys uid/email/name/
// picture/emailVerified/provider. These thin free functions delegate to the
// FirebaseService singleton and shape the result the way the caller expects,
// matching `firebaseConfig.verifyIdToken(idToken)` in the JS authService.
namespace pulse::services::firebase {

// firebaseConfig.isAvailable()
bool isAvailable();

// firebaseConfig.verifyIdToken(idToken) -> { uid, email, emailVerified, name,
// picture, provider }. Throws (FirebaseError) on an invalid/uninitialized token,
// exactly as the Admin SDK call the JS authService wrapped in try/catch did.
Json::Value verifyIdToken(const std::string& idToken);

}  // namespace pulse::services::firebase
