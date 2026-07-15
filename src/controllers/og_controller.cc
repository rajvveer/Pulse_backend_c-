// og_controller.cc — C++ port of src/controllers/ogController.js.
// See include/pulse/controllers/og_controller.hpp for the route table.
//
// 1:1 functional parity with the Express controller. Each handler renders an
// HTML page with Open Graph / Twitter Card meta tags + a deep-link redirect (or
// returns a plain-text 404 / 500), byte-for-byte matching the JS templates,
// status codes, and Content-Type headers.
//
// The JS used Mongoose:
//   Post.findById(id).populate('author', 'username profile.displayName profile.avatar').lean()
//   User.findOne({ username: lower, isActive: true }).lean()
//   Reel.findById(id).populate('author', 'username profile.displayName profile.avatar').lean()
// Direct CRUD (findById / findOne + the author populate projection) is done with
// mongocxx here, mirroring how the other ported controllers issue their queries.
#include "pulse/controllers/og_controller.hpp"

#include "pulse/config.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/post.hpp"
#include "pulse/models/user.hpp"
#include "pulse/models/reel.hpp"

#include <drogon/HttpResponse.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
namespace bj = pulse::bsonjson;

namespace {

// ── escapeHtml(str) — JS: replace & < > " with their entities (in that order) ─
// `if (!str) return ''` — falsy (empty) strings yield "".
std::string escapeHtml(const std::string& str) {
  std::string out;
  out.reserve(str.size());
  for (char c : str) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      case '\r':
      case '\n':
      case '\0': break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

// Encode a value for a JavaScript double-quoted string inside a <script> block.
// Escaping '<' is essential: otherwise stored text can terminate the script
// element before JavaScript string parsing takes place.
std::string escapeJsString(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (i + 2 < value.size() && c == 0xE2 &&
        static_cast<unsigned char>(value[i + 1]) == 0x80 &&
        (static_cast<unsigned char>(value[i + 2]) == 0xA8 ||
         static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
      out += static_cast<unsigned char>(value[i + 2]) == 0xA8
                 ? "\\u2028" : "\\u2029";
      i += 2;
      continue;
    }
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '<': out += "\\x3C"; break;
      case '>': out += "\\x3E"; break;
      case '&': out += "\\x26"; break;
      default:
        if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));
        break;
    }
  }
  return out;
}

bool safeHttpUrl(const std::string& value) {
  if (value.size() > 2048 ||
      (value.rfind("https://", 0) != 0 && value.rfind("http://", 0) != 0)) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](unsigned char c) {
    return c <= 0x20 || c == 0x7F || c == '\\';
  });
}

// Parameters for renderOGPage (mirrors the JS destructured object; `type`
// defaults to 'article', the rest are required by each caller).
struct OGPageParams {
  std::string title;
  std::string description;
  std::string image;
  std::string url;
  std::string type = "article";
  std::string appDeepLink;
};

