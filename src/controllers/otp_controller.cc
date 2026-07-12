// otp_controller.cc — port of routes/otp.js (controllers/otpController.js).
//
// 1:1 with the Express source:
//   router.get('/test', (req, res) => {
//     res.json({
//       success: true,
//       message: 'OTP API working!',
//       timestamp: new Date().toISOString()
//     });
//   });
// otpController.js is an empty file, so no controller handlers exist to port.
#include "pulse/controllers/otp_controller.hpp"

#include "pulse/bson_json.hpp"    // pulse::bsonjson::nowIso8601()
#include "pulse/http_response.hpp"

namespace pulse::controllers {

// GET /api/v1/auth/test
void OtpController::test(const drogon::HttpRequestPtr& /*req*/,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  // res.json({ success: true, message: 'OTP API working!', timestamp: <ISO> })
  // pulse::http::success already sets success:true; merge the remaining fields.
  Json::Value extra(Json::objectValue);
  extra["message"] = "OTP API working!";
  extra["timestamp"] = pulse::bsonjson::nowIso8601();  // new Date().toISOString()
  callback(pulse::http::success(std::move(extra)));
}

}  // namespace pulse::controllers
