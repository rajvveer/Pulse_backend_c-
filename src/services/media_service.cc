// media_service.cc — Cloudinary media layer implementation.
//
// Ports the inline Cloudinary usage from the JS controllers (mediaController,
// userController, snapController, reelController) into one service. The original
// src/services/mediaService.js was an empty stub, so the JS controllers ARE the
// ground truth; every folder, resource_type, transformation, eager option, and
// the SHA-1 signature computation here matches what the Cloudinary Node SDK
// produced for those calls.
//
// HTTP is done with Drogon's HttpClient against the Cloudinary REST API. A
// signed upload posts multipart/form-data to
//   https://api.cloudinary.com/v1_1/<cloud_name>/<resource_type>/upload
// with fields: file, api_key, timestamp, signature, folder, transformation,
// eager, eager_async. The signature is sha1(sortedParams + api_secret).
#include "pulse/services/media_service.hpp"
#include "pulse/services/http_client.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <sstream>
#include <future>
#include <stdexcept>
#include <utility>

namespace pulse {

namespace {

// SHA-1 hex digest of a string (Cloudinary signs with SHA-1 by default).
std::string sha1Hex(const std::string& input) {
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(SHA_DIGEST_LENGTH * 2);
  for (unsigned char b : digest) {
    out += hex[b >> 4];
    out += hex[b & 0x0F];
  }
  return out;
}

// A fresh high-entropy boundary prevents an uploaded file from deliberately
// colliding with the multipart delimiter. RNG failure must abort the request.
std::string randomMultipartBoundary() {
  unsigned char random[16];
  if (RAND_bytes(random, static_cast<int>(sizeof(random))) != 1) {
    throw std::runtime_error("Failed to generate multipart boundary");
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string boundary = "----PulseCloudinary";
  boundary.reserve(boundary.size() + sizeof(random) * 2);
  for (const unsigned char byte : random) {
    boundary.push_back(kHex[(byte >> 4) & 0x0f]);
    boundary.push_back(kHex[byte & 0x0f]);
  }
  return boundary;
}

// The original filename is emitted inside a quoted Content-Disposition
// parameter. Keep only its basename and neutralize characters that can escape
// the quoted value or inject another multipart header.
std::string sanitizeMultipartFilename(const std::string& filename) {
  const auto separator = filename.find_last_of("/\\");
  const std::string basename =
      separator == std::string::npos ? filename : filename.substr(separator + 1);

  std::string safe;
  safe.reserve(std::min<std::size_t>(basename.size(), 255));
  for (const unsigned char ch : basename) {
    if (safe.size() == 255) break;
    if (ch < 0x20 || ch == 0x7f || ch == '"' || ch == '\\') {
      safe.push_back('_');
    } else {
      safe.push_back(static_cast<char>(ch));
    }
  }
  return safe.empty() ? std::string("upload.bin") : safe;
}

// Current epoch seconds (Cloudinary `timestamp`).
long long nowEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Resolve the Cloudinary REST resource segment. The SDK posts an `auto` upload
// to the image/upload endpoint, so "auto" maps to "image" here.
std::string resourceSegment(const std::string& resourceType) {
  if (resourceType == "video") return "video";
  if (resourceType == "raw") return "raw";
  return "image"; // "image", "auto", or anything else -> image endpoint
}

// Join transformation segments (each one transformation object) with '/'. This
// matches the Cloudinary SDK chaining of a transformation array.
std::string joinTransformation(const std::vector<std::string>& segs) {
  std::string out;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (i) out += "/";
    out += segs[i];
  }
  return out;
}

} // namespace

MediaService::MediaService() {
  // Mirror config.media.cloudinary.* from src/config/index.js. The C++ Config
  // does not model the dotted media.* paths, so read the same env vars directly.
  auto& cfg = config();
  cloudName_    = cfg.env("CLOUDINARY_CLOUD_NAME");
  apiKey_       = cfg.env("CLOUDINARY_API_KEY");
  apiSecret_    = cfg.env("CLOUDINARY_API_SECRET");
  uploadPreset_ = cfg.env("CLOUDINARY_UPLOAD_PRESET");
  folder_       = cfg.env("CLOUDINARY_FOLDER", "pulse/media");
}

MediaService& MediaService::instance() {
  static MediaService s;
  return s;
}

std::string MediaService::apiUrl(const std::string& resourceType,
                                 const std::string& action) const {
  // Returns the full URL; the HttpClient is created from the host portion and
  // the path is passed to the request separately by the caller.
  return "https://api.cloudinary.com/v1_1/" + cloudName_ + "/" +
         resourceSegment(resourceType) + "/" + action;
}

std::string MediaService::signParams(
    const std::vector<std::pair<std::string, std::string>>& params) const {
  // Cloudinary signature: sort the params by key, join as "k=v&k2=v2", append
  // the api_secret, and take the SHA-1 hex. The `file`, `api_key`, `cloud_name`,
  // `resource_type`, and `signature` fields are excluded by the caller.
  std::map<std::string, std::string> sorted(params.begin(), params.end());
  std::string toSign;
  bool first = true;
  for (const auto& [k, v] : sorted) {
    if (v.empty()) continue; // empty params are not signed
    if (!first) toSign += "&";
    toSign += k + "=" + v;
    first = false;
  }
  return sha1Hex(toSign + apiSecret_);
}

UploadResult MediaService::upload(const std::string& bytes,
                                  const std::string& filename,
                                  const UploadOptions& opts) {
  UploadResult result;
  if (!isConfigured()) {
    pulse::log::error("Cloudinary not configured (missing cloud_name/api_key/api_secret)");
    throw std::runtime_error("Cloudinary not configured");
  }

  const long long ts = nowEpochSeconds();
  const std::string timestamp = std::to_string(ts);
  const std::string transformation = joinTransformation(opts.transformation);
  const std::string eager = joinTransformation(opts.eager);

  // ── Build the set of params to sign (everything except file/api_key/
  //    resource_type/cloud_name/signature). Only non-empty values participate. ──
  std::vector<std::pair<std::string, std::string>> signParamsList;
  if (!eager.empty()) signParamsList.emplace_back("eager", eager);
  if (opts.eagerAsync)  signParamsList.emplace_back("eager_async", "true");
  if (!opts.folder.empty()) signParamsList.emplace_back("folder", opts.folder);
  signParamsList.emplace_back("timestamp", timestamp);
  if (!transformation.empty()) signParamsList.emplace_back("transformation", transformation);

  const std::string signature = signParams(signParamsList);

  // ── Compose the multipart/form-data body explicitly. We hand-build it so the
  //    raw (possibly binary) file bytes survive verbatim, and so the field
  //    ordering / boundary are fully under our control. Cloudinary accepts the
  //    signed params as ordinary multipart text fields. ──
  const std::string fname = sanitizeMultipartFilename(filename);

  std::vector<std::pair<std::string, std::string>> formFields;
  formFields.emplace_back("api_key", apiKey_);
  formFields.emplace_back("timestamp", timestamp);
  formFields.emplace_back("signature", signature);
  if (!opts.folder.empty())      formFields.emplace_back("folder", opts.folder);
  if (!transformation.empty())   formFields.emplace_back("transformation", transformation);
  if (!eager.empty())            formFields.emplace_back("eager", eager);
  if (opts.eagerAsync)           formFields.emplace_back("eager_async", "true");

  const std::string boundary = randomMultipartBoundary();
  std::ostringstream body;
  for (const auto& [k, v] : formFields) {
    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"" << k << "\"\r\n\r\n";
    body << v << "\r\n";
  }
  body << "--" << boundary << "\r\n";
  body << "Content-Disposition: form-data; name=\"file\"; filename=\"" << fname << "\"\r\n";
  body << "Content-Type: application/octet-stream\r\n\r\n";
  body << bytes << "\r\n";
  body << "--" << boundary << "--\r\n";

  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Post);
  req->setPath("/v1_1/" + cloudName_ + "/" + resourceSegment(opts.resourceType) + "/upload");
  req->setBody(body.str());
  req->setContentTypeString(("multipart/form-data; boundary=" + boundary).c_str());

