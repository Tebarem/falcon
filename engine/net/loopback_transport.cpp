// The in-process transport (loopback://port): a listen server's own player, tests. Each side has
// an inbox; a send puts the event straight into the other side's. Nothing is lost or reordered
// (the conditioner adds that); datagrams keep the size limit real transports have.

#include "net/net_transport.h"

#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace fx::net {

namespace {

struct Inbox;

struct Peer {
    std::weak_ptr<Inbox> other;
    ConnectionId otherId = 0;
};

struct Inbox {
    std::mutex lock;
    std::deque<Event> events;
    std::unordered_map<ConnectionId, Peer> peers;
    ConnectionId next = 1;
    uint16_t port = 0;
    uint64_t sent = 0, received = 0;
};

struct Hub {
    std::mutex lock;
    std::unordered_map<uint16_t, std::weak_ptr<Inbox>> servers;
    uint16_t nextPort = 40000;
};

Hub& hub()
{
    static Hub h;
    return h;
}

void push(Inbox& inbox, Event&& e)
{
    std::lock_guard g(inbox.lock);
    if (e.type == Event::Type::Message || e.type == Event::Type::Datagram)
        inbox.received += e.data.size();
    inbox.events.push_back(std::move(e));
}

class LoopbackTransport final : public Transport {
public:
    ~LoopbackTransport() override
    {
        std::vector<ConnectionId> ids;
        {
            std::lock_guard g(inbox_->lock);
            for (const auto& [id, peer] : inbox_->peers)
                ids.push_back(id);
        }
        for (const ConnectionId id : ids)
            disconnect(id, "closed");
        if (inbox_->port) {
            std::lock_guard g(hub().lock);
            hub().servers.erase(inbox_->port);
        }
    }

    const char* kind() const override { return "loopback"; }

    bool listen(uint16_t port) override
    {
        std::lock_guard g(hub().lock);
        if (!port)
            while (hub().servers.count(hub().nextPort))
                ++hub().nextPort;
        const uint16_t p = port ? port : hub().nextPort++;
        if (auto it = hub().servers.find(p); it != hub().servers.end() && !it->second.expired())
            return false;
        hub().servers[p] = inbox_;
        inbox_->port = p;
        return true;
    }

    uint16_t boundPort() const override { return inbox_->port; }

    ConnectionId connect(const std::string& address) override
    {
        const int port = std::atoi(address.c_str());
        std::shared_ptr<Inbox> server;
        {
            std::lock_guard g(hub().lock);
            if (auto it = hub().servers.find(uint16_t(port)); it != hub().servers.end())
                server = it->second.lock();
        }
        ConnectionId mine;
        {
            std::lock_guard g(inbox_->lock);
            mine = inbox_->next++;
        }
        if (!server || port <= 0 || port > 65535) {
            Event e;
            e.type = Event::Type::Disconnected;
            e.connection = mine;
            e.text = "no loopback server on port " + address;
            push(*inbox_, std::move(e));
            return mine;
        }
        ConnectionId theirs;
        {
            std::lock_guard g(server->lock);
            theirs = server->next++;
            server->peers[theirs] = {inbox_, mine};
        }
        {
            std::lock_guard g(inbox_->lock);
            inbox_->peers[mine] = {server, theirs};
        }
        Event toServer;
        toServer.type = Event::Type::Connected;
        toServer.connection = theirs;
        push(*server, std::move(toServer));
        Event toMe;
        toMe.type = Event::Type::Connected;
        toMe.connection = mine;
        push(*inbox_, std::move(toMe));
        return mine;
    }

    bool send(ConnectionId c, uint8_t channel, std::span<const uint8_t> data) override
    {
        if (channel >= kChannels || data.size() > kMaxMessage)
            return false;
        return deliver(c, Event::Type::Message, channel, data);
    }

    bool sendDatagram(ConnectionId c, std::span<const uint8_t> data) override
    {
        if (data.size() > kDefaultDatagram)
            return false;
        return deliver(c, Event::Type::Datagram, 0, data);
    }

    void disconnect(ConnectionId c, const char* reason) override
    {
        Peer peer;
        {
            std::lock_guard g(inbox_->lock);
            auto it = inbox_->peers.find(c);
            if (it == inbox_->peers.end())
                return;
            peer = it->second;
            inbox_->peers.erase(it);
        }
        Event mine;
        mine.type = Event::Type::Disconnected;
        mine.connection = c;
        mine.text = reason ? reason : "";
        push(*inbox_, std::move(mine));
        if (std::shared_ptr<Inbox> other = peer.other.lock()) {
            {
                std::lock_guard g(other->lock);
                if (other->peers.erase(peer.otherId) == 0)
                    return;
            }
            Event theirs;
            theirs.type = Event::Type::Disconnected;
            theirs.connection = peer.otherId;
            theirs.text = reason ? reason : "";
            push(*other, std::move(theirs));
        }
    }

    bool poll(Event& e) override
    {
        std::lock_guard g(inbox_->lock);
        if (inbox_->events.empty())
            return false;
        e = std::move(inbox_->events.front());
        inbox_->events.pop_front();
        return true;
    }

    ConnectionStats stats(ConnectionId) const override
    {
        std::lock_guard g(inbox_->lock);
        ConnectionStats s;
        s.bytesSent = inbox_->sent;
        s.bytesReceived = inbox_->received;
        s.maxDatagram = kDefaultDatagram;
        return s;
    }

private:
    bool deliver(ConnectionId c, Event::Type type, uint8_t channel, std::span<const uint8_t> data)
    {
        Peer peer;
        {
            std::lock_guard g(inbox_->lock);
            auto it = inbox_->peers.find(c);
            if (it == inbox_->peers.end())
                return false;
            peer = it->second;
            inbox_->sent += data.size();
        }
        std::shared_ptr<Inbox> other = peer.other.lock();
        if (!other)
            return false;
        Event e;
        e.type = type;
        e.connection = peer.otherId;
        e.channel = channel;
        e.data.assign(data.begin(), data.end());
        push(*other, std::move(e));
        return true;
    }

    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
};

}  // namespace

std::unique_ptr<Transport> makeLoopbackTransport() { return std::make_unique<LoopbackTransport>(); }

}  // namespace fx::net
