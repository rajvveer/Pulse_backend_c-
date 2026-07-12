// db.hpp — MongoDB connection layer, ports src/config/database.js.
//
// Wraps the mongocxx driver. A single mongocxx::instance lives for the whole
// process (driver requirement); a mongocxx::pool provides per-operation client
// checkout (the C++ analogue of Mongoose's connection pool). Collections are
// fetched by name from the configured database.
#pragma once
#include <string>
#include <memory>
#include <mongocxx/pool.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>

namespace pulse::db {

// Initialize the driver + connection pool from Config (MONGO_URI, pool sizes,
// serverSelectionTimeout). Idempotent. Throws on a hard connection failure.
void connect();
void disconnect();

bool isConnected();
bool isHealthy();          // runs a `ping` admin command; used by /health/ready
std::string connectionStats();

// Create all indexes (ports scripts/createIndexes.js + per-model index decls).
void createIndexes();

// RAII handle: checks a client out of the pool for the duration of an op and
// returns it on destruction. Use one per logical operation, short-lived.
class ClientHandle {
public:
  ClientHandle();
  mongocxx::database  database();                          // the configured DB
  mongocxx::collection collection(const std::string& name); // db[name]
  mongocxx::client& client();
private:
  mongocxx::pool::entry entry_;
};

// Shorthand: db::collection("users") opens a client + returns the collection.
// NOTE: the returned collection borrows from a temporary client; for multi-step
// transactions hold a ClientHandle instead.
mongocxx::collection collection(const std::string& name);

// Create an index, tolerating the benign "an equivalent index already exists"
// errors (IndexOptionsConflict/IndexKeySpecsConflict/IndexAlreadyExists and the
// auto-generated-name collisions that happen when the same logical index was
// first created by the Node backend without an explicit name). Logs and
// continues rather than aborting the whole ensureIndexes() batch. `keys` and
// `opts` are BSON documents (build with bsoncxx basic builders).
void safeCreateIndex(mongocxx::collection& col,
                     bsoncxx::document::view_or_value keys,
                     bsoncxx::document::view_or_value opts = {});

} // namespace pulse::db