  auto client = drogon::HttpClient::newHttpClient("https://api.cloudinary.com");

  // The JS uses an async callback; for parity in a worker-thread context we
  // block on the result via a promise (Drogon handlers run on a thread pool).
  std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> prom;
  auto fut = prom.get_future();
  client->sendRequest(
      req,
      [&prom](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
        prom.set_value({r, resp});
      }, pulse::services::outboundHttpTimeoutSeconds());
  auto [reqResult, resp] = fut.get();

  if (reqResult != drogon::ReqResult::Ok || !resp) {
    pulse::log::error("Cloudinary upload transport error");
    throw std::runtime_error("Upload failed");
  }

  auto json = resp->getJsonObject();
  if (!json) {
    pulse::log::error("Cloudinary upload: invalid JSON response (status {})",
                      (int)resp->getStatusCode());
    throw std::runtime_error("Upload failed");
  }

  const Json::Value& resBody = *json;
  if (resBody.isMember("error")) {
    std::string msg = resBody["error"].isMember("message")
                          ? resBody["error"]["message"].asString()
                          : "Upload failed";
    pulse::log::error("Cloudinary upload rejected (status {})",
                      static_cast<int>(resp->getStatusCode()));
    throw std::runtime_error(msg);
  }

  result.raw          = resBody;
  result.secureUrl    = resBody.get("secure_url", "").asString();
  result.publicId     = resBody.get("public_id", "").asString();
  result.width        = resBody.get("width", 0).asInt();
  result.height       = resBody.get("height", 0).asInt();
  result.format       = resBody.get("format", "").asString();
  result.resourceType = resBody.get("resource_type", "").asString();
  return result;
}

