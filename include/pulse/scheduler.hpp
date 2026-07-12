// scheduler.hpp — background job scheduler. Ports src/jobs/scheduler.js.
#pragma once

namespace pulse::jobs {

// Start the Redis-locked job scheduler on the Drogon event loop. Idempotent.
void start();
void stop();

} // namespace pulse::jobs
