# C++ Porting Conventions (read before writing any file)

You are porting the Pulse Node.js/Express backend to C++ using the **Drogon**
web framework, **mongocxx** (MongoDB), **redis-plus-plus** (Redis), **jwt-cpp**,
and **JsonCpp** (`Json::Value`). The goal is **1:1 functional parity** with the
original — same routes, same request/response JSON shapes, same status codes,
same error `code` strings, same DB collections/queries, same Redis keys/TTLs.

## Project layout
```
pulse-backend-cpp/
  include/pulse/        # shared headers (ALREADY WRITTEN — include & use these)
    config.hpp          # pulse::config()  — env + typed config
    logger.hpp          # pulse::log::info/warn/error/debug
    http_response.hpp   # pulse::http::success/ok/error/badRequest/... (Drogon HttpResponsePtr)
    db.hpp              # pulse::db::collection("name"), ClientHandle, connect/isHealthy/createIndexes
    cache.hpp           # pulse::cache()  — get/set/del/getOrSet/delPattern/incrementRateLimit
    jwt_service.hpp     # pulse::jwt()    — generate/verify access/refresh/temp tokens
    bson_json.hpp       # pulse::bsonjson::toJson(doc), fromJson(json), oid(), nowIso8601()
    algorithms.hpp      # pulse::algos::feedRank(json)->json, vibeClassify, etc.
    common.hpp,json.hpp # algorithm kernel helpers (do not modify)
  src/
    config/  db/  models/  services/  filters/  controllers/  sockets/  jobs/  utils/
    main.cc             # bootstrap (server.js + app.js equivalent)
```

## Shared contracts (USE THESE — do not reinvent)
- **DB access**: `auto col = pulse::db::collection("users");` returns a
  `mongocxx::collection`. Build filters/updates with bsoncxx basic builders.
  Convert a fetched doc to a response with `pulse::bsonjson::toJson(doc.view())`.
- **ObjectId**: `pulse::bsonjson::oid(hexStr)` (throws if invalid),
  `tryOid(hexStr)` -> `std::optional`, `oidToHex(id)`.
- **Cache**: `pulse::cache().get(key)` -> `std::optional<std::string>` (JSON
  string), `.set(key, jsonStr, ttlSeconds)`, `.getOrSet(key, fetchFn, ttl)`,
  `.delPattern("feed:user:123:*")`, `.incrementRateLimit(key, ttl)`.
  **Preserve the exact key formats and TTLs from the JS source.**
- **JWT**: `pulse::jwt().verifyAccessToken(token)` -> `AccessClaims{userId,
  username, email, isVerified}` (throws `pulse::JwtError`). `generateTokenPair(...)`.
- **Responses**: return `pulse::http::success({...})`,
  `pulse::http::error(drogon::k404NotFound, "msg", "CODE")`, etc. Match the JS
  `res.status(x).json({ success, error, code, ... })` exactly — same field names,
  same `code` strings (e.g. `MISSING_ACCESS_TOKEN`, `USER_NOT_FOUND`,
  `VALIDATION_ERROR`, `DUPLICATE_ERROR`).
- **Logging**: `pulse::log::info("msg {}", arg);` (spdlog/fmt formatting).
- **Auth context**: the auth filter stores the authenticated user on the request
  via `req->getAttributes()->insert("user", userJson)` where userJson is a
  `Json::Value{userId, username, email, isVerified}`. Controllers read it back
  with `req->getAttributes()->get<Json::Value>("user")`.

## Controller pattern (Drogon HttpController)
Each route group becomes a class derived from
`drogon::HttpController<ClassName>`. Declare routes in the header with
`METHOD_LIST_BEGIN / ADD_METHOD_TO / METHOD_LIST_END`, attaching filters by
class name. Handlers have the signature:
```cpp
void handler(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);
```
Bind path params via `{1}` placeholders mapped to trailing handler args.
Mirror the EXACT path from the Express router (e.g. `/api/v1/auth/initiate`).
Read JSON body with `req->getJsonObject()` (a `std::shared_ptr<Json::Value>`),
query params with `req->getParameter("limit")`, headers with `req->getHeader(...)`.

## Filters (middleware)
Express middleware becomes Drogon filters (`drogon::HttpFilter<T>`):
`AuthFilter` (verifyAccessToken), `OptionalAuthFilter`, `RequireVerifiedFilter`,
`RequireAdminFilter`, rate-limit filters, `SanitizeFilter`. A filter either
calls `fccb()` to continue or `fcb(response)` to short-circuit.

## Models
Each Mongoose model becomes a `namespace pulse::models::<Name>` (or a class)
exposing: the collection name, an index spec function `ensureIndexes()`, and the
statics/instance helpers that carried real logic (e.g.
`User::findByAuthMethod`, `OTP::findValidOTP`, `Follow::isFollowing`). Field
defaults, enums, and validation that the controllers rely on must be applied on
insert. Keep the SAME field names and SAME collection name as the JS schema.
Register every model's `ensureIndexes` so `src/db/indexes.cc::ensureAllIndexes()`
can call them all.

## Async note
Drogon handlers may run blocking mongocxx/redis calls on its worker threads;
prefer `drogon::app().getLoop()->queueInLoop` only where the JS was async in a
way that matters. For parity, synchronous DB calls inside the handler are
acceptable (Drogon has a thread pool). Do NOT block the event loop in
WebSocket handlers — offload heavy work.

## Hard rules
- Do not change route paths, JSON field names, status codes, or error `code`s.
- Do not invent new endpoints; port exactly what exists.
- Preserve Redis key strings and TTL values verbatim.
- Every `.cc` you create must `#include` the relevant `pulse/*.hpp` and compile
  against the contracts above.
- Write a matching `.hpp` in `include/pulse/` (or alongside) when other files
  need the symbol; keep controllers self-contained where possible.
```