// ── High-level helpers (one per JS controller call site) ───────────────────

UploadResult MediaService::uploadPostMedia(const std::string& bytes,
                                           const std::string& filename) {
  // mediaController.uploadMedia:
  //   folder: 'pulse/posts', resource_type: 'auto',
  //   transformation: [{ width:1080, crop:'limit' }, { quality:'auto:good' }]
  UploadOptions opts;
  opts.folder = "pulse/posts";
  opts.resourceType = "auto";
  opts.transformation = {"c_limit,w_1080", "q_auto:good"};
  return upload(bytes, filename, opts);
}

UploadResult MediaService::uploadAvatar(const std::string& bytes,
                                        const std::string& filename) {
  // userController.uploadAvatar:
  //   folder: 'pulse/avatars', resource_type: 'auto',
  //   transformation: [{ width:400, height:400, crop:'fill', gravity:'face' }]
  UploadOptions opts;
  opts.folder = "pulse/avatars";
  opts.resourceType = "auto";
  opts.transformation = {"c_fill,g_face,h_400,w_400"};
  return upload(bytes, filename, opts);
}

UploadResult MediaService::uploadSnap(const std::string& bytes,
                                      const std::string& filename, bool isVideo) {
  // snapController create:
  //   folder: '<config.folder>/snaps', resource_type: image|video,
  //   transformation: video -> [{ width:720, crop:'limit', quality:'auto:good' }]
  //                    image -> [{ width:1080, crop:'limit', quality:'auto:good' }]
  UploadOptions opts;
  opts.folder = folder_ + "/snaps";
  opts.resourceType = isVideo ? "video" : "image";
  opts.transformation = isVideo
                            ? std::vector<std::string>{"c_limit,q_auto:good,w_720"}
                            : std::vector<std::string>{"c_limit,q_auto:good,w_1080"};
  return upload(bytes, filename, opts);
}

