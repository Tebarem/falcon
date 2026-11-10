// falcon_net_test: the transports (engine/net) between two endpoints in one process.
//   falcon_net_test <test> [msquic.dll]
// Tests: loopback, conditioner, quic, quic_pinned, quic_loss, quic_reconnect, quic_idle.
// Prints what it checked; exit code 0 when everything held.

#include "net/net_transport.h"

#include <SDL3/SDL_log.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace fx::net;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Both ends polled until `done` holds or `ms` pass; their events kept.
struct Endpoint {
    std::unique_ptr<Transport> t;
    std::vector<Event> events;
    std::vector<double> times;  // when each event came
    size_t count(Event::Type type) const
    {
        size_t n = 0;
        for (const Event& e : events)
            n += e.type == type;
        return n;
    }
    const Event* first(Event::Type type) const
    {
        for (const Event& e : events)
            if (e.type == type)
                return &e;
        return nullptr;
    }
};

bool pump(Endpoint& a, Endpoint& b, double ms, const std::function<bool()>& done)
{
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        for (Endpoint* end : {&a, &b}) {
            end->t->update();
            Event e;
            while (end->t->poll(e)) {
                end->events.push_back(std::move(e));
                end->times.push_back(nowMs());
            }
        }
        if (done && done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done ? done() : true;
}

std::vector<uint8_t> payload(uint32_t index, size_t size)
{
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i)
        v[i] = uint8_t((index * 31 + i * 7) & 0xff);
    if (size >= 4)
        std::memcpy(v.data(), &index, 4);
    return v;
}

uint32_t indexOf(const Event& e)
{
    uint32_t i = ~0u;
    if (e.data.size() >= 4)
        std::memcpy(&i, e.data.data(), 4);
    return i;
}

// A server and a client connected (false: they did not within 5 s).
bool connectPair(Endpoint& server, Endpoint& client, const std::string& scheme, ConnectionId& atClient, const std::string& pin = "")
{
    if (!server.t || !client.t || !server.t->listen(0))
        return false;
    std::string address = (scheme == "quic" ? "127.0.0.1:" : "") + std::to_string(server.t->boundPort());
    if (!pin.empty())
        address += "#" + pin;
    atClient = client.t->connect(address);
    return pump(server, client, 5000, [&] {
        return (server.count(Event::Type::Connected) && client.count(Event::Type::Connected)) || client.count(Event::Type::Disconnected);
    }) && client.count(Event::Type::Connected);
}

// Reliable messages on every channel, interleaved, and datagrams: all reliable ones arrive in
// order per channel and intact; returns the datagrams that arrived.
size_t exchange(Endpoint& server, Endpoint& client, ConnectionId c, uint32_t messages, size_t bigBytes, uint32_t datagrams,
                size_t datagramBytes, double ms, const char* label)
{
    const size_t before = server.events.size();
    if (bigBytes)
        client.t->send(c, kChannels - 1, payload(0xB16, bigBytes));
    uint32_t sent = 0;
    for (uint32_t i = 0; i < messages; ++i)
        sent += client.t->send(c, uint8_t(i % (kChannels - 1)), payload(i, 16 + i % 300));
    uint32_t sentDatagrams = 0;
    for (uint32_t i = 0; i < datagrams; ++i) {
        sentDatagrams += client.t->sendDatagram(c, payload(i, datagramBytes));
        if ((i & 63) == 0)
            pump(server, client, 1, nullptr);  // (not all at once: a real game sends per tick)
    }
    const auto received = [&](Event::Type type) {
        size_t n = 0;
        for (size_t i = before; i < server.events.size(); ++i)
            n += server.events[i].type == type;
        return n;
    };
    pump(server, client, ms, [&] { return received(Event::Type::Message) >= messages + (bigBytes ? 1 : 0); });
    pump(server, client, 300, nullptr);  // (late datagrams)
    bool ordered = true, intact = true, big = !bigBytes;
    uint32_t next[kChannels] = {};
    for (size_t i = before; i < server.events.size(); ++i) {
        const Event& e = server.events[i];
        if (e.type != Event::Type::Message)
            continue;
        if (e.channel == kChannels - 1) {
            big = e.data == payload(0xB16, bigBytes);
            continue;
        }
        const uint32_t index = indexOf(e);
        if (index % (kChannels - 1) != e.channel || index < next[e.channel])
            ordered = false;
        next[e.channel] = index + 1;
        intact &= e.data == payload(index, 16 + index % 300);
    }
    char line[160];
    std::snprintf(line, sizeof(line), "%s: %u/%u reliable messages arrived", label, unsigned(received(Event::Type::Message) - (bigBytes ? 1 : 0)),
                  unsigned(messages));
    check(sent == messages && received(Event::Type::Message) == messages + (bigBytes ? 1 : 0), line);
    std::snprintf(line, sizeof(line), "%s: in order per channel, intact", label);
    check(ordered && intact, line);
    if (bigBytes) {
        std::snprintf(line, sizeof(line), "%s: a %zu MB message arrived intact", label, bigBytes >> 20);
        check(big, line);
    }
    std::printf("     %s: %zu of %u datagrams arrived (%u sent)\n", label, received(Event::Type::Datagram), datagrams, sentDatagrams);
    return received(Event::Type::Datagram);
}

