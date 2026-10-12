#include "telemetry/server.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
unsigned number(std::string_view text, unsigned max) {
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > max)
        throw std::invalid_argument("Invalid numeric option: " + std::string(text));
    return value;
}
void help() {
    std::cout << "Usage: telemetry_monitor [--port 0..65535] [--queue-capacity 1..100000]\n"
                 "                         [--log PATH] [--processing-delay-ms 0..1000]\n"
                 "                         [--run-for-ms 0..86400000]\n"
                 "Binds to 127.0.0.1 only. Port 0 selects an available port. Ctrl+C drains and stops.\n";
}
}
int main(int argc, char** argv) {
    try {
        telemetry::Config config;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { help(); return 0; }
            if (i + 1 == argc) throw std::invalid_argument("Missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--port") config.port = static_cast<std::uint16_t>(number(value, 65535));
            else if (option == "--queue-capacity") {
                config.queue_capacity = number(value, 100000);
                if (config.queue_capacity == 0) throw std::invalid_argument("Queue capacity must be positive");
            }
            else if (option == "--processing-delay-ms") config.processing_delay_ms = number(value, 1000);
            else if (option == "--run-for-ms") config.run_for_ms = number(value, 86400000);
            else if (option == "--log") {
                if (value.empty()) throw std::invalid_argument("Log path must not be empty");
                config.log_path = value;
            }
            else throw std::invalid_argument("Unknown option: " + option);
        }
        return telemetry::run_server(config);
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