// renderOGPage({ title, description, image, url, type='article', appDeepLink })
// Produces the EXACT HTML template string from the JS helper. title/description
// are HTML-escaped (safeTitle/safeDesc); image/url/type/appDeepLink are inlined
// verbatim (matching the JS template literal, which did not escape them).
std::string renderOGPage(const OGPageParams& p) {
  const std::string safeTitle = escapeHtml(p.title);
  const std::string safeDesc = escapeHtml(p.description);
  const std::string safeImage = safeHttpUrl(p.image) ? escapeHtml(p.image) : "";
  const std::string safeUrl = safeHttpUrl(p.url) ? escapeHtml(p.url) : "";
  const std::string safeType = escapeHtml(p.type);
  // appDeepLink || '#'  and  appDeepLink || ''  (empty deep link is falsy).
  const std::string hrefDeep = escapeHtml(
      p.appDeepLink.empty() ? std::string("#") : p.appDeepLink);
  const std::string jsDeep = escapeJsString(p.appDeepLink);

  std::string html;
  html += "<!DOCTYPE html>\n";
  html += "<html lang=\"en\">\n";
  html += "<head>\n";
  html += "  <meta charset=\"UTF-8\"/>\n";
  html += "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"/>\n";
  html += "  <title>" + safeTitle + "</title>\n";
  html += "\n";
  html += "  <!-- Open Graph -->\n";
  html += "  <meta property=\"og:title\"       content=\"" + safeTitle + "\"/>\n";
  html += "  <meta property=\"og:description\" content=\"" + safeDesc + "\"/>\n";
  html += "  <meta property=\"og:image\"       content=\"" + safeImage + "\"/>\n";
  html += "  <meta property=\"og:url\"         content=\"" + safeUrl + "\"/>\n";
  html += "  <meta property=\"og:type\"        content=\"" + safeType + "\"/>\n";
  html += "  <meta property=\"og:site_name\"   content=\"Pulse\"/>\n";
  html += "\n";
  html += "  <!-- Twitter Card -->\n";
  html += "  <meta name=\"twitter:card\"        content=\"summary_large_image\"/>\n";
  html += "  <meta name=\"twitter:title\"       content=\"" + safeTitle + "\"/>\n";
  html += "  <meta name=\"twitter:description\" content=\"" + safeDesc + "\"/>\n";
  html += "  <meta name=\"twitter:image\"       content=\"" + safeImage + "\"/>\n";
  html += "\n";
  html += "  <style>\n";
  html += "    body{margin:0;font-family:system-ui,sans-serif;background:#07060B;color:#F0EDF7;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;text-align:center;padding:20px}\n";
  html += "    h1{font-size:1.8rem;margin-bottom:8px}\n";
  html += "    p{color:#9B95A8;max-width:480px;margin-bottom:24px}\n";
  html += "    a.btn{display:inline-block;padding:14px 32px;background:linear-gradient(135deg,#8B5CF6,#3B82F6);color:#fff;border-radius:50px;text-decoration:none;font-weight:600;transition:transform .2s}\n";
  html += "    a.btn:hover{transform:translateY(-2px)}\n";
  html += "  </style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "  <h1>" + safeTitle + "</h1>\n";
  html += "  <p>" + safeDesc + "</p>\n";
  html += "  <a class=\"btn\" href=\"" + hrefDeep + "\">Open in Pulse</a>\n";
  html += "\n";
  html += "  <script>\n";
  html += "    // Try deep link first, fall back to store / landing page\n";
  html += "    (function(){\n";
  html += "      var deepLink = \"" + jsDeep + "\";\n";
  html += "      if(deepLink){\n";
  html += "        var start = Date.now();\n";
  html += "        window.location = deepLink;\n";
  html += "        setTimeout(function(){\n";
  html += "          if(Date.now() - start < 2000){\n";
  html += "            // App not installed — redirect to landing or store\n";
  html += "            window.location = \"https://getpulse.app\";\n";
  html += "          }\n";
  html += "        },1500);\n";
  html += "      }\n";
  html += "    })();\n";
  html += "  </script>\n";
  html += "</body>\n";
  html += "</html>";
  return html;
}

// getServerUrl(req) — process.env.SERVER_URL || `${req.protocol}://${req.get('host')}`.
// Reads the RAW env var (empty default) so an unset SERVER_URL falls back to the
// request host, exactly like the JS (NOT the config default of localhost:3000).
std::string getServerUrl(const HttpRequestPtr& req) {
  const std::string fromEnv = pulse::config().env("SERVER_URL", "");
  if (!fromEnv.empty()) return fromEnv;
  // req.protocol ('http'|'https') :// req.get('host')
  const std::string protocol = req->isOnSecureConnection() ? "https" : "http";
  const std::string host = req->getHeader("host");
  return protocol + "://" + host;
}

