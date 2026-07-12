// social_dna_controller.hpp — Drogon HttpController porting
// src/controllers/socialDNAController.js and its route group
// src/routes/socialDNARoutes.js (mounted at /api/v1/social-dna).
//
// Every route requires verifyAccessToken (router.use(verifyAccessToken)) ->
// pulse::filters::AuthFilter. Handlers are thin: read the authed user / params /
// query, validate EXACTLY as the JS did, delegate to the existing ports
// (pulse::models::socialdna for getOrCreate / getShareCardData, and the
// DNA-match scoring kernel pulse::algos::dnaMatch — NOT reimplemented here), and
// reply with the EXACT JS res.json shapes/status codes.
//
// NOTE: socialDNAController.js uses the LEGACY error shape on failure:
//   res.status(500).json({ success: false, error: '<message>' })  — NO `code`.
// So error replies here build { success:false, error } directly (NOT
// pulse::http::error, which would add a `code`).
//
// Routes (full path = /api/v1/social-dna + sub-path from socialDNARoutes.js):
//   GET  /api/v1/social-dna/me                     -> getMyDNA
//   GET  /api/v1/social-dna/share-card             -> getShareCard
//   POST /api/v1/social-dna/share                  -> recordShare
//   GET  /api/v1/social-dna/evolution              -> getEvolution
//   GET  /api/v1/social-dna/twins                  -> findTwins
//   GET  /api/v1/social-dna/match/:targetUserId    -> getCompatibility
//   GET  /api/v1/social-dna/user/:userId           -> getUserDNA
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class SocialDNAController : public drogon::HttpController<SocialDNAController> {
public:
  METHOD_LIST_BEGIN
  // router.get('/me', socialDNAController.getMyDNA)
  ADD_METHOD_TO(SocialDNAController::getMyDNA, "/api/v1/social-dna/me", Get,
                "pulse::filters::AuthFilter");

  // router.get('/share-card', socialDNAController.getShareCard)
  ADD_METHOD_TO(SocialDNAController::getShareCard, "/api/v1/social-dna/share-card",
                Get, "pulse::filters::AuthFilter");

  // router.post('/share', socialDNAController.recordShare)
  ADD_METHOD_TO(SocialDNAController::recordShare, "/api/v1/social-dna/share", Post,
                "pulse::filters::AuthFilter");

  // router.get('/evolution', socialDNAController.getEvolution)
  ADD_METHOD_TO(SocialDNAController::getEvolution, "/api/v1/social-dna/evolution",
                Get, "pulse::filters::AuthFilter");

  // router.get('/twins', socialDNAController.findTwins)
  ADD_METHOD_TO(SocialDNAController::findTwins, "/api/v1/social-dna/twins", Get,
                "pulse::filters::AuthFilter");

  // router.get('/match/:targetUserId', socialDNAController.getCompatibility)
  ADD_METHOD_TO(SocialDNAController::getCompatibility,
                "/api/v1/social-dna/match/{1}", Get, "pulse::filters::AuthFilter");

  // router.get('/user/:userId', socialDNAController.getUserDNA)
  ADD_METHOD_TO(SocialDNAController::getUserDNA, "/api/v1/social-dna/user/{1}", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/social-dna/me — my DNA profile (strands, dominantVibe, totals,
  // streak, weeks, latest insights, lastComputedAt).
  void getMyDNA(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/social-dna/share-card — shareable card data for my DNA.
  void getShareCard(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/social-dna/share — record a card share (viral tracking).
  void recordShare(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/social-dna/evolution — weekly snapshots (?weeks=N, default 12).
  void getEvolution(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/social-dna/twins — my DNA twins (?limit=N, default 20).
  void findTwins(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/social-dna/match/:targetUserId — compatibility with another user.
  void getCompatibility(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        std::string targetUserId);

  // GET /api/v1/social-dna/user/:userId — another user's public DNA.
  void getUserDNA(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string userId);
};

} // namespace pulse::controllers
