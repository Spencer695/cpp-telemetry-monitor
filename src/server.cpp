#include "telemetry/server.hpp"
#include "telemetry/protocol.hpp"
#include "telemetry/queue.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace telemetry {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket invalid = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
bool would_block() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK; }
bool interrupted() { return WSAGetLastError() == WSAEINTR; }
void nonblocking(Socket s) { u_long on = 1; if (ioctlsocket(s, FIONBIO, &on) != 0) throw std::runtime_error("ioctlsocket failed"); }
struct SocketRuntime {
    SocketRuntime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data) != 0) throw std::runtime_error("WSAStartup failed"); }
    ~SocketRuntime() { WSACleanup(); }
};
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket invalid = -1;
void close_socket(Socket s) { close(s); }
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool interrupted() { return errno == EINTR; }
void nonblocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) throw std::runtime_error("fcntl failed");
}
struct SocketRuntime {};
#endif
class OwnedSocket {
public:
    explicit OwnedSocket(Socket s) : value_(s) { if (s == invalid) throw std::runtime_error("socket failed"); }
    ~OwnedSocket() { close_socket(value_); }
    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;
    Socket get() const { return value_; }
private:
    Socket value_;
};
struct Client {
    explicit Client(Socket s) : socket(s), last_activity(std::chrono::steady_clock::now()) {}
    OwnedSocket socket;
    LineFramer framer;
    std::chrono::steady_clock::time_point last_activity;
};
struct Counters {
    std::atomic<std::uint64_t> accepted{0}, rejected{0}, processed{0}, malformed{0}, dropped{0}, oversized{0}, truncated{0}, idle{0};
    std::atomic<unsigned> connected{0};
};
volatile std::sig_atomic_t signal_received = 0;
void on_signal(int value) { signal_received = value; }
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void print_status(const Counters& c, const BoundedQueue<Reading>& queue, const char* prefix) {
    std::cout << prefix << " connected=" << c.connected.load() << " accepted=" << c.accepted.load()
              << " processed=" << c.processed.load() << " malformed=" << c.malformed.load()
              << " dropped=" << c.dropped.load() << " oversized=" << c.oversized.load()
              << " truncated=" << c.truncated.load() << " rejected=" << c.rejected.load()
              << " idle=" << c.idle.load() << " queue_high_water=" << queue.high_water() << std::endl;
}
}
int run_server(const Config& config) {
    SocketRuntime runtime;
    (void)runtime;
    OwnedSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#ifndef _WIN32
    if (listener.get() >= FD_SETSIZE) throw std::runtime_error("Listener exceeds select descriptor limit");
#endif
    int reuse = 1;
    if (setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0)
        throw std::runtime_error("setsockopt failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(config.port);
    if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("Cannot bind port (already in use?)");
    if (listen(listener.get(), 16) != 0) throw std::runtime_error("listen failed");
    nonblocking(listener.get());
    AddressLength length = sizeof(address);
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
        throw std::runtime_error("getsockname failed");
    // Refuse to overwrite an existing capture so repeated demos do not erase data.
    { std::ifstream existing(config.log_path); if (existing.good()) throw std::runtime_error("Log already exists; choose a new --log path"); }
    std::ofstream log(config.log_path);
    log.exceptions(std::ios::badbit | std::ios::failbit);
    log << "received_ms,device,sequence,value\n" << std::flush;
    BoundedQueue<Reading> queue(config.queue_capacity);
    Counters counters;
    std::atomic<bool> stop{false};
    std::exception_ptr io_error, processing_error;
    signal_received = 0;
    const auto previous_int = std::signal(SIGINT, on_signal);
    const auto previous_term = std::signal(SIGTERM, on_signal);
#ifdef _WIN32
    const auto previous_break = std::signal(SIGBREAK, on_signal);
#endif

    std::thread processor([&] {
        try {
            while (auto reading = queue.pop()) {
                if (config.processing_delay_ms != 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(config.processing_delay_ms));
                log << reading->received_ms << ',' << reading->device << ',' << reading->sequence << ','
                    << std::setprecision(17) << reading->value << '\n' << std::flush;
                ++counters.processed;
            }
        } catch (...) { processing_error = std::current_exception(); stop = true; queue.close(); }
    });
    std::thread receiver;
    try {
        receiver = std::thread([&] {
            try {
                std::vector<std::unique_ptr<Client>> clients;
                while (!stop.load()) {
                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(listener.get(), &readable);
                    Socket largest = listener.get();
                    for (const auto& client : clients) {
                        FD_SET(client->socket.get(), &readable);
                        if (client->socket.get() > largest) largest = client->socket.get();
                    }
                    timeval timeout{0, 100000};
#ifdef _WIN32
                    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
                    const int ready = select(largest + 1, &readable, nullptr, nullptr, &timeout);
#endif
                    if (ready < 0) { if (interrupted()) continue; throw std::runtime_error("select failed"); }
                    if (stop.load()) break;
                    // Handle the clients present in this select snapshot before accepting new ones.
                    for (auto it = clients.begin(); it != clients.end();) {
                        auto& client = **it;
                        bool remove = false;
                        if (FD_ISSET(client.socket.get(), &readable)) {
                            char bytes[4096];
                            const auto n = recv(client.socket.get(), bytes, sizeof(bytes), 0);
                            if (n > 0) {
                                client.last_activity = std::chrono::steady_clock::now();
                                std::vector<std::string> lines;
                                const bool within_limit = client.framer.feed(std::string_view(bytes, static_cast<std::size_t>(n)), lines);
                                for (const auto& line : lines) {
                                    auto reading = parse_reading(line);
                                    if (!reading) { ++counters.malformed; continue; }
                                    reading->received_ms = now_ms();
                                    if (!queue.try_push(std::move(*reading))) ++counters.dropped;
                                }
                                if (!within_limit) { ++counters.oversized; remove = true; }
                            } else if (n == 0 || (!would_block() && !interrupted())) {
                                if (client.framer.unfinished()) ++counters.truncated;
                                remove = true;
                            }
                        }
                        if (!remove && std::chrono::steady_clock::now() - client.last_activity > std::chrono::seconds(10)) {
                            if (client.framer.unfinished()) ++counters.truncated;
                            ++counters.idle;
                            remove = true;
                        }
                        if (remove) { it = clients.erase(it); --counters.connected; }
                        else ++it;
                    }
                    if (FD_ISSET(listener.get(), &readable)) {
                        // One accept per iteration keeps an incoming connection flood from starving reads.
                        const Socket accepted = accept(listener.get(), nullptr, nullptr);
                        if (accepted != invalid) {
                            ++counters.accepted;
                            bool permitted = clients.size() < 32;
#ifndef _WIN32
                            permitted = permitted && accepted < FD_SETSIZE;
#endif
                            if (!permitted) { close_socket(accepted); ++counters.rejected; }
                            else {
                                auto client = std::make_unique<Client>(accepted);
                                nonblocking(accepted);
                                clients.push_back(std::move(client));
                                ++counters.connected;
                            }
                        } else if (!would_block() && !interrupted()) throw std::runtime_error("accept failed");
                    }
                }
                for (const auto& client : clients) if (client->framer.unfinished()) ++counters.truncated;
                clients.clear();
                counters.connected = 0;
            } catch (...) { io_error = std::current_exception(); stop = true; }
        });
    } catch (...) { queue.close(); processor.join(); throw; }
    std::cout << "LISTENING 127.0.0.1:" << ntohs(address.sin_port) << std::endl;
    auto last_status = std::chrono::steady_clock::now();
    const auto started_at = last_status;
    while (!stop.load() && signal_received == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (config.run_for_ms != 0 && std::chrono::steady_clock::now() - started_at >= std::chrono::milliseconds(config.run_for_ms)) break;
        if (std::chrono::steady_clock::now() - last_status >= std::chrono::seconds(1)) {
            print_status(counters, queue, "STATUS");
            last_status = std::chrono::steady_clock::now();
        }
    }
    stop = true;
    receiver.join();
    queue.close(); // Let processing drain the accepted queue, then wake and exit.
    processor.join();
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
#ifdef _WIN32
    std::signal(SIGBREAK, previous_break);
#endif
    print_status(counters, queue, "FINAL");
    if (io_error) std::rethrow_exception(io_error);
    if (processing_error) std::rethrow_exception(processing_error);
    return 0;
}
}