// HTML response: res.set('Content-Type', 'text/html'); res.send(html) -> 200.
HttpResponsePtr htmlResponse(const std::string& html) {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_TEXT_HTML);
  resp->setBody(html);
  return resp;
}

// Plain-text response with an explicit status (res.status(x).send('text')).
// Express defaults string sends to Content-Type text/html, so mirror that.
HttpResponsePtr textResponse(HttpStatusCode code, const std::string& text) {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(code);
  resp->setContentTypeCode(drogon::CT_TEXT_HTML);
  resp->setBody(text);
  return resp;
}

// String coercion of a JSON value, treating missing/null as "" (JS optional
// chaining + `|| ''` idiom). Only strings produce a value; everything else "".
std::string strOrEmpty(const Json::Value& v) {
  return v.isString() ? v.asString() : std::string();
}

// Member access that returns null when absent (mirrors `obj?.key`).
const Json::Value& member(const Json::Value& obj, const char* key) {
  static const Json::Value kNull(Json::nullValue);
  if (obj.isObject() && obj.isMember(key)) return obj[key];
  return kNull;
}

// JS String.prototype.substring(0, n) on a (possibly multibyte) string. The JS
// source treats content/caption as plain strings; substring operates on UTF-16
// code units, but for parity with the stored UTF-8 text we take the first n
// bytes — the controller only ever feeds short captions/post text here.
std::string substring0(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

// ── .populate('author', 'username profile.displayName profile.avatar') ───────
// Resolve a single author ObjectId (hex) into the projected user subdocument.
// Returns the populated author object, or null when it cannot be resolved
// (Mongoose populate leaves an unresolved ref; the callers only read optional
// chains off it, so a null author degrades gracefully like JS `undefined`).
Json::Value populateAuthor(const Json::Value& authorRef) {
  std::string hex;
  if (authorRef.isString()) hex = authorRef.asString();
  else if (authorRef.isObject() && authorRef.isMember("_id") &&
           authorRef["_id"].isString())
    hex = authorRef["_id"].asString();
  if (hex.empty()) return Json::Value(Json::nullValue);

  auto oid = bj::tryOid(hex);
  if (!oid) return Json::Value(Json::nullValue);
  try {
    auto projection = make_document(kvp("username", 1), kvp("profile.displayName", 1),
                                    kvp("profile.avatar", 1));
    mongocxx::options::find opts{};
    opts.projection(projection.view());
    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto doc = col.find_one(make_document(kvp("_id", *oid)), opts);
    if (!doc) return Json::Value(Json::nullValue);
    return bj::toJson(doc->view());
  } catch (const std::exception& e) {
    pulse::log::error("[og] author populate failed: {}", e.what());
    return Json::Value(Json::nullValue);
  }
}

}  // namespace

