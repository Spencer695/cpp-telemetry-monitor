#include "telemetry/protocol.hpp"
#include <charconv>
#include <cmath>
#include <system_error>

namespace telemetry {
std::optional<Reading> parse_reading(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const auto a = line.find(',');
    if (a == std::string_view::npos) return std::nullopt;
    const auto b = line.find(',', a + 1);
    if (b == std::string_view::npos || line.find(',', b + 1) != std::string_view::npos) return std::nullopt;
    const auto device = line.substr(0, a);
    if (device.empty() || device.size() > 32) return std::nullopt;
    for (const char c : device) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return std::nullopt;
    }
    const auto seq = line.substr(a + 1, b - a - 1);
    const auto val = line.substr(b + 1);
    if (seq.empty() || val.empty()) return std::nullopt;
    std::uint64_t sequence = 0;
    double value = 0;
    const auto s = std::from_chars(seq.data(), seq.data() + seq.size(), sequence);
    const auto v = std::from_chars(val.data(), val.data() + val.size(), value);
    if (s.ec != std::errc{} || s.ptr != seq.data() + seq.size() ||
        v.ec != std::errc{} || v.ptr != val.data() + val.size() ||
        !std::isfinite(value) || std::abs(value) > 1000000.0) return std::nullopt;
    return Reading{std::string(device), sequence, value, 0};
}
bool LineFramer::feed(std::string_view bytes, std::vector<std::string>& lines) {
    for (const char c : bytes) {
        if (c == '\n') { lines.push_back(std::move(pending_)); pending_.clear(); }
        else {
            if (pending_.size() == max_line) return false;
            pending_.push_back(c);
        }
    }
    return true;
}
}
