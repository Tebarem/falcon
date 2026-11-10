// Steam's networking (ISteamNetworkingSockets) as a transport:
//   steam://<steamid64>[:port]   peer to peer by SteamID, through Valve's relays (SDR): NAT
//                                traversal, the players' IP addresses hidden; `port` is a virtual
//                                port (default 0)
//   steamip://host:port          Steam's sockets over plain UDP (dedicated servers, LAN)
// Channels are lanes 0..3 (reliable, each ordered); datagrams go unreliable on lane 4. Steam
// encrypts and authenticates every connection.
//
// Everything runs on the game's thread: Steam's connection-state callbacks come from RunCallbacks,
// called by poll(). The functions are found by name in steam_api64.dll (engine/steam/steam_client).

#include "steam/steam_transport.h"

#include "core/log.h"

#include <SDL3/SDL_timer.h>
#include "steam/steam_client.h"

#if FALCON_STEAMWORKS

#include <steam/steam_api_flat.h>

#include <cstring>
#include <deque>
#include <unordered_map>

namespace fx::steam {

namespace {

constexpr int kLanes = net::kChannels + 1;  // the reliable channels, then datagrams
constexpr int kDatagramLane = net::kChannels;
constexpr uint32_t kMaxDatagram = 1200;

// The flat functions used, found once Steam runs.
struct Api {
    decltype(&SteamAPI_SteamNetworkingSockets_SteamAPI_v013) sockets = nullptr;
    decltype(&SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013) serverSockets = nullptr;
    decltype(&SteamAPI_SteamNetworkingUtils_SteamAPI_v004) utils = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP) listenIp = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P) listenP2P = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress) connectIp = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_ConnectP2P) connectP2P = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_AcceptConnection) accept = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CloseConnection) close = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CloseListenSocket) closeListen = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_SendMessages) sendMessages = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup) receive = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_CreatePollGroup) createPollGroup = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_DestroyPollGroup) destroyPollGroup = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup) setPollGroup = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes) lanes = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus) status = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_RunCallbacks) runCallbacks = nullptr;
    decltype(&SteamAPI_ISteamNetworkingUtils_AllocateMessage) allocate = nullptr;
    decltype(&SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess) initRelay = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_InitAuthentication) initAuthentication = nullptr;
    decltype(&SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus) authenticationStatus = nullptr;
    decltype(&SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus) relayStatus = nullptr;
    decltype(&SteamAPI_SteamNetworkingIPAddr_ParseString) parseAddress = nullptr;
    decltype(&SteamAPI_SteamNetworkingIPAddr_Clear) clearAddress = nullptr;
    decltype(&SteamAPI_SteamNetworkingIdentity_SetSteamID64) setSteamId = nullptr;
    decltype(&SteamAPI_SteamNetworkingMessage_t_Release) release = nullptr;
    ISteamNetworkingSockets* self = nullptr;
    ISteamNetworkingUtils* utilsSelf = nullptr;

    template <typename F>
    bool find(F& f, const char* name)
    {
        f = reinterpret_cast<F>(proc(name));
        return f != nullptr;
    }

    bool load()
    {
        if (self)
            return true;
        bool ok = find(sockets, "SteamAPI_SteamNetworkingSockets_SteamAPI_v013") &&
                  find(serverSockets, "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013") &&
                  find(utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004") &&
                  find(listenIp, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP") &&
                  find(listenP2P, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P") &&
                  find(connectIp, "SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress") &&
                  find(connectP2P, "SteamAPI_ISteamNetworkingSockets_ConnectP2P") &&
                  find(accept, "SteamAPI_ISteamNetworkingSockets_AcceptConnection") &&
                  find(close, "SteamAPI_ISteamNetworkingSockets_CloseConnection") &&
                  find(closeListen, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket") &&
                  find(sendMessages, "SteamAPI_ISteamNetworkingSockets_SendMessages") &&
                  find(receive, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup") &&
                  find(createPollGroup, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup") &&
                  find(destroyPollGroup, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup") &&
                  find(setPollGroup, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup") &&
                  find(lanes, "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes") &&
                  find(status, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus") &&
                  find(runCallbacks, "SteamAPI_ISteamNetworkingSockets_RunCallbacks") &&
                  find(allocate, "SteamAPI_ISteamNetworkingUtils_AllocateMessage") &&
                  find(initRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess") &&
                  find(initAuthentication, "SteamAPI_ISteamNetworkingSockets_InitAuthentication") &&
                  find(authenticationStatus, "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus") &&
                  find(relayStatus, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus") &&
                  find(parseAddress, "SteamAPI_SteamNetworkingIPAddr_ParseString") &&
                  find(clearAddress, "SteamAPI_SteamNetworkingIPAddr_Clear") &&
                  find(setSteamId, "SteamAPI_SteamNetworkingIdentity_SetSteamID64") &&
                  find(release, "SteamAPI_SteamNetworkingMessage_t_Release");
        if (!ok)
            return false;
        self = fx::steam::status().state == State::Server ? serverSockets() : sockets();  // (a game server's own)
        utilsSelf = utils();
        return self && utilsSelf;
    }
};

Api g_api;

class SteamTransport;
// Connections and listen sockets -> their transport (the status callback has only the handle).
std::unordered_map<uint32_t, SteamTransport*> g_owners;

void onStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

class SteamTransport final : public net::Transport {
public:
    explicit SteamTransport(bool ip) : ip_(ip)
    {
        pollGroup_ = g_api.createPollGroup(g_api.self);
        ready();
    }

    ~SteamTransport() override
    {
        for (auto& [id, c] : connections_) {
            g_owners.erase(id);
            g_api.close(g_api.self, id, 0, "closed", false);
        }
        if (listen_) {
            g_owners.erase(listen_);
            g_api.closeListen(g_api.self, listen_);
        }
        g_api.destroyPollGroup(g_api.self, pollGroup_);
    }

    const char* kind() const override { return ip_ ? "steamip" : "steam"; }

    bool listen(uint16_t port) override
    {
        SteamNetworkingConfigValue_t option = statusCallback();
        if (ip_) {
            SteamNetworkingIPAddr address;
            g_api.clearAddress(&address);
            address.m_port = port;
            listen_ = g_api.listenIp(g_api.self, address, 1, &option);
        } else {
            listen_ = g_api.listenP2P(g_api.self, port, 1, &option);
        }
        if (listen_ == k_HSteamListenSocket_Invalid)
            return false;
        g_owners[listen_] = this;
        port_ = port;
        return true;
    }

    uint16_t boundPort() const override { return port_; }

    net::ConnectionId connect(const std::string& address) override
    {
        SteamNetworkingConfigValue_t option = statusCallback();
        HSteamNetConnection c = k_HSteamNetConnection_Invalid;
        if (ip_) {
            SteamNetworkingIPAddr a;
            if (!g_api.parseAddress(&a, address.c_str()))
                return 0;
            c = g_api.connectIp(g_api.self, a, 1, &option);
        } else {
            // <steamid64>[:virtual port]
            const size_t colon = address.find(':');
            const uint64_t id = std::strtoull(address.substr(0, colon).c_str(), nullptr, 10);
            const int port = colon == std::string::npos ? 0 : std::atoi(address.c_str() + colon + 1);
            if (!id)
                return 0;
            SteamNetworkingIdentity identity;
            std::memset(&identity, 0, sizeof(identity));
            g_api.setSteamId(&identity, id);
            c = g_api.connectP2P(g_api.self, identity, port, 1, &option);
        }
        if (c == k_HSteamNetConnection_Invalid)
            return 0;
        g_owners[c] = this;
        connections_[c] = {};
        g_api.setPollGroup(g_api.self, c, pollGroup_);
        return c;
    }

    bool send(net::ConnectionId c, uint8_t channel, std::span<const uint8_t> data) override
    {
        return sendOn(c, channel, k_nSteamNetworkingSend_Reliable, data);
    }

    bool sendDatagram(net::ConnectionId c, std::span<const uint8_t> data) override
    {
        if (data.size() > kMaxDatagram)
            return false;
        return sendOn(c, kDatagramLane, k_nSteamNetworkingSend_UnreliableNoNagle, data);
    }

    void disconnect(net::ConnectionId c, const char* reason) override
    {
        auto it = connections_.find(c);
        if (it == connections_.end())
            return;
        g_api.close(g_api.self, c, k_ESteamNetConnectionEnd_App_Generic, reason ? reason : "closed", true);
        g_owners.erase(c);
        connections_.erase(it);
        net::Event e;
        e.type = net::Event::Type::Disconnected;
        e.connection = c;
        e.text = reason ? reason : "closed";
        events_.push_back(std::move(e));
    }

    bool poll(net::Event& e) override
    {
        if (events_.empty()) {
            g_api.runCallbacks(g_api.self);  // (status changes: onStatusChanged)
            receive();
        }
        if (events_.empty())
            return false;
        e = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    net::ConnectionStats stats(net::ConnectionId c) const override
    {
        net::ConnectionStats s;
        auto it = connections_.find(c);
        if (it == connections_.end())
            return s;
        SteamNetConnectionRealTimeStatus_t status;
        if (g_api.status(g_api.self, c, &status, 0, nullptr) == k_EResultOK) {
            s.rttMs = float(status.m_nPing);
            s.lossPercent = status.m_flConnectionQualityLocal >= 0 ? (1.0f - status.m_flConnectionQualityLocal) * 100.0f : 0.0f;
        }
        s.bytesSent = it->second.sent;
        s.bytesReceived = it->second.received;
        s.maxDatagram = it->second.connected ? kMaxDatagram : 0;
        return s;
    }

    // Steam told us a connection's state changed (from RunCallbacks, in poll()).
    void statusChanged(const SteamNetConnectionStatusChangedCallback_t& info)
    {
        const HSteamNetConnection c = info.m_hConn;
        switch (info.m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid && !connections_.count(c)) {
                // Someone connecting to our listen socket: accepted (the session decides who stays).
                if (g_api.accept(g_api.self, c) != k_EResultOK) {
                    g_api.close(g_api.self, c, 0, "not accepted", false);
                    return;
                }
                g_owners[c] = this;
                connections_[c] = {};
                g_api.setPollGroup(g_api.self, c, pollGroup_);
            }
            break;
        case k_ESteamNetworkingConnectionState_Connected: {
            auto it = connections_.find(c);
            if (it == connections_.end() || it->second.connected)
                break;
            it->second.connected = true;
            const int priorities[kLanes] = {0, 0, 0, 0, 0};
            const uint16_t weights[kLanes] = {1, 1, 1, 1, 1};
            g_api.lanes(g_api.self, c, kLanes, priorities, weights);
            net::Event e;
            e.type = net::Event::Type::Connected;
            e.connection = c;
            events_.push_back(std::move(e));
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            g_api.close(g_api.self, c, 0, nullptr, false);
            g_owners.erase(c);
            if (connections_.erase(c)) {
                net::Event e;
                e.type = net::Event::Type::Disconnected;
                e.connection = c;
                e.text = info.m_info.m_szEndDebug[0] ? info.m_info.m_szEndDebug : "closed";
                events_.push_back(std::move(e));
            }
            break;
        }
        default:
            break;
        }
    }

private:
    // Steam's sockets need this machine's certificate and Steam's network config (the CAs that
    // sign peers' certificates, the relays): asked for, then waited for (a few seconds at most,
    // once a process).
    static void ready()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        g_api.initAuthentication(g_api.self);
        g_api.initRelay(g_api.utilsSelf);
        const uint64_t until = SDL_GetTicks() + 10'000;
        ESteamNetworkingAvailability auth = k_ESteamNetworkingAvailability_Unknown;
        ESteamNetworkingAvailability config = k_ESteamNetworkingAvailability_Unknown;
        while (SDL_GetTicks() < until) {
            pump();  // (Steam only gets on with it while its callbacks are taken)
            g_api.runCallbacks(g_api.self);
            auth = g_api.authenticationStatus(g_api.self, nullptr);
            SteamRelayNetworkStatus_t relay;
            g_api.relayStatus(g_api.utilsSelf, &relay);
            config = relay.m_eAvailNetworkConfig;
            if ((auth == k_ESteamNetworkingAvailability_Current || auth < 0) &&
                (config == k_ESteamNetworkingAvailability_Current || config < 0))
                break;
            SDL_Delay(10);
        }
        if (auth != k_ESteamNetworkingAvailability_Current || config != k_ESteamNetworkingAvailability_Current)
            FX_WARN("steam: networking not ready (certificate %d, network config %d): connections may fail", int(auth), int(config));
    }

    struct Connection {
        bool connected = false;
        uint64_t sent = 0, received = 0;
    };

    static SteamNetworkingConfigValue_t statusCallback()
    {
        SteamNetworkingConfigValue_t option;
        option.m_eValue = k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged;
        option.m_eDataType = k_ESteamNetworkingConfig_Ptr;
        option.m_val.m_ptr = reinterpret_cast<void*>(&onStatusChanged);
        return option;
    }

    bool sendOn(net::ConnectionId c, int lane, int flags, std::span<const uint8_t> data)
    {
        auto it = connections_.find(c);
        if (it == connections_.end() || !it->second.connected)
            return false;
        SteamNetworkingMessage_t* m = g_api.allocate(g_api.utilsSelf, int(data.size()));
        if (!m)
            return false;
        std::memcpy(m->m_pData, data.data(), data.size());
        m->m_conn = c;
        m->m_nFlags = flags;
        m->m_idxLane = uint16_t(lane);
        int64 result = 0;
        g_api.sendMessages(g_api.self, 1, &m, &result, true);
        if (result < 0)
            return false;
        it->second.sent += data.size();
        return true;
    }

    void receive()
    {
        SteamNetworkingMessage_t* messages[64];
        for (;;) {
            const int n = g_api.receive(g_api.self, pollGroup_, messages, 64);
            for (int i = 0; i < n; ++i) {
                SteamNetworkingMessage_t* m = messages[i];
                auto it = connections_.find(m->m_conn);
                if (it != connections_.end()) {
                    it->second.received += uint64_t(m->m_cbSize);
                    net::Event e;
                    e.connection = m->m_conn;
                    if (m->m_idxLane < kDatagramLane) {
                        e.type = net::Event::Type::Message;
                        e.channel = uint8_t(m->m_idxLane);
                    } else {
                        e.type = net::Event::Type::Datagram;
                    }
                    const auto* bytes = static_cast<const uint8_t*>(m->m_pData);
                    e.data.assign(bytes, bytes + m->m_cbSize);
                    events_.push_back(std::move(e));
                }
                g_api.release(m);
            }
            if (n < 64)
                break;
        }
    }

    bool ip_;
    HSteamListenSocket listen_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;
    uint16_t port_ = 0;
    std::unordered_map<uint32_t, Connection> connections_;
    std::deque<net::Event> events_;
};

void onStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    auto it = g_owners.find(info->m_hConn);
    if (it == g_owners.end() && info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
        it = g_owners.find(info->m_info.m_hListenSocket);
    if (it != g_owners.end())
        it->second->statusChanged(*info);
    else if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting)
        g_api.close(g_api.self, info->m_hConn, 0, "no listener", false);
}

}  // namespace

std::unique_ptr<net::Transport> makeSteamTransport(bool ip)
{
    if ((status().state != State::Running && status().state != State::Server) || !g_api.load()) {
        FX_WARN("net: %s needs Steam running (--steam, or the project's App ID)", ip ? "steamip://" : "steam://");
        return nullptr;
    }
    return std::make_unique<SteamTransport>(ip);
}

}  // namespace fx::steam

#else

namespace fx::steam {

std::unique_ptr<net::Transport> makeSteamTransport(bool)
{
    FX_WARN("net: this build has no Steam (scripts/fetch-steamworks.ps1)");
    return nullptr;
}

}  // namespace fx::steam

#endif
