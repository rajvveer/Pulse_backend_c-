// otp_controller.hpp — port of routes/otp.js (controllers/otpController.js).
//
// Mounted at /api/v1/auth. The Express router exposes a single inline test
// endpoint; the companion otpController.js is empty, so there are no service or
// model calls and no middleware chain to attach.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

class OtpController : public drogon::HttpController<OtpController> {
 public:
  METHOD_LIST_BEGIN
  // router.get('/test', ...) -> /api/v1/auth + /test
  ADD_METHOD_TO(OtpController::test, "/api/v1/auth/test", drogon::Get);
  METHOD_LIST_END

  // GET /api/v1/auth/test
  void test(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

}  // namespace pulse::controllers
