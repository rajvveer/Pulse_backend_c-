// scheduler.hpp — background job scheduler. Ports src/jobs/scheduler.js.
#pragma once

namespace pulse::jobs {

// Start the Redis-locked job scheduler on the Drogon event loop. Idempotent.
void start();
// Stop accepting new ticks without waiting for an in-flight job. Used at the
// beginning of graceful shutdown while Mongo/Redis remain available.
void requestStop();
// Request stop and join all background workers.
void stop();

} // namespace pulse::jobs
