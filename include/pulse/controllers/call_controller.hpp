// call_controller.hpp — REST endpoints for the voice/video calling feature.
//
// Calls use LiveKit for media (the SFU + TURN) and the existing /ws channel for
// ring/accept/reject signaling. This controller is the HTTP control plane:
//
//   POST /api/v1/calls/initiate
//     Caller starts a call to a peer. The server:
//       1. resolves the conversation + the peer (callee),
//       2. allocates a stable room name (call_<conversationId>_<callId>),
//       3. mints the CALLER a LiveKit join token,
//       4. fires an FCM/Expo "incoming_call" data push to the callee's devices
//          (so a backgrounded peer wakes), and
//       5. returns { callId, room, token, wsUrl, callee, callType }.
//     The actual ring also travels over /ws (call_invite) for foregrounded peers
//     — the push is the backgrounded-peer fallback.
//
//   POST /api/v1/calls/token
//     Mint a LiveKit join token for an already-known room. The CALLEE calls this
//     when it accepts (it received room+callId via the push/ws invite). Also used
//     by the caller to refresh a token. Returns { token, wsUrl, identity, room }.
//
//   POST /api/v1/calls/end   (optional bookkeeping)
//     Best-effort signal that a call ended; currently just clears any Redis call
//     state. Media teardown happens client-side on LiveKit disconnect.
//
// All routes require AuthFilter. Identity in LiveKit is "<userId>#<deviceId>" so
// the same user on two devices gets two distinct participants.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class CallController : public drogon::HttpController<CallController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CallController::initiate, "/api/v1/calls/initiate", Post,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(CallController::token, "/api/v1/calls/token", Post,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(CallController::end, "/api/v1/calls/end", Post,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  void initiate(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);
  void token(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);
  void end(const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