int loopback()
{
    Endpoint server{makeTransport("loopback://")}, client{makeTransport("loopback://")};
    ConnectionId c = 0;
    check(connectPair(server, client, "loopback", c), "loopback: connected");
    const size_t got = exchange(server, client, c, 600, 4u << 20, 200, 1000, 2000, "loopback");
    check(got == 200, "loopback: every datagram arrived");
    check(!client.t->sendDatagram(c, std::vector<uint8_t>(kDefaultDatagram + 1)), "loopback: an oversized datagram is refused");
    client.t->disconnect(c, "bye");
    pump(server, client, 1000, [&] { return server.count(Event::Type::Disconnected) && client.count(Event::Type::Disconnected); });
    const Event* gone = server.first(Event::Type::Disconnected);
    check(gone && gone->text == "bye", "loopback: the server heard the disconnect and why");
    Endpoint nobody{makeTransport("loopback://")};
    nobody.t->connect("1");
    Endpoint none{makeTransport("loopback://")};
    pump(nobody, none, 200, [&] { return nobody.count(Event::Type::Disconnected) > 0; });
    check(nobody.count(Event::Type::Disconnected) == 1, "loopback: connecting to no server fails");
    return 0;
}

int conditioner()
{
    Conditions conditions;
    check(parseConditions("40,10,10,5", conditions) && conditions.latencyMs == 40 && conditions.lossPercent == 10,
          "conditioner: --net-sim text parsed");
    Endpoint server{makeTransport("loopback://")}, client{makeConditioner(makeTransport("loopback://"), conditions)};
    ConnectionId c = 0;
    check(connectPair(server, client, "loopback", c), "conditioner: connected");
    const double sentAt = nowMs();
    const size_t got = exchange(server, client, c, 400, 0, 2000, 200, 3000, "conditioner");
    // 10% lost, 5% of the rest twice: about 1890 of 2000.
    check(got > 1700 && got < 2050, "conditioner: about 10% of the datagrams lost (5% doubled)");
    double firstAt = 0;
    for (size_t i = 0; i < server.events.size(); ++i)
        if (server.events[i].type == Event::Type::Message) {
            firstAt = server.times[i];
            break;
        }
    check(firstAt - sentAt >= 38.0, "conditioner: messages arrive after the latency");
    return 0;
}

std::string g_pin;

int quicBasic()
{
    Endpoint server{makeTransport("quic://127.0.0.1")}, client{makeTransport("quic://")};
    ConnectionId c = 0;
    check(connectPair(server, client, "quic", c), "quic: connected on localhost");
    check(server.t->certificate().size() == 64, "quic: the server made its certificate (sha256)");
    const ConnectionStats s = client.t->stats(c);
    std::printf("     quic: max datagram %u bytes, rtt %.2f ms\n", s.maxDatagram, s.rttMs);
    check(s.maxDatagram >= 1000, "quic: datagrams are on");
    const size_t got = exchange(server, client, c, 2000, 10u << 20, 500, s.maxDatagram, 10000, "quic");
    check(got >= 490, "quic: datagrams arrived (on localhost, nearly all)");
    // The server answers on its own streams.
    const ConnectionId back = server.first(Event::Type::Connected)->connection;
    server.t->send(back, 0, payload(7, 64));
    pump(server, client, 2000, [&] { return client.count(Event::Type::Message) > 0; });
    check(client.count(Event::Type::Message) == 1, "quic: the server's message reached the client");
    client.t->disconnect(c, "bye");
    pump(server, client, 3000, [&] { return server.count(Event::Type::Disconnected) && client.count(Event::Type::Disconnected); });
    check(server.count(Event::Type::Disconnected) == 1, "quic: the server saw the client go");
    return 0;
}

