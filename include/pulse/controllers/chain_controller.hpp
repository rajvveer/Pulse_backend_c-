// chain_controller.hpp — Drogon HttpController porting src/controllers/chainController.js
// and its route group src/routes/chainRoutes.js (mounted at /api/v1/chains).
//
// Every route requires verifyAccessToken (router.use(verifyAccessToken)) ->
// pulse::filters::AuthFilter. Handlers are thin: parse the same params / body /
// authed user, validate EXACTLY as the JS did, delegate the embedded-subdocument
// mutations to pulse::models::chainstory (submitSegment / voteOnSegment /
// toggleLike — NOT reimplemented here), perform the controller-layer
// .populate('starterAuthor'/'segments.author'/'contributors', 'username
// profile.avatar') joins, and reply with the EXACT JS res.json shapes/status
// codes ({ success, data } / { success, message }).
//
// Routes (full path = /api/v1/chains + sub-path from chainRoutes.js):
//   GET  /api/v1/chains                                  -> getChains
//   GET  /api/v1/chains/:chainId                         -> getById
//   POST /api/v1/chains                                  -> create
//   POST /api/v1/chains/:chainId/segment                 -> submitSegment
//   GET  /api/v1/chains/:chainId/pending                 -> getPending
//   POST /api/v1/chains/:chainId/segment/:segmentId/vote -> voteSegment
//   POST /api/v1/chains/:chainId/like                    -> likeChain
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class ChainController : public drogon::HttpController<ChainController> {
public:
  METHOD_LIST_BEGIN
  // router.get('/', chainController.getChains)
  ADD_METHOD_TO(ChainController::getChains, "/api/v1/chains", Get,
                "pulse::filters::AuthFilter");

  // router.post('/', chainController.create)
  ADD_METHOD_TO(ChainController::create, "/api/v1/chains", Post,
                "pulse::filters::AuthFilter");

  // router.get('/:chainId', chainController.getById)
  ADD_METHOD_TO(ChainController::getById, "/api/v1/chains/{1}", Get,
                "pulse::filters::AuthFilter");

  // router.post('/:chainId/segment', chainController.submitSegment)
  ADD_METHOD_TO(ChainController::submitSegment, "/api/v1/chains/{1}/segment", Post,
                "pulse::filters::AuthFilter");

  // router.get('/:chainId/pending', chainController.getPending)
  ADD_METHOD_TO(ChainController::getPending, "/api/v1/chains/{1}/pending", Get,
                "pulse::filters::AuthFilter");

  // router.post('/:chainId/segment/:segmentId/vote', chainController.voteSegment)
  ADD_METHOD_TO(ChainController::voteSegment,
                "/api/v1/chains/{1}/segment/{2}/vote", Post,
                "pulse::filters::AuthFilter");

  // router.post('/:chainId/like', chainController.likeChain)
  ADD_METHOD_TO(ChainController::likeChain, "/api/v1/chains/{1}/like", Post,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/chains — active chains (filtered/paginated), starterAuthor
  // populated, lastSegment virtual attached.
  void getChains(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/chains — create a new chain.
  void create(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/chains/:chainId — single chain with full story (all authors
  // populated).
  void getById(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               std::string chainId);

  // POST /api/v1/chains/:chainId/segment — submit a pending segment.
  void submitSegment(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     std::string chainId);

  // GET /api/v1/chains/:chainId/pending — pending segments for voting.
  void getPending(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string chainId);

  // POST /api/v1/chains/:chainId/segment/:segmentId/vote — vote on a pending
  // segment.
  void voteSegment(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string chainId, std::string segmentId);

  // POST /api/v1/chains/:chainId/like — like a chain.
  void likeChain(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string chainId);
};

} // namespace pulse::controllers
