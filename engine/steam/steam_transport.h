#pragma once

// Steam's networking as a transport (steam://steamid64[:port], peer to peer through Valve's
// relays; steamip://host:port, Steam's sockets over UDP). Registered with net::makeTransport when
// Steam starts (steam_client.cpp).

#include "net/net_transport.h"

#include <memory>

namespace fx::steam {

// Null when Steam isn't running (or this build has no Steamworks SDK).
std::unique_ptr<net::Transport> makeSteamTransport(bool ip);

}  // namespace fx::steam
