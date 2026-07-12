// group_controller.hpp — Drogon HttpController porting src/controllers/groupController.js
// and the route group src/routes/groupRoutes.js (mounted at /api/v1/groups).
//
// 1:1 functional parity with the Express router: same sub-paths, HTTP methods,
// ordered middleware (as filters), request/response JSON shapes, status codes,
// and error/message strings.
//
// Middleware -> filter mapping (preserved in order):
//   verifyAccessToken -> pulse::filters::AuthFilter   (every route)
//
// Routes (full path = /api/v1/groups + sub-path from groupRoutes.js):
//   POST   /api/v1/groups                            -> createGroup
//   GET    /api/v1/groups/{groupId}                  -> getGroupDetails
//   PUT    /api/v1/groups/{groupId}                  -> updateGroupInfo
//   DELETE /api/v1/groups/{groupId}                  -> deleteGroup
//   POST   /api/v1/groups/{groupId}/members          -> addGroupMembers
//   DELETE /api/v1/groups/{groupId}/members/{userId} -> removeGroupMember
//   POST   /api/v1/groups/{groupId}/leave            -> leaveGroup
//   POST   /api/v1/groups/{groupId}/admins           -> makeAdmin
//   DELETE /api/v1/groups/{groupId}/admins/{userId}  -> removeAdmin
#pragma once
#include <drogon/HttpController.h>

#include <string>

namespace pulse::controllers {

using namespace drogon;

class GroupController : public drogon::HttpController<GroupController> {
 public:
  METHOD_LIST_BEGIN
  // Group CRUD
  ADD_METHOD_TO(GroupController::createGroup,
                "/api/v1/groups", Post, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GroupController::getGroupDetails,
                "/api/v1/groups/{1}", Get, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GroupController::updateGroupInfo,
                "/api/v1/groups/{1}", Put, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GroupController::deleteGroup,
                "/api/v1/groups/{1}", Delete, "pulse::filters::AuthFilter");

  // Member management
  ADD_METHOD_TO(GroupController::addGroupMembers,
                "/api/v1/groups/{1}/members", Post, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GroupController::removeGroupMember,
                "/api/v1/groups/{1}/members/{2}", Delete, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GroupController::leaveGroup,
                "/api/v1/groups/{1}/leave", Post, "pulse::filters::AuthFilter");

  // Admin management
  ADD_METHOD_TO(GroupController::makeAdmin,
                "/api/v1/groups/{1}/admins", Post, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GroupController::removeAdmin,
                "/api/v1/groups/{1}/admins/{2}", Delete, "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // POST /api/v1/groups
  void createGroup(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/groups/{groupId}
  void getGroupDetails(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       std::string groupId);

  // PUT /api/v1/groups/{groupId}
  void updateGroupInfo(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       std::string groupId);

  // DELETE /api/v1/groups/{groupId}
  void deleteGroup(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string groupId);

  // POST /api/v1/groups/{groupId}/members
  void addGroupMembers(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       std::string groupId);

  // DELETE /api/v1/groups/{groupId}/members/{userId}
  void removeGroupMember(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         std::string groupId, std::string userId);

  // POST /api/v1/groups/{groupId}/leave
  void leaveGroup(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string groupId);

  // POST /api/v1/groups/{groupId}/admins
  void makeAdmin(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string groupId);

  // DELETE /api/v1/groups/{groupId}/admins/{userId}
  void removeAdmin(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string groupId, std::string userId);
};

}  // namespace pulse::controllers
