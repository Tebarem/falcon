// Simulated network conditions around any transport (--net-sim, the editor's Play options):
// what this side sends is held for the latency (plus jitter) and the bandwidth cap's queue, and
// datagrams may be dropped or sent twice (jitter reorders them). Reliable messages are only
// delayed, in order per channel: their transport already resends what a real network loses.

#include "net/net_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>

namespace fx::net {

bool parseConditions(const char* text, Conditions& out)
{
    if (!text)
        return false;
    Conditions c;
    const int n = std::sscanf(text, "%f,%f,%f,%f,%f", &c.latencyMs, &c.jitterMs, &c.lossPercent, &c.duplicatePercent, &c.bandwidthKbps);
    if (n < 1 || c.latencyMs < 0 || c.jitterMs < 0 || c.lossPercent < 0 || c.lossPercent > 100 || c.duplicatePercent < 0 ||
        c.bandwidthKbps < 0)
        return false;
    out = c;
    return true;
}

namespace {

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

class Conditioner final : public Transport {
public:
    Conditioner(std::unique_ptr<Transport> inner, const Conditions& c) : inner_(std::move(inner)), c_(c), rng_(c.seed ? c.seed : 1) {}

    const char* kind() const override { return inner_->kind(); }
    bool listen(uint16_t port) override { return inner_->listen(port); }
    uint16_t boundPort() const override { return inner_->boundPort(); }
    std::string certificate() const override { return inner_->certificate(); }
    ConnectionId connect(const std::string& address) override { return inner_->connect(address); }

    bool send(ConnectionId c, uint8_t channel, std::span<const uint8_t> data) override
    {
        if (channel >= kChannels)
            return false;
        double& last = lastReliable_[{c, channel}];
        const double at = std::max(release(data.size()), last);  // (never before the channel's last)
        last = at;
        queue({at, c, channel, false, {data.begin(), data.end()}});
        return true;
    }

    bool sendDatagram(ConnectionId c, std::span<const uint8_t> data) override
    {
        if (chance(c_.lossPercent))
            return true;  // (lost on the way)
        const int copies = chance(c_.duplicatePercent) ? 2 : 1;
        for (int i = 0; i < copies; ++i)
            queue({release(data.size()), c, 0, true, {data.begin(), data.end()}});
        return true;
    }

    void disconnect(ConnectionId c, const char* reason) override
    {
        std::erase_if(pending_, [c](const Pending& p) { return p.connection == c; });
        inner_->disconnect(c, reason);
    }

    bool poll(Event& e) override
    {
        update();
        return inner_->poll(e);
    }

    ConnectionStats stats(ConnectionId c) const override
    {
        ConnectionStats s = inner_->stats(c);
        s.rttMs += c_.latencyMs;  // (this side's share: the other side adds its own)
        return s;
    }

    void update() override
    {
        const double now = nowMs();
        while (!pending_.empty() && pending_.front().at <= now) {
            Pending p = std::move(pending_.front());
            pending_.pop_front();
            if (p.datagram)
                inner_->sendDatagram(p.connection, p.data);
            else
                inner_->send(p.connection, p.channel, p.data);
        }
        inner_->update();
    }

private:
    struct Pending {
        double at;
        ConnectionId connection;
        uint8_t channel;
        bool datagram;
        std::vector<uint8_t> data;
    };

    float random01()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return float(rng_ >> 8) * (1.0f / 16777216.0f);
    }
    bool chance(float percent) { return percent > 0.0f && random01() * 100.0f < percent; }

    // When a send of `bytes` leaves: after the bandwidth cap's queue, the latency and some jitter.
    double release(size_t bytes)
    {
        const double now = nowMs();
        double at = now;
        if (c_.bandwidthKbps > 0.0f) {
            busyUntil_ = std::max(busyUntil_, now) + double(bytes) * 8.0 / double(c_.bandwidthKbps);
            at = busyUntil_;
        }
        return at + c_.latencyMs + c_.jitterMs * random01();
    }

    void queue(Pending&& p)
    {
        // In release order (stable for equal times: a channel's messages stay in order).
        auto it = std::upper_bound(pending_.begin(), pending_.end(), p.at, [](double at, const Pending& q) { return at < q.at; });
        pending_.insert(it, std::move(p));
    }

    std::unique_ptr<Transport> inner_;
    Conditions c_;
    uint32_t rng_;
    std::deque<Pending> pending_;
    std::map<std::pair<ConnectionId, uint8_t>, double> lastReliable_;
    double busyUntil_ = 0.0;
};

}  // namespace

std::unique_ptr<Transport> makeConditioner(std::unique_ptr<Transport> inner, const Conditions& conditions)
{
    if (!inner || !conditions.active())
        return inner;
    return std::make_unique<Conditioner>(std::move(inner), conditions);
}

}  // namespace fx::net
