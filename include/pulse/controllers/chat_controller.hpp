// chat_controller.hpp — Drogon HttpController porting src/controllers/chatController.js
// + src/routes/chatRoutes.js (mounted at /api/v1/chat).
//
// 1:1 functional parity with the Express chat route group. Every route in the JS
// router runs `verifyAccessToken` first, which maps to pulse::filters::AuthFilter.
// The authenticated user is read back from the request attributes ("user") inside
// each handler, exactly as the JS read req.user.userId.
//
// Route map (FULL path = /api/v1/chat + sub-path from chatRoutes.js):
//   POST   /api/v1/chat/conversation              -> getOrCreateConversation
//   GET    /api/v1/chat/conversations             -> getConversations
//   GET    /api/v1/chat/search                    -> searchConversations
//   GET    /api/v1/chat/{conversationId}          -> getConversationDetails
//   GET    /api/v1/chat/{conversationId}/messages -> getMessages
//   POST   /api/v1/chat/{conversationId}/read     -> markConversationRead
//   DELETE /api/v1/chat/messages/{messageId}      -> deleteMessage
//
// NOTE on route ordering: Drogon matches ADD_METHOD_TO patterns; the literal
// paths (/conversations, /search, /messages/{id}) are registered alongside the
// parameterized /{conversationId}. Drogon's router prefers more specific (literal)
// matches, mirroring how the Express router resolved /search and
// /messages/:messageId ahead of the bare /:conversationId GET.
#pragma once
#include <drogon/HttpController.h>
#include <functional>
#include <string>

namespace pulse::controllers {

using namespace drogon;

class ChatController : public drogon::HttpController<ChatController> {
public:
  METHOD_LIST_BEGIN
  // POST /api/v1/chat/conversation  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::getOrCreateConversation,
                "/api/v1/chat/conversation", Post,
                "pulse::filters::AuthFilter");

  // GET /api/v1/chat/conversations  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::getConversations,
                "/api/v1/chat/conversations", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/chat/search  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::searchConversations,
                "/api/v1/chat/search", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/chat/:conversationId/messages  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::getMessages,
                "/api/v1/chat/{1}/messages", Get,
                "pulse::filters::AuthFilter");

  // POST /api/v1/chat/:conversationId/read  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::markConversationRead,
                "/api/v1/chat/{1}/read", Post,
                "pulse::filters::AuthFilter");

  // DELETE /api/v1/chat/messages/:messageId  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::deleteMessage,
                "/api/v1/chat/messages/{1}", Delete,
                "pulse::filters::AuthFilter");

  // GET /api/v1/chat/:conversationId  (verifyAccessToken)
  ADD_METHOD_TO(ChatController::getConversationDetails,
                "/api/v1/chat/{1}", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // POST /api/v1/chat/conversation
  void getOrCreateConversation(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/chat/conversations
  void getConversations(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/chat/search
  void searchConversations(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/chat/:conversationId
  void getConversationDetails(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback,
                              std::string conversationId);

  // GET /api/v1/chat/:conversationId/messages
  void getMessages(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string conversationId);

  // POST /api/v1/chat/:conversationId/read
  void markConversationRead(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback,
                            std::string conversationId);

  // DELETE /api/v1/chat/messages/:messageId
  void deleteMessage(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     std::string messageId);
};

} // namespace pulse::controllers