int quicPinned()
{
    Endpoint server{makeTransport("quic://127.0.0.1")}, client{makeTransport("quic://")};
    ConnectionId c = 0;
    server.t->listen(0);
    const std::string pin = server.t->certificate();
    c = client.t->connect("127.0.0.1:" + std::to_string(server.t->boundPort()) + "#" + pin);
    pump(server, client, 5000, [&] { return client.count(Event::Type::Connected) || client.count(Event::Type::Disconnected); });
    check(client.count(Event::Type::Connected) == 1, "quic: the pinned certificate is accepted");
    Endpoint wrong{makeTransport("quic://")};
    wrong.t->connect("127.0.0.1:" + std::to_string(server.t->boundPort()) + "#" + std::string(64, 'a'));
    pump(server, wrong, 5000, [&] { return wrong.count(Event::Type::Connected) || wrong.count(Event::Type::Disconnected); });
    const Event* refused = wrong.first(Event::Type::Disconnected);
    check(!wrong.count(Event::Type::Connected) && refused && refused->text.find("pinned") != std::string::npos,
          "quic: another certificate than the pinned one is refused");
    (void)c;
    return 0;
}

int quicLoss()
{
    Conditions conditions;
    parseConditions("20,5,10", conditions);
    Endpoint server{makeTransport("quic://127.0.0.1")}, client{makeConditioner(makeTransport("quic://"), conditions)};
    ConnectionId c = 0;
    check(connectPair(server, client, "quic", c), "quic+loss: connected");
    const size_t got = exchange(server, client, c, 1000, 0, 1000, 800, 10000, "quic+loss");
    check(got > 820 && got < 960, "quic+loss: about 10% of the datagrams lost, reliable messages all there");
    return 0;
}

int quicReconnect()
{
    Endpoint server{makeTransport("quic://127.0.0.1")}, client{makeTransport("quic://")};
    ConnectionId c = 0;
    check(connectPair(server, client, "quic", c), "quic reconnect: connected");
    client.t->disconnect(c, "again");
    pump(server, client, 3000, [&] { return client.count(Event::Type::Disconnected) > 0; });
    const ConnectionId again = client.t->connect("127.0.0.1:" + std::to_string(server.t->boundPort()));
    pump(server, client, 5000, [&] { return client.count(Event::Type::Connected) >= 2; });
    check(client.count(Event::Type::Connected) == 2 && again != c, "quic reconnect: connected again (a new connection)");
    client.t->send(again, 1, payload(1, 32));
    pump(server, client, 2000, [&] { return server.count(Event::Type::Message) > 0; });
    check(server.count(Event::Type::Message) == 1, "quic reconnect: the new connection carries messages");
    return 0;
}

int quicIdle()
{
    setQuicTimeouts(800, 0);  // (no keep-alive: nothing goes either way)
    Endpoint server{makeTransport("quic://127.0.0.1")}, client{makeTransport("quic://")};
    ConnectionId c = 0;
    check(connectPair(server, client, "quic", c), "quic idle: connected");
    pump(server, client, 4000, [&] { return client.count(Event::Type::Disconnected) > 0; });
    const Event* gone = client.first(Event::Type::Disconnected);
    check(gone && gone->text.find("idle") != std::string::npos, "quic idle: a silent connection times out");
    setQuicTimeouts(800, 200);
    Endpoint server2{makeTransport("quic://127.0.0.1")}, client2{makeTransport("quic://")};
    check(connectPair(server2, client2, "quic", c), "quic keep-alive: connected");
    pump(server2, client2, 2500, [&] { return client2.count(Event::Type::Disconnected) > 0; });
    check(client2.count(Event::Type::Disconnected) == 0, "quic keep-alive: kept alive past the idle timeout");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
    if (argc < 2) {
        std::printf("usage: falcon_net_test loopback|conditioner|quic|quic_pinned|quic_loss|quic_reconnect|quic_idle [msquic.dll]\n");
        return 2;
    }
    if (argc > 2)
        setQuicLibrary(argv[2]);
    const std::string test = argv[1];
    if (test == "loopback")
        loopback();
    else if (test == "conditioner")
        conditioner();
    else if (test == "quic")
        quicBasic();
    else if (test == "quic_pinned")
        quicPinned();
    else if (test == "quic_loss")
        quicLoss();
    else if (test == "quic_reconnect")
        quicReconnect();
    else if (test == "quic_idle")
        quicIdle();
    else {
        std::printf("no test %s\n", test.c_str());
        return 2;
    }
    std::printf("%s: %s\n", test.c_str(), g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
