// notification.hpp — C++ port of src/models/Notification.js (Mongoose model).
//
// Notifications delivered to a user (likes, comments, follows, chats, whispers,
// mentions, reel likes/comments). This header exposes the collection name, the
// index spec, the schema statics that carry query logic, and the
// defaults/sanitize helpers that mirror Mongoose's insert + toJSON behavior.
//
// Field names and the collection name match the JS schema EXACTLY.
#pragma once
#include <json/json.h>
#include <optional>
#include <string>

namespace pulse::models::notification {

// Mongoose: collection: 'notifications'
inline constexpr const char* kCollection = "notifications";

// Allowed values for the `type` field (Mongoose enum).
// ['like','comment','follow','chat','whisper','mention','reel_like','reel_comment']

// Creates every index the schema declares:
//   - { recipient: 1 }                              (field-level index: true)
//   - { isRead: 1 }                                 (field-level index: true)
//   - { recipient: 1, createdAt: -1 }
//   - { recipient: 1, isRead: 1, createdAt: -1 }
//   - { recipient: 1, type: 1, createdAt: -1 }
void ensureIndexes();

// ---- Schema statics (query logic) --------------------------------------------

// Options for getNotifications (defaults mirror the JS destructuring).
struct GetNotificationsOptions {
  int page = 1;
  int limit = 20;
  std::optional<std::string> type;   // null => no type filter
  bool unreadOnly = false;
};

// Static: getNotifications(userId, options) — paginated list + pagination meta.
// Returns { notifications: [...], pagination: { page, limit, total, hasMore } }.
Json::Value getNotifications(const std::string& userId,
                             const GetNotificationsOptions& options = {});

// Static: getUnreadCount(userId) — countDocuments({recipient, isRead:false}).
long long getUnreadCount(const std::string& userId);

// Static: getUnreadCountByType(userId) — aggregate group-by type for unread.
// Returns { total, <type>: count, ... }.
Json::Value getUnreadCountByType(const std::string& userId);

// Static: markAsRead(notificationId, userId) — findOneAndUpdate, returns the
// updated doc (new:true) or std::nullopt if not matched.
std::optional<Json::Value> markAsRead(const std::string& notificationId,
                                      const std::string& userId);

// Static: markAllAsRead(userId, type=null) — updateMany({recipient, isRead:false}
// [, type]) -> { isRead:true }. Returns the number of documents modified.
long long markAllAsRead(const std::string& userId,
                        const std::optional<std::string>& type = std::nullopt);

// Static: createNotification(data) — de-duplicated insert.
//   - returns null (std::nullopt) when sender == recipient
//   - returns an existing recent duplicate (within 60s, same recipient/sender/
//     type/post/reel) if found
//   - otherwise inserts (with schema defaults applied) and returns the new doc.
// NOTE: the JS push-notification side effect (setImmediate) is intentionally NOT
// performed here; it belongs to the push service and is handled at the call site.
std::optional<Json::Value> createNotification(const Json::Value& data);

// Static: deleteOldNotifications(daysOld=30) — deleteMany read notifications
// older than the cutoff. Returns the number of documents deleted.
long long deleteOldNotifications(int daysOld = 30);

// ---- Insert defaults / output transform --------------------------------------

// Fills in schema defaults on insert: isRead=false (if absent), createdAt/
// updatedAt timestamps, __v=0. Does NOT mutate the input (returns a copy).
Json::Value applyDefaults(Json::Value doc);

// Mirrors Mongoose's default toJSON: strips the internal version key `__v`.
// (This schema declares no select:false fields and no custom transform.)
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::notification
