// db.hpp — MongoDB connection layer, ports src/config/database.js.
//
// Wraps the mongocxx driver. A single mongocxx::instance lives for the whole
// process (driver requirement); a mongocxx::pool provides per-operation client
// checkout (the C++ analogue of Mongoose's connection pool). Collections are
// fetched by name from the configured database.
#pragma once
#include <string>
#include <memory>
#include <utility>
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

namespace detail {

// This owner is the first base of CollectionHandle and is therefore destroyed
// after its mongocxx::collection base. That ordering keeps the pool client alive
// while the driver collection tears down.
class CollectionLeaseHolder {
protected:
  explicit CollectionLeaseHolder(std::shared_ptr<void> lease) noexcept
      : lease_(std::move(lease)) {}
  CollectionLeaseHolder(const CollectionLeaseHolder&) = default;
  CollectionLeaseHolder(CollectionLeaseHolder&&) noexcept = default;
  CollectionLeaseHolder& operator=(const CollectionLeaseHolder&) = default;
  CollectionLeaseHolder& operator=(CollectionLeaseHolder&&) noexcept = default;
  ~CollectionLeaseHolder() = default;

private:
  std::shared_ptr<void> lease_;
};

} // namespace detail

// A collection plus ownership of the pool entry from which it came. Public
// inheritance preserves the existing `auto col = db::collection(...); col.find`
// API and permits passing a handle to helpers taking mongocxx::collection&.
class CollectionHandle final : private detail::CollectionLeaseHolder,
                               public mongocxx::collection {
public:
  CollectionHandle(const CollectionHandle&) = default;
  CollectionHandle(CollectionHandle&&) noexcept = default;

  CollectionHandle& operator=(const CollectionHandle& other) {
    if (this != &other) {
      // Dispose/copy the driver collection while the old lease is still held.
      mongocxx::collection::operator=(
          static_cast<const mongocxx::collection&>(other));
      detail::CollectionLeaseHolder::operator=(
          static_cast<const detail::CollectionLeaseHolder&>(other));
    }
    return *this;
  }

  CollectionHandle& operator=(CollectionHandle&& other) noexcept {
    if (this != &other) {
      // Dispose/move the driver collection while the old lease is still held.
      mongocxx::collection::operator=(
          static_cast<mongocxx::collection&&>(other));
      detail::CollectionLeaseHolder::operator=(
          static_cast<detail::CollectionLeaseHolder&&>(other));
    }
    return *this;
  }

  ~CollectionHandle() = default;

private:
  friend CollectionHandle collection(const std::string& name);

  CollectionHandle(std::shared_ptr<void> lease, mongocxx::collection col)
      : detail::CollectionLeaseHolder(std::move(lease)),
        mongocxx::collection(std::move(col)) {}
};

// Checks a client out for the lifetime of the returned handle. Concurrent
// handles created on one thread share a lease, so code touching several
// collections does not consume one pool client per collection. The lease is
// returned as soon as that thread's last handle is destroyed.
CollectionHandle collection(const std::string& name);

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
