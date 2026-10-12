#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace telemetry {
struct Config {
    std::uint16_t port = 9000;
    std::size_t queue_capacity = 256;
    std::string log_path = "telemetry.csv";
    unsigned processing_delay_ms = 0;
    unsigned run_for_ms = 0;
};
int run_server(const Config& config);
}
