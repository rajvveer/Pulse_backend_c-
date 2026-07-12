// indexes.cc — central index aggregator. Ports scripts/createIndexes.js.
//
// The JS script connected to Mongo and created a hand-picked set of named
// indexes on a handful of collections (posts, users, likes, notifications,
// comments, messages/conversations, follows, reels). In the C++ port each
// Mongoose model owns its full index spec via pulse::models::<name>::
// ensureIndexes(); those per-model functions create EVERY index the schema
// declares — a superset of what the script created — so this aggregator simply
// invokes them all. Each call is wrapped in its own try/catch so that a failure
// to build one collection's indexes (e.g. a transient duplicate-key while a
// unique index back-fills) is logged and the remaining models still run, just
// as the script tolerated a missing collection per-collection.
#include "pulse/models.hpp"
#include "pulse/logger.hpp"

#include <exception>
#include <functional>
#include <utility>

namespace pulse::db {

namespace {

// Invoke one model's ensureIndexes(), logging and swallowing any failure so the
// remaining models still get a chance to build their indexes.
void runOne(const char* model, const std::function<void()>& fn) {
  try {
    fn();
    pulse::log::info("\xE2\x9C\x85 Indexes ensured for {}", model);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Index creation failed for {}: {}", model, e.what());
  } catch (...) {
    pulse::log::error("\xE2\x9D\x8C Index creation failed for {}: unknown error", model);
  }
}

} // namespace

// Create every model's indexes. Mirrors scripts/createIndexes.js: best-effort,
// continue-on-failure, one collection at a time. Idempotent — mongocxx
// create_index is a no-op when the index already exists with the same spec.
void ensureAllIndexes() {
  pulse::log::info("\xF0\x9F\x93\x8A Ensuring all database indexes...");

  runOne("user",           &pulse::models::user::ensureIndexes);
  runOne("otp",            &pulse::models::otp::ensureIndexes);
  runOne("session",        &pulse::models::session::ensureIndexes);
  runOne("post",           &pulse::models::post::ensureIndexes);
  runOne("comment",        &pulse::models::comment::ensureIndexes);
  runOne("like",           &pulse::models::like::ensureIndexes);
  runOne("follow",         &pulse::models::follow::ensureIndexes);
  runOne("notification",   &pulse::models::notification::ensureIndexes);
  runOne("conversation",   &pulse::models::conversation::ensureIndexes);
  runOne("message",        &pulse::models::message::ensureIndexes);
  runOne("reel",           &pulse::models::reel::ensureIndexes);
  runOne("reelcomment",    &pulse::models::reelcomment::ensureIndexes);
  runOne("snap",           &pulse::models::snap::ensureIndexes);
  runOne("bookmark",       &pulse::models::bookmark::ensureIndexes);
  runOne("whisper",        &pulse::models::whisper::ensureIndexes);
  runOne("pulsedrop",      &pulse::models::pulsedrop::ensureIndexes);
  runOne("chainstory",     &pulse::models::chainstory::ensureIndexes);
  runOne("alterego",       &pulse::models::alterego::ensureIndexes);
  runOne("socialdna",      &pulse::models::socialdna::ensureIndexes);
  runOne("pulsescore",     &pulse::models::pulsescore::ensureIndexes);
  runOne("roulette",       &pulse::models::roulette::ensureIndexes);
  runOne("userbehavior",   &pulse::models::userbehavior::ensureIndexes);
  runOne("userengagement", &pulse::models::userengagement::ensureIndexes);

  pulse::log::info("\xF0\x9F\x8E\x89 Index aggregation complete");
}

} // namespace pulse::db
