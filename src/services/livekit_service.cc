// livekit_service.cc — implementation of LiveKitService (see the header).
#include "pulse/services/livekit_service.hpp"

#include "pulse/config.hpp"
#include "pulse/logger.hpp"

// jwt-cpp is built with JWT_DISABLE_PICOJSON in vcpkg; use the JsonCpp traits,
// exactly like jwt_service.cc and push_service.cc.
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/defaults.h>
#include <json/json.h>

#include <chrono>

namespace pulse {

LiveKitService& LiveKitService::instance() {
  static LiveKitService s;
  return s;
}

LiveKitService::LiveKitService() {
  auto& cfg = config();
  apiKey_    = cfg.env("LIVEKIT_API_KEY");
  apiSecret_ = cfg.env("LIVEKIT_API_SECRET");
  wsUrl_     = cfg.env("LIVEKIT_WS_URL");
  if (isConfigured()) {
    log::info("LiveKit configured (ws={})", wsUrl_.empty() ? "<unset>" : wsUrl_);
  } else {
    log::warn("LiveKit not configured — set LIVEKIT_API_KEY / LIVEKIT_API_SECRET "
              "/ LIVEKIT_WS_URL to enable calling");
  }
}

bool LiveKitService::isConfigured() const {
  return !apiKey_.empty() && !apiSecret_.empty();
}

std::string LiveKitService::mintToken(const std::string& room,
                                      const std::string& identity,
                                      const std::string& name,
                                      long long ttlSeconds,
                                      bool canPublish,
                                      bool canSubscribe) const {
  if (!isConfigured()) {
    log::error("LiveKit mintToken called but service is not configured");
    return "";
  }
  if (room.empty() || identity.empty()) {
    log::error("LiveKit mintToken requires a room and identity");
    return "";
  }

  try {
    // The nested VideoGrant. LiveKit names the claim "video"; the inner keys are
    // exactly the server SDK's VideoGrant field names.
    Json::Value video(Json::objectValue);
    video["room"]          = room;
    video["roomJoin"]      = true;
    video["canPublish"]    = canPublish;
    video["canSubscribe"]  = canSubscribe;
    video["canPublishData"] = true;  // allow data messages (we use them for in-call signals)

    auto now = std::chrono::system_clock::now();

    auto builder =
        jwt::create<jwt::traits::open_source_parsers_jsoncpp>()
            .set_issuer(apiKey_)                 // iss = API key
            .set_subject(identity)               // sub = participant identity
            .set_id(identity)                    // jti (LiveKit tolerates/ignores)
            .set_issued_at(now)
            .set_not_before(now)
            .set_expires_at(now + std::chrono::seconds(ttlSeconds))
            .set_payload_claim("video", jwt::claim(video));

    if (!name.empty()) {
      builder.set_payload_claim("name", jwt::claim(name));
    }

    return builder.sign(jwt::algorithm::hs256{apiSecret_});
  } catch (const std::exception& e) {
    log::error("LiveKit mintToken error: {}", e.what());
    return "";
  }
}

} // namespace pulse