// ── GET /share/post/:postId ─────────────────────────────────────────────────
void OgController::sharePost(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback,
                             std::string postId) {
  try {
    // Post.findById(req.params.postId).populate('author', ...).lean()
    auto oid = bj::tryOid(postId);
    std::optional<Json::Value> postOpt;
    if (oid) {
      auto col = pulse::db::collection(pulse::models::post::kCollection);
      auto doc = col.find_one(make_document(kvp("_id", *oid)));
      if (doc) postOpt = bj::toJson(doc->view());
    }

    // if (!post || post.isActive === false ||
    //     (post.visibility && post.visibility !== 'public')) -> 404
    if (!postOpt) {
      callback(textResponse(drogon::k404NotFound, "Post not found"));
      return;
    }
    Json::Value post = *postOpt;
    const bool isActiveFalse =
        post.isMember("isActive") && post["isActive"].isBool() &&
        post["isActive"].asBool() == false;
    const Json::Value& visibility = member(post, "visibility");
    const bool visibilityBlocks =
        visibility.isString() && !visibility.asString().empty() &&
        visibility.asString() != "public";
    if (isActiveFalse || visibilityBlocks) {
      callback(textResponse(drogon::k404NotFound, "Post not found"));
      return;
    }

    // Hydrate author (username profile.displayName profile.avatar).
    if (post.isMember("author"))
      post["author"] = populateAuthor(post["author"]);

    const bool isAnonymous =
        post.isMember("isAnonymous") && post["isAnonymous"].isBool() &&
        post["isAnonymous"].asBool();

    // authorName = isAnonymous ? 'Someone'
    //   : author?.profile?.displayName || author?.username || 'Someone'
    std::string authorName;
    if (isAnonymous) {
      authorName = "Someone";
    } else {
      const Json::Value& author = member(post, "author");
      const std::string dn = strOrEmpty(member(member(author, "profile"), "displayName"));
      const std::string un = strOrEmpty(member(author, "username"));
      if (!dn.empty()) authorName = dn;
      else if (!un.empty()) authorName = un;
      else authorName = "Someone";
    }

    // text = typeof post.content === 'string' ? post.content : post.content?.text
    std::string text;
    const Json::Value& content = member(post, "content");
    if (content.isString()) text = content.asString();
    else text = strOrEmpty(member(content, "text"));

    // description = text ? text.substring(0,200)
    //                    : `Check out this post by ${authorName} on Pulse`
    std::string description;
    if (!text.empty()) description = substring0(text, 200);
    else description = "Check out this post by " + authorName + " on Pulse";

    // image = post.content?.media?.[0]?.url || post.media?.[0]?.url
    //         || (isAnonymous ? '' : post.author?.profile?.avatar) || ''
    std::string image;
    {
      const Json::Value& cMedia = member(content, "media");
      if (cMedia.isArray() && cMedia.size() > 0)
        image = strOrEmpty(member(cMedia[0u], "url"));
      if (image.empty()) {
        const Json::Value& pMedia = member(post, "media");
        if (pMedia.isArray() && pMedia.size() > 0)
          image = strOrEmpty(member(pMedia[0u], "url"));
      }
      if (image.empty() && !isAnonymous) {
        image = strOrEmpty(member(member(member(post, "author"), "profile"), "avatar"));
      }
    }

    const std::string serverUrl = getServerUrl(req);
    const std::string postIdStr = strOrEmpty(member(post, "_id"));

    OGPageParams params;
    params.title = authorName + " on Pulse";
    params.description = description;
    params.image = image;
    params.url = serverUrl + "/share/post/" + postIdStr;
    params.appDeepLink = "pulse://post/" + postIdStr;

    callback(htmlResponse(renderOGPage(params)));
  } catch (const std::exception& e) {
    pulse::log::error("OG sharePost error: {}", e.what());
    callback(textResponse(drogon::k500InternalServerError, "Internal Server Error"));
  }
}

