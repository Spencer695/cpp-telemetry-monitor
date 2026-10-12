#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace telemetry {
struct Reading {
    std::string device;
    std::uint64_t sequence;
    double value;
    std::int64_t received_ms = 0;
};
std::optional<Reading> parse_reading(std::string_view line);
// TCP provides bytes, not message boundaries. Retain an unfinished line per client.
class LineFramer {
public:
    static constexpr std::size_t max_line = 256;
    // False means an oversized frame: caller must close that connection.
    bool feed(std::string_view bytes, std::vector<std::string>& lines);
    bool unfinished() const { return !pending_.empty(); }
private:
    std::string pending_;
};
}