UploadResult MediaService::uploadReel(const std::string& bytes,
                                      const std::string& filename) {
  // reelController upload:
  //   folder: '<config.folder>/reels', resource_type: 'video',
  //   eager: [{ width:720, crop:'limit', quality:'auto:good' }], eager_async: true
  UploadOptions opts;
  opts.folder = folder_ + "/reels";
  opts.resourceType = "video";
  opts.eager = {"c_limit,q_auto:good,w_720"};
  opts.eagerAsync = true;
  return upload(bytes, filename, opts);
}

// ── Delete (signed) ────────────────────────────────────────────────────────

Json::Value MediaService::destroy(const std::string& publicId,
                                  const std::string& resourceType) {
  // snapController: cloudinary.uploader.destroy(publicId, { resource_type }).
  // POST /v1_1/<cloud>/<resourceType>/destroy with signed { public_id,
  // timestamp } + api_key + signature.
  Json::Value out(Json::objectValue);
  if (!isConfigured()) {
    pulse::log::error("Cloudinary not configured for destroy");
    out["result"] = "error";
    return out;
  }

  const std::string timestamp = std::to_string(nowEpochSeconds());
  std::vector<std::pair<std::string, std::string>> signList;
  signList.emplace_back("public_id", publicId);
  signList.emplace_back("timestamp", timestamp);
  const std::string signature = signParams(signList);

  const std::string boundary = randomMultipartBoundary();
  std::vector<std::pair<std::string, std::string>> formFields = {
      {"public_id", publicId},
      {"timestamp", timestamp},
      {"api_key", apiKey_},
      {"signature", signature},
  };
  std::ostringstream body;
  for (const auto& [k, v] : formFields) {
    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"" << k << "\"\r\n\r\n";
    body << v << "\r\n";
  }
  body << "--" << boundary << "--\r\n";

  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Post);
  req->setPath("/v1_1/" + cloudName_ + "/" + resourceSegment(resourceType) + "/destroy");
  req->setBody(body.str());
  req->setContentTypeString(("multipart/form-data; boundary=" + boundary).c_str());

  auto client = drogon::HttpClient::newHttpClient("https://api.cloudinary.com");
  std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> prom;
  auto fut = prom.get_future();
  client->sendRequest(req, [&prom](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
    prom.set_value({r, resp});
  }, pulse::services::outboundHttpTimeoutSeconds());
  auto [reqResult, resp] = fut.get();

  if (reqResult != drogon::ReqResult::Ok || !resp) {
    pulse::log::error("Cloudinary destroy transport error");
    out["result"] = "error";
    return out;
  }
  auto json = resp->getJsonObject();
  if (json) return *json;
  out["result"] = "error";
  return out;
}

// ── URL transforms (no network) ────────────────────────────────────────────

std::string MediaService::optimizeUrl(const std::string& url) {
  // reelController optimizeUrl: only rewrite Cloudinary URLs; split on
  // '/upload/' and inject 'f_auto,q_auto,w_720/'.
  if (url.empty() || url.find("cloudinary") == std::string::npos) return url;
  const std::string marker = "/upload/";
  auto pos = url.find(marker);
  if (pos == std::string::npos) return url; // JS would produce 'undefined'; guard instead
  std::string head = url.substr(0, pos);
  std::string tail = url.substr(pos + marker.size());
  return head + "/upload/f_auto,q_auto,w_720/" + tail;
}

std::string MediaService::videoThumbnailUrl(const std::string& secureUrl, bool isVideo) {
  // snapController thumbnailUrl: for video, replace a trailing .mp4/.mov/.webm
  // (case-insensitive) with .jpg; otherwise return the url unchanged.
  if (!isVideo) return secureUrl;
  static const char* exts[] = {".mp4", ".mov", ".webm"};
  for (const char* ext : exts) {
    const size_t el = std::string(ext).size();
    if (secureUrl.size() >= el) {
      std::string suffix = secureUrl.substr(secureUrl.size() - el);
      std::string lower = suffix;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return (char)std::tolower(c); });
      if (lower == ext) {
        return secureUrl.substr(0, secureUrl.size() - el) + ".jpg";
      }
    }
  }
  return secureUrl;
}

} // namespace pulse
