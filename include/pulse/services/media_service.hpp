// media_service.hpp — Cloudinary media layer, ports src/services/mediaService.js.
//
// NOTE ON GROUND TRUTH: the original src/services/mediaService.js is an empty
// stub (0 bytes) — all Cloudinary logic lives inline in the controllers
// (mediaController, userController, snapController, reelController). This service
// centralizes that logic 1:1 so the C++ controllers call one place instead of
// re-implementing the signed Cloudinary REST flow. Every folder, public_id,
// resource_type, transformation string, eager option, and the SHA-1 signature
// computation are preserved exactly as the JS Cloudinary SDK produced them.
//
// Cloudinary is configured (cloud_name / api_key / api_secret) from the SAME
// config values the JS used — config.media.cloudinary.* which read the env vars
// CLOUDINARY_CLOUD_NAME / CLOUDINARY_API_KEY / CLOUDINARY_API_SECRET /
// CLOUDINARY_UPLOAD_PRESET / CLOUDINARY_FOLDER (default 'pulse/media').
//
// All HTTP calls to Cloudinary use Drogon's HttpClient (REST upload/destroy API)
// with a signed request: signature = sha1( sortedParams + api_secret ), exactly
// as the Cloudinary Node SDK computes it for an authenticated signed upload.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse {

// Result of a Cloudinary upload, mirroring the fields the JS controllers read
// off the SDK `result` object (result.secure_url, result.public_id, ...).
struct UploadResult {
  std::string secureUrl;     // result.secure_url
  std::string publicId;      // result.public_id
  int         width = 0;     // result.width
  int         height = 0;    // result.height
  std::string format;        // result.format
  std::string resourceType;  // result.resource_type ("image" | "video" | "raw")
  Json::Value raw;           // full Cloudinary JSON response (for any extra field)
};

// Options for an upload, mirroring the option object passed to
// cloudinary.uploader.upload_stream({...}).
struct UploadOptions {
  // folder: the Cloudinary folder (e.g. "pulse/posts", "pulse/avatars",
  // "<config.folder>/snaps", "<config.folder>/reels").
  std::string folder;
  // resource_type: "auto" | "image" | "video" | "raw". Default "auto".
  std::string resourceType = "auto";
  // transformation: an ordered list of "key=value,key2=value2" segments, each
  // entry is one element of the JS `transformation: [{...}, {...}]` array. They
  // are joined with '/' when forming the transformation param, matching the SDK.
  // e.g. {"c_limit,w_1080", "q_auto:good"}.
  std::vector<std::string> transformation;
  // eager: list of eager transformation strings (reels use a single eager).
  std::vector<std::string> eager;
  bool eagerAsync = false;   // eager_async: true
};

// MediaService — singleton, mirrors the JS module-level Cloudinary singleton.
class MediaService {
public:
  static MediaService& instance();

  bool isConfigured() const { return !cloudName_.empty() && !apiKey_.empty() && !apiSecret_.empty(); }

  // ── High-level upload helpers (one per call site in the JS controllers) ──

  // mediaController.uploadMedia: folder 'pulse/posts', resource_type 'auto',
  // transformation [{ width:1080, crop:'limit' }, { quality:'auto:good' }].
  UploadResult uploadPostMedia(const std::string& bytes, const std::string& filename);

  // userController.uploadAvatar: folder 'pulse/avatars', resource_type 'auto',
  // transformation [{ width:400, height:400, crop:'fill', gravity:'face' }].
  UploadResult uploadAvatar(const std::string& bytes, const std::string& filename);

  // snapController create: folder '<config.folder>/snaps',
  // resource_type image|video, transformation
  //   image -> [{ width:1080, crop:'limit', quality:'auto:good' }]
  //   video -> [{ width:720,  crop:'limit', quality:'auto:good' }].
  UploadResult uploadSnap(const std::string& bytes, const std::string& filename, bool isVideo);

  // reelController upload: folder '<config.folder>/reels', resource_type 'video',
  // eager [{ width:720, crop:'limit', quality:'auto:good' }], eager_async true.
  UploadResult uploadReel(const std::string& bytes, const std::string& filename);

  // ── Generic upload (signed) — used by the helpers above ──
  UploadResult upload(const std::string& bytes, const std::string& filename,
                      const UploadOptions& opts);

  // ── Delete (signed) — snapController destroy(publicId, { resource_type }) ──
  // Returns the Cloudinary JSON ({ "result": "ok" | "not found" }).
  Json::Value destroy(const std::string& publicId, const std::string& resourceType = "image");

  // ── URL transforms (no network) ──

  // reelController optimizeUrl: rewrite '/upload/' -> '/upload/f_auto,q_auto,w_720/'
  // for Cloudinary URLs; returns the URL unchanged otherwise.
  std::string optimizeUrl(const std::string& url);

  // snapController thumbnail: for a video secure_url, replace a trailing
  // .mp4/.mov/.webm with .jpg (case-insensitive); otherwise return as-is.
  std::string videoThumbnailUrl(const std::string& secureUrl, bool isVideo);

  // Compute the Cloudinary signature over `params` (the sha1 hex of the sorted
  // "k=v&k2=v2" string concatenated with the api_secret). Exposed for callers
  // (e.g. issuing a signed direct-to-Cloudinary upload to the client).
  std::string signParams(const std::vector<std::pair<std::string, std::string>>& params) const;

  // Accessors mirroring config.media.cloudinary.*.
  const std::string& cloudName() const { return cloudName_; }
  const std::string& apiKey() const { return apiKey_; }
  const std::string& uploadPreset() const { return uploadPreset_; }
  const std::string& folder() const { return folder_; }

private:
  MediaService();

  // Build the Cloudinary REST endpoint for an action ("upload"/"destroy") and a
  // resource type ("image"/"video"/"raw"/"auto" -> auto maps to "image" path,
  // matching the SDK which posts auto uploads to the image/upload endpoint).
  std::string apiUrl(const std::string& resourceType, const std::string& action) const;

  std::string cloudName_;
  std::string apiKey_;
  std::string apiSecret_;
  std::string uploadPreset_;
  std::string folder_;
};

// Convenience free function matching the JS module singleton usage.
inline MediaService& media() { return MediaService::instance(); }

} // namespace pulse
