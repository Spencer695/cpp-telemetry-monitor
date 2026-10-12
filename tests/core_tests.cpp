#include "telemetry/protocol.hpp"
#include "telemetry/queue.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        using namespace telemetry;
        const auto valid = parse_reading("sensor-1,18446744073709551615,-12.5\r");
        require(valid && valid->device == "sensor-1" && valid->sequence == UINT64_MAX && valid->value == -12.5, "Valid boundary record rejected");
        require(parse_reading("device,0,1e6").has_value(), "Valid scientific notation rejected");
        for (const auto* input : {"", "a,1", "a,1,2,3", ",1,2", "a b,1,2", "a,-1,2", "a,1x,2", "a,18446744073709551616,2", "a,1,nan", "a,1,inf", "a,1,1000001", "a,1,2x", "a,1,", "a,,1", "a, 1,2", "a,1, 2", "a,1,2\rjunk"})
            require(!parse_reading(input), "Malformed record accepted");
        require(!parse_reading(std::string(33, 'a') + ",1,2"), "Long device accepted");
        std::cout << "PASS protocol validation\n";

        LineFramer framer;
        std::vector<std::string> lines;
        require(framer.feed("a,1,", lines) && lines.empty(), "Partial frame emitted");
        require(framer.feed("2\nb,2,3\npart", lines) && lines.size() == 2 && lines[0] == "a,1,2" && lines[1] == "b,2,3" && framer.unfinished(), "Split/coalesced framing failed");
        LineFramer exact;
        lines.clear();
        require(exact.feed(std::string(256, 'x') + "\n", lines) && lines.size() == 1, "Exact frame limit rejected");
        LineFramer oversized;
        lines.clear();
        require(!oversized.feed(std::string(257, 'x'), lines), "Oversized frame accepted");
        std::cout << "PASS TCP framing and size bounds\n";

        bool zero_rejected = false;
        try { BoundedQueue<int> zero(0); } catch (const std::invalid_argument&) { zero_rejected = true; }
        require(zero_rejected, "Zero capacity accepted");
        BoundedQueue<int> bounded(2);
        require(bounded.try_push(10) && bounded.try_push(20) && !bounded.try_push(30), "Full queue policy failed");
        bounded.close();
        require(!bounded.try_push(40), "Closed queue accepted a push");
        require(bounded.pop() == 10 && bounded.pop() == 20 && !bounded.pop(), "FIFO drain/close failed");
        require(bounded.high_water() == 2, "Capacity accounting failed");
        std::cout << "PASS queue overload and drain\n";

        BoundedQueue<int> waiting(1);
        std::promise<void> started;
        auto woke = std::async(std::launch::async, [&] { started.set_value(); return waiting.pop(); });
        started.get_future().wait();
        waiting.close();
        require(woke.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !woke.get(), "Close failed to wake a waiting consumer");
        std::cout << "PASS consumer wakeup\n";

        BoundedQueue<unsigned> concurrent(17);
        constexpr unsigned producers = 4, each = 2000;
        std::vector<unsigned> occurrences(producers * each, 0);
        std::thread consumer([&] { while (auto value = concurrent.pop()) { if (*value < occurrences.size()) ++occurrences[*value]; } });
        std::vector<std::thread> workers;
        for (unsigned p = 0; p < producers; ++p) workers.emplace_back([&, p] {
            for (unsigned i = 0; i < each; ++i) {
                const unsigned value = p * each + i;
                while (!concurrent.try_push(value)) std::this_thread::yield();
            }
        });
        for (auto& worker : workers) worker.join();
        concurrent.close();
        consumer.join();
        for (const auto count : occurrences) require(count == 1, "Concurrent queue lost/duplicated a record");
        require(concurrent.high_water() <= 17, "Queue exceeded its bound");
        std::cout << "PASS 8,000 records across four producers\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
