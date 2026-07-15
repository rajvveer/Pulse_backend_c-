// media_controller.cc — implementation of MediaController.
//
// Ground truth: src/controllers/mediaController.js + src/routes/media.js
// (mounted at /api/v1/media). The JS handlers stream the multer-buffered file
// bytes to Cloudinary and shape the response. The Cloudinary flow itself is
// ported in pulse::MediaService (media_service.hpp); this controller only does
// the request parsing, validation, and response shaping the JS controller did.
//
// IMPORTANT — response shapes: these two endpoints DO NOT use the standard
// { success, error, code } error contract. The JS returns { success, message }
// (and { success, message, error } on 500), with success bodies of the form
// { success: true, data: ... }. We reproduce those exact shapes verbatim with
// pulse::http::json(...) / pulse::http::ok(...), NOT pulse::http::error(...).
#include "pulse/controllers/media_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/services/media_service.hpp"

#include <drogon/MultiPart.h>

#include <exception>
#include <algorithm>
#include <string>
#include <vector>

using namespace pulse::controllers;

namespace {

size_t maxUploadBytes() {
  const int64_t mb = std::max<int64_t>(1, pulse::config().envInt("UPLOAD_MAX_MB", 25));
  return static_cast<size_t>(mb) * 1024U * 1024U;
}

size_t maxUploadFiles() {
  return static_cast<size_t>(std::max<int64_t>(
      1, std::min<int64_t>(20, pulse::config().envInt("UPLOAD_MAX_FILES", 5))));
}

// Shape one UploadResult into the JSON object the JS controller emits for each
// file: { url, publicId, width, height, format, type }. Note `type` maps to the
// Cloudinary result.resource_type (the JS used `type: result.resource_type`).
Json::Value toFileData(const pulse::UploadResult& r) {
  Json::Value d(Json::objectValue);
  d["url"] = r.secureUrl;        // result.secure_url
  d["publicId"] = r.publicId;    // result.public_id
  d["width"] = r.width;          // result.width
  d["height"] = r.height;        // result.height
  d["format"] = r.format;        // result.format
  d["type"] = r.resourceType;    // result.resource_type
  return d;
}

// res.status(code).json({ success:false, message }) — the JS "no file" 400 and
// the generic 500s use this { success, message } shape (no error/code fields).
pulse::http::HttpResponsePtr messageError(drogon::HttpStatusCode code,
                                          const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// res.status(500).json({ success:false, message, error }) — Cloudinary/catch.
pulse::http::HttpResponsePtr messageErrorWithDetail(const std::string& message,
                                                    const std::string& detail) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  body["error"] = detail;
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

} // namespace

// POST /api/v1/media/upload — single image (req.file from upload.single('file')).
void MediaController::uploadMedia(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    // Parse the multipart/form-data body. multer's upload.single('file')
    // populated req.file with the single 'file' field; here we read the first
    // uploaded file out of the parsed multipart payload.
    drogon::MultiPartParser parser;
    if (parser.parse(req) != 0 || parser.getFiles().empty()) {
      // if (!req.file) -> 400 { success:false, message:'No file uploaded' }
      callback(messageError(drogon::k400BadRequest, "No file uploaded"));
      return;
    }

    if (parser.getFiles().size() != 1) {
      callback(messageError(drogon::k400BadRequest,
                            "Exactly one file must be uploaded"));
      return;
    }

    const auto& file = parser.getFiles()[0];
    const auto content = file.fileContent();           // raw bytes (string_view)
    if (content.size() > maxUploadBytes()) {
      callback(messageError(drogon::k413RequestEntityTooLarge,
                            "Uploaded file is too large"));
      return;
    }
    const std::string bytes(content.data(), content.size());
    const std::string filename = file.getFileName();

    // Stream the buffer to Cloudinary (folder 'pulse/posts', resource_type
    // 'auto', transformation [w_1080 c_limit][q_auto:good]). The service throws
    // std::runtime_error on a Cloudinary/transport error, which in the JS is the
    // upload_stream error callback -> 500 { message:'Upload failed', error }.
    pulse::UploadResult result =
        pulse::media().uploadPostMedia(bytes, filename);

    // res.json({ success:true, data: { url, publicId, width, height, format, type } })
    callback(pulse::http::ok(toFileData(result)));
  } catch (const std::exception& e) {
    // Cloudinary upload_stream error callback: 500 { success:false,
    // message:'Upload failed', error: error.message }.
    pulse::log::error("Upload media error: {}", e.what());
    callback(messageErrorWithDetail("Upload failed", e.what()));
  }
}

// POST /api/v1/media/upload-multiple — up to 5 images (req.files from
// upload.array('files', 5)).
void MediaController::uploadMultipleMedia(const HttpRequestPtr& req,
                                          std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    drogon::MultiPartParser parser;
    if (parser.parse(req) != 0 || parser.getFiles().empty()) {
      // if (!req.files || req.files.length === 0) -> 400
      // { success:false, message:'No files uploaded' }
      callback(messageError(drogon::k400BadRequest, "No files uploaded"));
      return;
    }

    if (parser.getFiles().size() > maxUploadFiles()) {
      callback(messageError(drogon::k400BadRequest,
                            "Too many files uploaded"));
      return;
    }

    // req.files.map(file => upload) then Promise.all. Any rejection bubbles to
    // the catch -> 500 { message:'Upload failed', error }. We upload each file
    // sequentially; the first failure throws and is caught below, matching the
    // Promise.all reject-on-first-error behaviour and error shape.
    Json::Value results(Json::arrayValue);
    for (const auto& file : parser.getFiles()) {
      const auto content = file.fileContent();
      if (content.size() > maxUploadBytes()) {
        callback(messageError(drogon::k413RequestEntityTooLarge,
                              "An uploaded file is too large"));
        return;
      }
      const std::string bytes(content.data(), content.size());
      const std::string filename = file.getFileName();
      pulse::UploadResult r = pulse::media().uploadPostMedia(bytes, filename);
      results.append(toFileData(r));
    }

    // res.json({ success:true, data: results })
    callback(pulse::http::ok(results));
  } catch (const std::exception& e) {
    // catch -> 500 { success:false, message:'Upload failed', error: error.message }
    pulse::log::error("Upload multiple media error: {}", e.what());
    callback(messageErrorWithDetail("Upload failed", e.what()));
  }
}
