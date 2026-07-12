// referral_controller.hpp — Drogon HttpController porting
// src/controllers/referralController.js and the route group
// src/routes/referralRoutes.js (mounted at /api/v1/referral).
//
// 1:1 functional parity with the Express router: same sub-paths, HTTP methods,
// ordered middleware (as filters), request/response JSON shapes, and status
// codes. The JS referralController uses the bare shapes:
//   { success:true, data }                    (getMyCode / getStats)
//   { success:true, message, data }           (applyCode)
//   { success:false, error }                  (errors — NO `code` field)
// so those exact shapes are reproduced here rather than the {error,code}
// helper shape.
//
// Middleware -> filter mapping (preserved in order):
//   router.use(verifyAccessToken)  ->  pulse::filters::AuthFilter on EVERY route.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class ReferralController : public drogon::HttpController<ReferralController> {
 public:
  METHOD_LIST_BEGIN
  // router.get('/my-code', verifyAccessToken, referralController.getMyCode)
  ADD_METHOD_TO(ReferralController::getMyCode,
                "/api/v1/referral/my-code", Get, "pulse::filters::AuthFilter");

  // router.post('/apply', verifyAccessToken, referralController.applyCode)
  ADD_METHOD_TO(ReferralController::applyCode,
                "/api/v1/referral/apply", Post, "pulse::filters::AuthFilter");

  // router.get('/stats', verifyAccessToken, referralController.getStats)
  ADD_METHOD_TO(ReferralController::getStats,
                "/api/v1/referral/stats", Get, "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // ── Handlers ──────────────────────────────────────────────────────────────
  void getMyCode(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
  void applyCode(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
  void getStats(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);
};

}  // namespace pulse::controllers
