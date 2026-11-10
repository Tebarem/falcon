#include "net/net_transport.h"

namespace fx::net {

std::unique_ptr<Transport> makeLoopbackTransport();  // loopback_transport.cpp
std::unique_ptr<Transport> makeQuicTransport(const std::string& rest);  // quic_transport.cpp

namespace {

std::vector<std::pair<std::string, TransportFactory>>& registered()
{
    static std::vector<std::pair<std::string, TransportFactory>> list;
    return list;
}

}  // namespace

void registerTransport(const std::string& scheme, TransportFactory make)
{
    for (auto& [s, f] : registered())
        if (s == scheme) {
            f = std::move(make);
            return;
        }
    registered().emplace_back(scheme, std::move(make));
}

std::unique_ptr<Transport> makeTransport(const std::string& address, std::string* rest)
{
    const size_t scheme = address.find("://");
    const std::string kind = scheme == std::string::npos ? "quic" : address.substr(0, scheme);
    const std::string after = scheme == std::string::npos ? address : address.substr(scheme + 3);
    if (rest)
        *rest = after;
    if (kind == "quic")
        return makeQuicTransport(after);
    if (kind == "loopback")
        return makeLoopbackTransport();
    for (auto& [scheme, make] : registered())
        if (scheme == kind)
            return make();
    return nullptr;
}

}  // namespace fx::net
