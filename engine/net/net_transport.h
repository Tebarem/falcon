#pragma once

// How machines talk: connections carrying reliable, ordered messages on a few channels and
// unreliable datagrams. Transports: QUIC (MsQuic: dedicated servers, LAN, the internet), an
// in-process loopback (a listen server's own player, tests) and, wrapping any of them, a
// conditioner that adds latency, jitter, loss, duplication and a bandwidth cap. Steam's
// networking is another (engine/steam). Addresses name the transport:
//   quic://host:port[#sha256]   (the server's certificate pinned by its SHA-256, hex)
//   loopback://port
//   steam://steamid64[:port], steamip://host:port   (Steam's: engine/steam/steam_transport.h)
// Everything is polled from one thread (the game's): the transports' own threads only queue
// events for poll().

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fx::net {

using ConnectionId = uint32_t;  // 0: none
inline constexpr uint8_t kChannels = 4;              // reliable channels 0..3, each ordered
inline constexpr uint32_t kMaxMessage = 64u << 20;   // a reliable message's largest size
inline constexpr uint32_t kDefaultDatagram = 1200;   // what fits a datagram on every path

struct ConnectionStats {
    float rttMs = 0.0f;
    float lossPercent = 0.0f;  // packets the transport had to send again (its estimate)
    uint64_t bytesSent = 0, bytesReceived = 0;
    uint32_t maxDatagram = 0;  // the largest datagram it can send now (0: none yet)
};

struct Event {
    enum class Type : uint8_t {
        Connected,     // a connection is up (a client's to its server, or a server's new one)
        Disconnected,  // it went (`text`: why); its id is not used again
        Message,       // a reliable message on `channel`
        Datagram,      // an unreliable one
    };
    Type type = Type::Message;
    ConnectionId connection = 0;
    uint8_t channel = 0;
    std::vector<uint8_t> data;
    std::string text;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual const char* kind() const = 0;
    // Accept connections on `port` (0: any free one; boundPort() says which).
    virtual bool listen(uint16_t port) = 0;
    virtual uint16_t boundPort() const = 0;
    // A connection to `address` (its scheme's part after "://"); its Connected or Disconnected
    // event follows. 0: the address is bad.
    virtual ConnectionId connect(const std::string& address) = 0;
    // A reliable message: delivered once, in order with the channel's others.
    virtual bool send(ConnectionId c, uint8_t channel, std::span<const uint8_t> data) = 0;
    // An unreliable datagram (at most stats().maxDatagram bytes): delivered once or not at all,
    // in any order.
    virtual bool sendDatagram(ConnectionId c, std::span<const uint8_t> data) = 0;
    virtual void disconnect(ConnectionId c, const char* reason) = 0;
    // The next event (false: none now).
    virtual bool poll(Event& e) = 0;
    virtual ConnectionStats stats(ConnectionId c) const = 0;
    // Once a frame (the conditioner lets delayed sends go here).
    virtual void update() {}
    // A listening QUIC server's certificate SHA-256 (hex; what quic://host:port#sha256 pins).
    virtual std::string certificate() const { return {}; }
};

// A transport for an address's scheme (quic://, loopback://, or a registered one); null for an
// unknown one. `rest`: the address without its scheme. A QUIC server listens on the address's
// host when it has one ("quic://127.0.0.1": this machine only), else on every address.
std::unique_ptr<Transport> makeTransport(const std::string& address, std::string* rest = nullptr);
// More schemes, made by the engine's other parts (Steam's, once it runs): null when it can't.
using TransportFactory = std::function<std::unique_ptr<Transport>()>;
void registerTransport(const std::string& scheme, TransportFactory make);

// Network conditions to simulate (the conditioner): one way, applied to what this side sends.
struct Conditions {
    float latencyMs = 0.0f, jitterMs = 0.0f;
    float lossPercent = 0.0f;       // datagrams dropped
    float duplicatePercent = 0.0f;  // datagrams sent twice
    float bandwidthKbps = 0.0f;     // 0: no cap
    uint32_t seed = 1;
    bool active() const { return latencyMs > 0 || jitterMs > 0 || lossPercent > 0 || duplicatePercent > 0 || bandwidthKbps > 0; }
};
// "latency,jitter,loss[,duplicate[,kbps]]" (--net-sim).
bool parseConditions(const char* text, Conditions& out);
std::unique_ptr<Transport> makeConditioner(std::unique_ptr<Transport> inner, const Conditions& conditions);

// QUIC: msquic.dll's path (default: net/msquic.dll next to the executable), and its connections'
// idle timeout and keep-alive (default 10 s and 2 s; 0: no keep-alive), for connections made after.
void setQuicLibrary(const std::string& path);
void setQuicTimeouts(uint32_t idleMs, uint32_t keepAliveMs);

}  // namespace fx::net
