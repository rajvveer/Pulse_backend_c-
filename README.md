# Pulse Backend (C++)

A **1:1 C++ port** of the Pulse hyperlocal social-network backend, originally
written in Node.js/Express. Same REST API surface, same WebSocket events, same
MongoDB collections + indexes, same Redis keys/TTLs, same response shapes and
error codes — so it is **drop-in compatible** with the existing database and the
mobile / web / admin frontends.

Built on:

| Concern            | Node original                | C++ port                              |
|--------------------|------------------------------|---------------------------------------|
| HTTP server        | Express 5                    | **Drogon** (`HttpController`)         |
| WebSocket realtime | Socket.IO + Redis adapter    | **Drogon `WebSocketController`** + Redis pub/sub |
| Database           | Mongoose / MongoDB           | **mongocxx** (MongoDB C++ driver)     |
| Cache / rate limit | ioredis                      | **redis-plus-plus**                   |
| JWT                | jsonwebtoken                 | **jwt-cpp** (HS256, pinned)           |
| Password hashing   | bcryptjs (cost 12)           | **libbcrypt**                         |
| JSON               | native                       | **JsonCpp** (`Json::Value`)           |
| Ranking algorithms | C++ N-API addon              | **same C++ kernels, in-process**      |
| Logging            | winston                      | **spdlog**                            |

## Layout

```
pulse-backend-cpp/
├─ CMakeLists.txt          # build (vcpkg manifest mode)
├─ vcpkg.json              # dependency manifest
├─ Dockerfile             # multi-stage (vcpkg build -> slim runtime)
├─ config.json             # Drogon runtime config
├─ .env.example            # env contract (same keys as the Node backend)
├─ include/pulse/          # shared headers (contracts)
│  ├─ config / logger / http_response / db / cache / jwt_service / bson_json
│  ├─ algorithms.hpp + common.hpp + json.hpp   # the 8 ranking kernels
│  ├─ models/*.hpp         # 23 model ports
│  ├─ services/*.hpp       # service-layer ports
│  ├─ filters/*.hpp        # middleware (auth, rate limit, sanitize, upload)
│  ├─ controllers/*.hpp    # route groups
│  └─ sockets/*.hpp        # realtime / presence WebSocket controllers
└─ src/
   ├─ main.cc              # bootstrap (server.js + app.js)
   ├─ config/ db/ models/ services/ filters/ controllers/ sockets/ jobs/ utils/
   └─ algorithms/*.cc      # the 8 kernels (copied verbatim from native/src)
```

### How the layers map to the original

- **Models** (`src/models`): each Mongoose schema → a `pulse::models::<name>`
  namespace with the exact collection name, `ensureIndexes()` (every schema
  index), the statics/instance helpers that carried query logic, `applyDefaults`
  (schema defaults on insert) and `sanitizeForOutput` (the `toJSON` transform).
- **Services** (`src/services`): each service module → a singleton class /
  namespace. Same Mongo queries, same Redis keys + TTLs, same external-API
  calls (Cloudinary, Twilio, Brevo, FCM, Firebase, Gemini/OpenAI) via Drogon's
  `HttpClient`. Methods return `Json::Value` (the exact JS response objects) and
  throw `std::runtime_error` with the JS error text on failure.
- **Filters** (`src/filters`): Express middleware → Drogon `HttpFilter`s
  (`AuthFilter`, `OptionalAuthFilter`, `RequireVerifiedFilter`,
  `RequireAdminFilter`, the rate limiters, `SanitizeFilter`, upload guards).
- **Controllers** (`src/controllers`): each route group → a Drogon
  `HttpController` with the exact paths + ordered filters. Handlers validate
  input exactly as the JS did and return `pulse::http::*` responses matching the
  `{ success, error, code }` convention byte-for-byte.
- **Sockets** (`src/sockets`): the Socket.IO realtime + presence handlers →
  `WebSocketController`s. JWT-authenticated handshake, `user_<id>` rooms, and
  Redis pub/sub for cross-instance fan-out (the Socket.IO Redis adapter
  analogue). Event names and payload shapes are preserved exactly.
- **Algorithms** (`src/algorithms`): the 8 ranking/classification kernels
  (`feedRank`, `reelRank`, `commentsRank`, `userRank`, `dnaMatch`,
  `vibeClassify`, `moodDetect`, `interestScore`) are the **same C++ source** the
  Node addon used, now called in-process via `pulse::algos::*` — no N-API, no JS
  round-trip.

> **WebSocket protocol note:** Socket.IO uses its own Engine.IO framing on top
> of WebSocket. This port speaks raw WebSocket with one JSON text frame per event
> (`{ event, data, ack? }`). Event names and payload shapes are identical, but a
> Socket.IO client needs a thin framing adapter, or point the client at the raw
> WS endpoints (`/ws`, `/ws/presence`).

## Build

Requires a C++17 toolchain, CMake ≥ 3.16, and [vcpkg](https://github.com/microsoft/vcpkg).

```bash
# 1. Get vcpkg (once)
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh

# 2. Configure + build (vcpkg resolves Drogon, mongocxx, redis-plus-plus, …)
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j

# 3. Run
cp .env.example .env   # fill in secrets
./build/pulse_backend
```

### Docker

```bash
docker build -t pulse-backend-cpp .
docker run --env-file .env -p 3000:3000 pulse-backend-cpp
```

## Configuration

Reads the **same environment variables** as the Node backend (see `.env.example`).
The required secrets (`JWT_SECRET`, `JWT_REFRESH_SECRET`, `TEMP_JWT_SECRET`, and
in production `MONGO_URI` + `SESSION_SECRET`) are enforced at boot, and Redis is
required in production — identical to the original.

## Health & ops

- `GET /health` — liveness (503 while draining)
- `GET /health/ready` — readiness (checks Mongo + Redis)
- `GET /metrics` — Prometheus text (internal-only in prod)
- `GET /status` — API status (internal-only in prod)

Graceful shutdown drains on `SIGTERM`/`SIGINT` (flip readiness → drain → close).

## Parity notes

This port aims for behavioral parity, not line-by-line translation. Where the
Node stack relied on a library with no C++ equivalent (nodemailer/Brevo,
firebase-admin), the exact wire behavior is reproduced over Drogon's HTTP client
against the same provider APIs. See `docs/` for the per-subsystem port spec and
build logs generated during the port.
```
