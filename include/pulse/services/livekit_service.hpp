// livekit_service.hpp — mints LiveKit access tokens for the calling feature.
//
// LiveKit Cloud (and any LiveKit SFU) authenticates a participant with a signed
// JWT it calls an "access token". The token is a plain HS256 JWT whose payload
// carries the API key as the issuer plus a nested `video` grant object:
//
//   header  : { alg: "HS256", typ: "JWT" }
//   payload : { iss: <LIVEKIT_API_KEY>,
//               sub: <participant identity>,      // e.g. "userId#deviceId"
//               nbf: <now>, iat: <now>, exp: <now + ttl>,
//               name: <display name>,             // optional
//               video: { room, roomJoin: true, canPublish, canSubscribe,
//                        canPublishData } }
//
// signed with HS256 using LIVEKIT_API_SECRET. We build it with jwt-cpp (JsonCpp
// traits, the same path jwt_service.cc / push_service.cc already use). The nested
// `video` object is supplied as a jwt::claim wrapping a Json::Value.
//
// Configuration is read from env (no config.hpp changes needed — Config::env()
// exposes raw keys):
//   LIVEKIT_API_KEY     — the project API key   (the JWT `iss`)
//   LIVEKIT_API_SECRET  — the project API secret (the HS256 signing key)
//   LIVEKIT_WS_URL      — wss://<proj>.livekit.cloud (handed to the client so it
//                         knows which SFU to dial)
//
// All three come from the LiveKit Cloud dashboard (cloud.livekit.io). When they
// are unset the service reports isConfigured()==false and the call endpoints
// return a clear "calling not configured" error instead of minting a junk token.
#pragma once
#include <string>

namespace pulse {

class LiveKitService {
public:
  static LiveKitService& instance();

  // True only when API key + secret are present. WS URL is required for the
  // client to connect but a token can still be minted without it.
  bool isConfigured() const;

  // The wss:// URL the mobile client should connect to (LIVEKIT_WS_URL).
  const std::string& wsUrl() const { return wsUrl_; }

  // Mint a join token for `identity` (e.g. "userId#deviceId") to join `room`.
  //   name        — display name shown to other participants (optional).
  //   ttlSeconds  — token lifetime; default 6h (a call won't outlive that, and a
  //                 client reconnect re-uses the same token).
  //   canPublish/canSubscribe — media grants (both true for a normal call).
  // Returns "" on failure (not configured / signing error).
  std::string mintToken(const std::string& room,
                        const std::string& identity,
                        const std::string& name = "",
                        long long ttlSeconds = 6 * 3600,
                        bool canPublish = true,
                        bool canSubscribe = true) const;

private:
  LiveKitService();

  std::string apiKey_;
  std::string apiSecret_;
  std::string wsUrl_;
};

inline LiveKitService& livekit() { return LiveKitService::instance(); }

} // namespace pulse