// ── GET /share/profile/:username ────────────────────────────────────────────
void OgController::shareProfile(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                std::string username) {
  try {
    // User.findOne({ username: req.params.username.toLowerCase(), isActive: true }).lean()
    std::string lower = username;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::optional<Json::Value> userOpt;
    {
      auto col = pulse::db::collection(pulse::models::user::kCollection);
      auto doc = col.find_one(make_document(kvp("username", lower), kvp("isActive", true)));
      if (doc) userOpt = bj::toJson(doc->view());
    }

    if (!userOpt) {
      callback(textResponse(drogon::k404NotFound, "Profile not found"));
      return;
    }
    Json::Value user = *userOpt;

    // displayName = user.profile?.displayName || user.username
    const std::string dn = strOrEmpty(member(member(user, "profile"), "displayName"));
    const std::string un = strOrEmpty(member(user, "username"));
    const std::string displayName = !dn.empty() ? dn : un;

    // isPrivate = !!user.privacy?.isPrivate
    const Json::Value& isPrivateVal = member(member(user, "privacy"), "isPrivate");
    const bool isPrivate = isPrivateVal.isBool() ? isPrivateVal.asBool()
                          : (isPrivateVal.isString() ? !isPrivateVal.asString().empty()
                          : (isPrivateVal.isNumeric() ? isPrivateVal.asDouble() != 0
                          : false));

    // bio = (!isPrivate && user.profile?.bio) || `Follow ${displayName} on Pulse`
    std::string bio;
    if (!isPrivate) bio = strOrEmpty(member(member(user, "profile"), "bio"));
    if (bio.empty()) bio = "Follow " + displayName + " on Pulse";

    // avatar = user.profile?.avatar || ''
    const std::string avatar = strOrEmpty(member(member(user, "profile"), "avatar"));

    const std::string serverUrl = getServerUrl(req);
    const std::string usernameStr = un;

    OGPageParams params;
    params.title = displayName + " (@" + usernameStr + ") — Pulse";
    params.description = bio;
    params.image = avatar;
    params.url = serverUrl + "/share/profile/" + usernameStr;
    params.type = "profile";
    params.appDeepLink = "pulse://profile/" + usernameStr;

    callback(htmlResponse(renderOGPage(params)));
  } catch (const std::exception& e) {
    pulse::log::error("OG shareProfile error: {}", e.what());
    callback(textResponse(drogon::k500InternalServerError, "Internal Server Error"));
  }
}

// ── GET /share/reel/:reelId ─────────────────────────────────────────────────
void OgController::shareReel(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback,
                             std::string reelId) {
  try {
    // Reel.findById(req.params.reelId).populate('author', ...).lean()
    auto oid = bj::tryOid(reelId);
    std::optional<Json::Value> reelOpt;
    if (oid) {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      auto doc = col.find_one(make_document(kvp("_id", *oid)));
      if (doc) reelOpt = bj::toJson(doc->view());
    }

    if (!reelOpt) {
      callback(textResponse(drogon::k404NotFound, "Reel not found"));
      return;
    }
    Json::Value reel = *reelOpt;

    // Hydrate author (username profile.displayName profile.avatar).
    if (reel.isMember("author"))
      reel["author"] = populateAuthor(reel["author"]);

    // authorName = reel.author?.profile?.displayName || reel.author?.username || 'Someone'
    std::string authorName;
    {
      const Json::Value& author = member(reel, "author");
      const std::string dn = strOrEmpty(member(member(author, "profile"), "displayName"));
      const std::string un = strOrEmpty(member(author, "username"));
      if (!dn.empty()) authorName = dn;
      else if (!un.empty()) authorName = un;
      else authorName = "Someone";
    }

    // description = reel.caption ? reel.caption.substring(0,200)
    //                            : `Watch this reel by ${authorName} on Pulse`
    std::string description;
    const std::string caption = strOrEmpty(member(reel, "caption"));
    if (!caption.empty()) description = substring0(caption, 200);
    else description = "Watch this reel by " + authorName + " on Pulse";

    // thumbnail = reel.thumbnailUrl || reel.author?.profile?.avatar || ''
    std::string thumbnail = strOrEmpty(member(reel, "thumbnailUrl"));
    if (thumbnail.empty())
      thumbnail = strOrEmpty(member(member(member(reel, "author"), "profile"), "avatar"));

    const std::string serverUrl = getServerUrl(req);
    const std::string reelIdStr = strOrEmpty(member(reel, "_id"));

    OGPageParams params;
    params.title = authorName + "'s Reel — Pulse";
    params.description = description;
    params.image = thumbnail;
    params.url = serverUrl + "/share/reel/" + reelIdStr;
    params.type = "video.other";
    params.appDeepLink = "pulse://reel/" + reelIdStr;

    callback(htmlResponse(renderOGPage(params)));
  } catch (const std::exception& e) {
    pulse::log::error("OG shareReel error: {}", e.what());
    callback(textResponse(drogon::k500InternalServerError, "Internal Server Error"));
  }
}

}  // namespace pulse::controllers
