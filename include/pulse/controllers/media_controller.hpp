// media_controller.hpp — Drogon HttpController porting src/controllers/mediaController.js
// and the route group src/routes/media.js (mounted at /api/v1/media).
//
// Routes (1:1 with media.js):
//   POST /api/v1/media/upload
//        verifyAccessToken -> uploadLimiter -> upload.uploadGuard ->
//        upload.single('file')  -> uploadMedia
//   POST /api/v1/media/upload-multiple
//        verifyAccessToken -> uploadLimiter -> upload.uploadGuard ->
//        upload.array('files', 5) -> uploadMultipleMedia
//
// Filter mapping (preserving order):
//   verifyAccessToken -> pulse::filters::AuthFilter
//   uploadLimiter     -> pulse::filters::UploadLimiter
//
// upload.uploadGuard / upload.single / upload.array are Express multipart
// (multer) middlewares. Drogon parses multipart/form-data inside the handler
// via drogon::MultiPartParser, so the body parsing lives in the handler rather
// than in a filter; the per-request size guard (Content-Length ceiling) is the
// app-level body-size limit and is not modeled as a separate filter here.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class MediaController : public drogon::HttpController<MediaController> {
public:
  METHOD_LIST_BEGIN
  // POST /api/v1/media/upload
  ADD_METHOD_TO(MediaController::uploadMedia,
                "/api/v1/media/upload", Post,
                "pulse::filters::AuthFilter",
                "pulse::filters::UploadLimiter");
  // POST /api/v1/media/upload-multiple
  ADD_METHOD_TO(MediaController::uploadMultipleMedia,
                "/api/v1/media/upload-multiple", Post,
                "pulse::filters::AuthFilter",
                "pulse::filters::UploadLimiter");
  METHOD_LIST_END

  // POST /upload — single image. mediaController.uploadMedia.
  void uploadMedia(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /upload-multiple — up to 5 images. mediaController.uploadMultipleMedia.
  void uploadMultipleMedia(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
