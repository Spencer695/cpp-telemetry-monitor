#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>
#include <functional>
#include "telemetry/protocol.hpp"

namespace telemetry {
struct Config {
    std::uint16_t port = 9000;
    std::size_t queue_capacity = 256;
    std::string log_path = "telemetry.csv";
    unsigned processing_delay_ms = 0;
    unsigned run_for_ms = 0;
};
struct Status {
    std::uint64_t accepted = 0, rejected = 0, processed = 0, malformed = 0;
    std::uint64_t dropped = 0, oversized = 0, truncated = 0, idle = 0;
    unsigned connected = 0;
    std::size_t queue_high_water = 0;
};
// A dashboard can request a drain without relying on platform console signals.
// Callbacks execute on server/processing threads and must not access GUI widgets.
struct ServerObserver {
    std::atomic<bool> stop_requested{false};
    std::function<void(std::uint16_t)> listening;
    std::function<void(const Reading&)> reading;
    std::function<void(const Status&)> status;
};
int run_server(const Config& config, ServerObserver* observer = nullptr);
}
