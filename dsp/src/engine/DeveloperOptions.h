// Not part of the product interface: what the developer tools may change. Read by Engine::prepare().
#pragma once
#include <atomic>

namespace magmora::engine {


// What the audio thread did, for the stress benchmark: process-wide, relaxed. Counted only while a tool has switched
// `on` on (magmora-bench does); the product leaves it off, so its audio threads never write these shared counters.
struct WorkCounters {
  std::atomic<bool> on{false};
  std::atomic<long> clusterRebuilds{0};   // full rebuilds of a cluster's matrices (relays, settled knobs, reset)
  std::atomic<long> relaySwitches{0};     // relay changes applied
  std::atomic<long> settles{0};           // knob movements that ended
  std::atomic<long> restarts{0};          // returns to the operating point (power on, reset)
  std::atomic<long> recoveries{0};        // Newton solves that needed the continuation
};
WorkCounters& workCounters();

}  // namespace magmora::engine
