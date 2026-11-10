// QUIC through Microsoft's MsQuic (its OpenSSL build; scripts/fetch-msquic.ps1), loaded at run time
// from net/msquic.dll next to the executable: a build or machine without it still runs (no quic://).
//
// - Reliable messages: one unidirectional stream per channel and direction, opened on the first
//   send. A stream starts with its channel's byte; then each message is its length (u32) and bytes.
// - Unreliable messages: QUIC datagrams (RFC 9221).
// - ALPN "falcon/1". Keep-alive every 2 s, idle timeout 10 s (setQuicTimeouts).
// - Threads: MsQuic's API is called on the game's thread only (sends, shutdowns, closing handles);
//   its callbacks (its worker threads) only set flags and queue events. A connection's handle is
//   closed when the game polls its Disconnected event. (Calling MsQuic while holding a lock its
//   callbacks also take deadlocked a server when 200 connections closed at once.)
// - The server makes itself a self-signed certificate at start: a fresh ECDSA P-256 key made with
//   BCrypt alone (no key or certificate store, never any UI). Its SHA-256 is logged and can be
//   pinned by clients (quic://host:port#sha256). Unpinned clients rely on the session's own authentication (a token, a Steam ticket).

#include "net/net_transport.h"

#if FALCON_QUIC

#define NOMINMAX
#include <msquic.h>  // (first: it includes windows.h and winsock2.h in the order they need)

#include <bcrypt.h>

#include "core/log.h"
#include "core/platform.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fx::net {

namespace {

std::mutex g_quicLock;
std::string g_libraryPath;
uint32_t g_idleMs = 10000, g_keepAliveMs = 2000;
void* g_dll = nullptr;
const QUIC_API_TABLE* g_api = nullptr;
HQUIC g_registration = nullptr;

const QUIC_BUFFER kAlpn = {sizeof("falcon/1") - 1, (uint8_t*)"falcon/1"};

// MsQuic's function table and the process's registration (opened once, kept until exit).
const QUIC_API_TABLE* quic()
{
    std::lock_guard g(g_quicLock);
    if (g_api)
        return g_api;
    const std::filesystem::path path = g_libraryPath.empty()
                                           ? platform::executableDir() / "net" / platform::libraryFileName("msquic")
                                           : std::filesystem::path(g_libraryPath);
    g_dll = platform::loadLibrary(path);
    if (!g_dll) {
        FX_ERROR("quic: no MsQuic at %s (%s; scripts/fetch-msquic.ps1)", platform::utf8(path).c_str(), platform::lastError().c_str());
        return nullptr;
    }
    auto open = reinterpret_cast<MsQuicOpenVersionFn>(platform::librarySymbol(g_dll, "MsQuicOpenVersion"));
    const QUIC_API_TABLE* api = nullptr;
    if (!open || QUIC_FAILED(open(QUIC_API_VERSION_2, reinterpret_cast<const void**>(&api)))) {
        FX_ERROR("quic: msquic.dll would not open (API version 2)");
        return nullptr;
    }
    const QUIC_REGISTRATION_CONFIG config = {"falcon", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(api->RegistrationOpen(&config, &g_registration))) {
        FX_ERROR("quic: registration failed");
        return nullptr;
    }
    g_api = api;
    return g_api;
}

std::string hex(const uint8_t* bytes, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i)
        s[2 * i] = digits[bytes[i] >> 4], s[2 * i + 1] = digits[bytes[i] & 15];
    return s;
}

std::string sha256Hex(const uint8_t* data, size_t n)
{
    uint8_t digest[32] = {};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), ULONG(n), digest, 32)))
        return {};
    return hex(digest, 32);
}

// PEM: base64 lines of 64 between the label's lines.
std::string pem(const char* label, const std::vector<uint8_t>& der)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i < der.size(); i += 3) {
        const uint32_t v = uint32_t(der[i]) << 16 | (i + 1 < der.size() ? uint32_t(der[i + 1]) << 8 : 0u) |
                           (i + 2 < der.size() ? uint32_t(der[i + 2]) : 0u);
        b64 += table[(v >> 18) & 63];
        b64 += table[(v >> 12) & 63];
        b64 += i + 1 < der.size() ? table[(v >> 6) & 63] : '=';
        b64 += i + 2 < der.size() ? table[v & 63] : '=';
    }
    std::string out = std::string("-----BEGIN ") + label + "-----\n";
    for (size_t i = 0; i < b64.size(); i += 64)
        out += b64.substr(i, 64) + "\n";
    return out + "-----END " + label + "-----\n";
}

// DER (ASN.1): a tag and its contents.
std::vector<uint8_t> der(uint8_t tag, const std::vector<uint8_t>& content)
{
    std::vector<uint8_t> out = {tag};
    const size_t n = content.size();
    if (n < 0x80) {
        out.push_back(uint8_t(n));
    } else if (n < 0x100) {
        out.push_back(0x81);
        out.push_back(uint8_t(n));
    } else {
        out.push_back(0x82);
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(n));
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

// A positive INTEGER from big-endian bytes (leading zeros dropped, a zero added before a high bit).
std::vector<uint8_t> derInteger(const uint8_t* bytes, size_t n)
{
    size_t start = 0;
    while (start + 1 < n && bytes[start] == 0)
        ++start;
    std::vector<uint8_t> v(bytes + start, bytes + n);
    if (v.empty() || (v[0] & 0x80))
        v.insert(v.begin(), 0);
    return der(0x02, v);
}

// UTCTime YYMMDDHHMMSSZ, `days` from now.
std::vector<uint8_t> derTime(int days)
{
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    ULARGE_INTEGER u = {{t.dwLowDateTime, t.dwHighDateTime}};
    u.QuadPart += uint64_t(int64_t(days) * 24 * 3600) * 10000000ull;
    const FILETIME f = {u.LowPart, u.HighPart};
    SYSTEMTIME s;
    FileTimeToSystemTime(&f, &s);
    char text[16];
    std::snprintf(text, sizeof(text), "%02u%02u%02u%02u%02u%02uZ", unsigned(s.wYear % 100), unsigned(s.wMonth), unsigned(s.wDay),
                  unsigned(s.wHour), unsigned(s.wMinute), unsigned(s.wSecond));
    return der(0x17, std::vector<uint8_t>(text, text + 13));
}

// The server's certificate: self-signed, a fresh ECDSA P-256 key, valid from a day ago for a year.
// Made with BCrypt alone (the primitives: no key store, no certificate store, never any UI), the
// X.509 structure written here. PEM for MsQuic, and its SHA-256 (what clients pin).
struct ServerCertificate {
    std::string certificate, key, sha256;
};

bool makeCertificate(ServerCertificate& out)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    do {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
            break;
        if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(algorithm, &key, 256, 0)) || !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0)))
            break;
        // The private blob: BCRYPT_ECCKEY_BLOB, then X, Y and d (32 bytes each).
        ULONG size = 0;
        if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &size, 0)))
            break;
        std::vector<uint8_t> blob(size);
        if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), size, &size, 0)) ||
            size != sizeof(BCRYPT_ECCKEY_BLOB) + 96)
            break;
        const uint8_t* xy = blob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
        const uint8_t* d = xy + 64;
        std::vector<uint8_t> point = {0x00, 0x04};  // (a BIT STRING's unused bits, then an uncompressed point)
        point.insert(point.end(), xy, xy + 64);

        const std::vector<uint8_t> ecdsaSha256 = {0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};
        const std::vector<uint8_t> prime256v1 = {0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
        const std::vector<uint8_t> ecPublicKey = {0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
        const std::string cn = "Falcon Server";
        const std::vector<uint8_t> name =
            der(0x30, der(0x31, der(0x30, cat({{0x06, 0x03, 0x55, 0x04, 0x03}, der(0x0c, std::vector<uint8_t>(cn.begin(), cn.end()))}))));
        uint8_t serial[16];
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            break;
        serial[0] &= 0x7f;
        serial[0] |= 0x01;
        const std::vector<uint8_t> tbs = der(0x30, cat({
                                                       {0xa0, 0x03, 0x02, 0x01, 0x02},  // v3
                                                       derInteger(serial, sizeof(serial)),
                                                       ecdsaSha256,
                                                       name,
                                                       der(0x30, cat({derTime(-1), derTime(365)})),
                                                       name,
                                                       der(0x30, cat({der(0x30, cat({ecPublicKey, prime256v1})), der(0x03, point)})),
                                                   }));
        uint8_t digest[32];
        if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(tbs.data()), ULONG(tbs.size()), digest, 32)))
            break;
        uint8_t rs[64];
        ULONG signed_ = 0;
        if (!BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, digest, 32, rs, sizeof(rs), &signed_, 0)) || signed_ != 64)
            break;
        std::vector<uint8_t> signature = {0x00};  // (BIT STRING: no unused bits) ECDSA-Sig-Value
        const std::vector<uint8_t> sig = der(0x30, cat({derInteger(rs, 32), derInteger(rs + 32, 32)}));
        signature.insert(signature.end(), sig.begin(), sig.end());
        const std::vector<uint8_t> certificate = der(0x30, cat({tbs, ecdsaSha256, der(0x03, signature)}));
        // SEC 1 ECPrivateKey: version 1, d, the curve, the public point.
        std::vector<uint8_t> privateKey =
            der(0x30, cat({{0x02, 0x01, 0x01}, der(0x04, std::vector<uint8_t>(d, d + 32)), der(0xa0, prime256v1), der(0xa1, der(0x03, point))}));
        out.certificate = pem("CERTIFICATE", certificate);
        out.key = pem("EC PRIVATE KEY", privateKey);
        out.sha256 = sha256Hex(certificate.data(), certificate.size());
        SecureZeroMemory(blob.data(), blob.size());
        SecureZeroMemory(privateKey.data(), privateKey.size());
        ok = !out.sha256.empty();
    } while (false);
    if (key)
        BCryptDestroyKey(key);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

QUIC_SETTINGS settings()
{
    QUIC_SETTINGS s = {};
    s.IdleTimeoutMs = g_idleMs;
    s.IsSet.IdleTimeoutMs = TRUE;
    s.HandshakeIdleTimeoutMs = 5000;
    s.IsSet.HandshakeIdleTimeoutMs = TRUE;
    if (g_keepAliveMs) {
        s.KeepAliveIntervalMs = g_keepAliveMs;
        s.IsSet.KeepAliveIntervalMs = TRUE;
    }
    s.DatagramReceiveEnabled = TRUE;
    s.IsSet.DatagramReceiveEnabled = TRUE;
    s.PeerUnidiStreamCount = kChannels;
    s.IsSet.PeerUnidiStreamCount = TRUE;
    return s;
}

// A send in flight: MsQuic reads the bytes until it says it is done with them.
struct SendBuffer {
    QUIC_BUFFER buffer;
    static SendBuffer* make(size_t size)
    {
        auto* b = static_cast<SendBuffer*>(std::malloc(sizeof(SendBuffer) + size));
        b->buffer.Length = uint32_t(size);
        b->buffer.Buffer = reinterpret_cast<uint8_t*>(b + 1);
        return b;
    }
};

class QuicTransport;

struct Connection {
    QuicTransport* owner = nullptr;
    ConnectionId id = 0;
    HQUIC handle = nullptr;
    std::mutex lock;                     // `reason` (written by callbacks, read at the end)
    std::atomic<bool> shutDown{false};   // its shutdown completed (the handle closes at the next poll)
    std::atomic<bool> connected{false};
    HQUIC send[kChannels] = {};          // our streams, opened on the first message (the game's thread)
    std::string pin;     // a client's: the server certificate's SHA-256 it must have
    std::string reason;  // why we closed it
    std::atomic<uint32_t> maxDatagram{0};
};

// A peer's stream coming in: its channel (its first byte), then messages.
struct Incoming {
    Connection* connection = nullptr;
    int channel = -1;
    std::vector<uint8_t> bytes;
    size_t read = 0;
};

class QuicTransport final : public Transport {
public:
    // `listenHost`: the address a server listens on (an IP, e.g. 127.0.0.1: only this machine's
    // clients, no firewall prompt); empty: every one.
    explicit QuicTransport(std::string listenHost) : api_(quic()), listenHost_(std::move(listenHost)) {}

    ~QuicTransport() override
    {
        if (!api_)
            return;
        if (listener_) {
            api_->ListenerClose(listener_);  // (waits until it stopped)
            listener_ = nullptr;
        }
        std::unordered_map<ConnectionId, std::shared_ptr<Connection>> all;
        {
            std::lock_guard g(lock_);
            all.swap(connections_);
        }
        for (auto& [id, c] : all)
            if (!c->shutDown)
                api_->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        // Their shutdowns complete (silent: at once); then their handles go.
        for (int i = 0; i < 400; ++i) {
            bool done = true;
            for (auto& [id, c] : all)
                done = done && c->shutDown;
            if (done)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (auto& [id, c] : all)
            closeHandles(*c);
        if (serverConfig_)
            api_->ConfigurationClose(serverConfig_);
        if (clientConfig_)
            api_->ConfigurationClose(clientConfig_);
    }

    const char* kind() const override { return "quic"; }

    bool listen(uint16_t port) override
    {
        if (!api_ || listener_)
            return false;
        ServerCertificate cert;
        if (!makeCertificate(cert)) {
            FX_ERROR("quic: no server certificate (BCrypt)");
            return false;
        }
        if (!openServerConfig(cert))
            return false;
        if (QUIC_FAILED(api_->ListenerOpen(g_registration, &QuicTransport::onListener, this, &listener_)))
            return false;
        QUIC_ADDR address = {};
        if (listenHost_.empty()) {
            QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
            QuicAddrSetPort(&address, port);
        } else if (!QuicAddrFromString(listenHost_ == "localhost" ? "127.0.0.1" : listenHost_.c_str(), port, &address)) {
            FX_ERROR("quic: cannot listen on %s: not an IP address", listenHost_.c_str());
            api_->ListenerClose(listener_);
            listener_ = nullptr;
            return false;
        }
        if (QUIC_FAILED(api_->ListenerStart(listener_, &kAlpn, 1, &address))) {
            FX_ERROR("quic: cannot listen on %s port %u", listenHost_.empty() ? "every address," : listenHost_.c_str(), unsigned(port));
            api_->ListenerClose(listener_);
            listener_ = nullptr;
            return false;
        }
        QUIC_ADDR bound = {};
        uint32_t size = sizeof(bound);
        if (QUIC_SUCCEEDED(api_->GetParam(listener_, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &size, &bound)))
            port_ = QuicAddrGetPort(&bound);
        certificate_ = cert.sha256;
        FX_INFO("quic: listening on %s port %u, certificate sha256 %s", listenHost_.empty() ? "every address," : listenHost_.c_str(),
                unsigned(port_), cert.sha256.c_str());
        return true;
    }

    uint16_t boundPort() const override { return port_; }
    std::string certificate() const override { return certificate_; }

    ConnectionId connect(const std::string& address) override
    {
        auto c = std::make_shared<Connection>();
        c->owner = this;
        {
            std::lock_guard g(lock_);
            c->id = next_++;
        }
        std::string host = address;
        if (const size_t hash = host.find('#'); hash != std::string::npos) {
            c->pin = host.substr(hash + 1);
            for (char& ch : c->pin)
                ch = char(std::tolower(uint8_t(ch)));
            host.resize(hash);
        }
        // host:port, or [IPv6]:port
        int port = 0;
        size_t colon = std::string::npos;
        if (!host.empty() && host.front() == '[') {
            const size_t close = host.find(']');
            if (close != std::string::npos && close + 1 < host.size() && host[close + 1] == ':')
                colon = close + 1;
        } else {
            colon = host.rfind(':');
        }
        if (colon != std::string::npos) {
            port = std::atoi(host.c_str() + colon + 1);
            host.resize(colon);
        }
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
            host = host.substr(1, host.size() - 2);
        if (!api_ || host.empty() || port <= 0 || port > 65535 || !openClientConfig()) {
            fail(c->id, api_ ? "bad address " + address : "QUIC is not available (msquic.dll)");
            return c->id;
        }
        {
            std::lock_guard g(lock_);
            connections_[c->id] = c;
        }
        if (QUIC_FAILED(api_->ConnectionOpen(g_registration, &QuicTransport::onConnection, c.get(), &c->handle)) ||
            QUIC_FAILED(api_->ConnectionStart(c->handle, clientConfig_, QUIC_ADDRESS_FAMILY_UNSPEC, host.c_str(), uint16_t(port)))) {
            if (c->handle)
                api_->ConnectionClose(c->handle);
            {
                std::lock_guard g(lock_);
                connections_.erase(c->id);
            }
            fail(c->id, "cannot connect to " + address);
        }
        return c->id;
    }

    bool send(ConnectionId id, uint8_t channel, std::span<const uint8_t> data) override
    {
        std::shared_ptr<Connection> c = find(id);
        if (!c || channel >= kChannels || data.size() > kMaxMessage)
            return false;
        if (c->shutDown || !c->connected)
            return false;
        const bool first = !c->send[channel];
        if (first) {
            if (QUIC_FAILED(api_->StreamOpen(c->handle, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, &QuicTransport::onSendStream, nullptr,
                                             &c->send[channel])))
                return false;
            if (QUIC_FAILED(api_->StreamStart(c->send[channel], QUIC_STREAM_START_FLAG_IMMEDIATE))) {
                api_->StreamClose(c->send[channel]);
                c->send[channel] = nullptr;
                return false;
            }
        }
        SendBuffer* b = SendBuffer::make((first ? 1 : 0) + 4 + data.size());
        uint8_t* p = b->buffer.Buffer;
        if (first)
            *p++ = channel;
        const uint32_t n = uint32_t(data.size());
        std::memcpy(p, &n, 4);
        if (n)
            std::memcpy(p + 4, data.data(), n);
        if (QUIC_FAILED(api_->StreamSend(c->send[channel], &b->buffer, 1, QUIC_SEND_FLAG_NONE, b))) {
            std::free(b);
            return false;
        }
        return true;
    }

    bool sendDatagram(ConnectionId id, std::span<const uint8_t> data) override
    {
        std::shared_ptr<Connection> c = find(id);
        if (!c || data.empty() || data.size() > c->maxDatagram.load())
            return false;
        if (c->shutDown || !c->connected)
            return false;
        SendBuffer* b = SendBuffer::make(data.size());
        std::memcpy(b->buffer.Buffer, data.data(), data.size());
        if (QUIC_FAILED(api_->DatagramSend(c->handle, &b->buffer, 1, QUIC_SEND_FLAG_NONE, b))) {
            std::free(b);
            return false;
        }
        return true;
    }

    void disconnect(ConnectionId id, const char* reason) override
    {
        std::shared_ptr<Connection> c = find(id);
        if (!c)
            return;
        if (c->shutDown)
            return;
        {
            std::lock_guard g(c->lock);
            c->reason = reason ? reason : "closed";
        }
        api_->ConnectionShutdown(c->handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }

    bool poll(Event& e) override
    {
        std::shared_ptr<Connection> gone;
        {
            std::lock_guard g(lock_);
            if (events_.empty())
                return false;
            e = std::move(events_.front());
            events_.pop_front();
            if (e.type == Event::Type::Disconnected)
                if (auto it = connections_.find(e.connection); it != connections_.end()) {
                    gone = std::move(it->second);
                    connections_.erase(it);
                }
        }
        if (gone)
            closeHandles(*gone);  // (its shutdown completed: no callback comes after; on this thread)
        return true;
    }

    ConnectionStats stats(ConnectionId id) const override
    {
        ConnectionStats s;
        std::shared_ptr<Connection> c = find(id);
        if (!c)
            return s;
        s.maxDatagram = c->maxDatagram.load();
        if (c->shutDown)
            return s;
        QUIC_STATISTICS_V2 q = {};
        uint32_t size = sizeof(q);
        if (QUIC_SUCCEEDED(api_->GetParam(c->handle, QUIC_PARAM_CONN_STATISTICS_V2, &size, &q))) {
            s.rttMs = float(q.Rtt) / 1000.0f;
            s.bytesSent = q.SendTotalBytes;
            s.bytesReceived = q.RecvTotalBytes;
            const uint64_t lost = q.SendSuspectedLostPackets - std::min(q.SendSpuriousLostPackets, q.SendSuspectedLostPackets);
            s.lossPercent = q.SendTotalPackets ? 100.0f * float(lost) / float(q.SendTotalPackets) : 0.0f;
        }
        return s;
    }

private:
    // A connection whose shutdown completed: its streams' and its handle closed (the game's thread).
    void closeHandles(Connection& c)
    {
        for (HQUIC& stream : c.send)
            if (stream) {
                api_->StreamClose(stream);
                stream = nullptr;
            }
        if (c.handle) {
            api_->ConnectionClose(c.handle);
            c.handle = nullptr;
        }
    }

    std::shared_ptr<Connection> find(ConnectionId id) const
    {
        std::lock_guard g(lock_);
        auto it = connections_.find(id);
        return it == connections_.end() ? nullptr : it->second;
    }

    void push(Event&& e)
    {
        std::lock_guard g(lock_);
        events_.push_back(std::move(e));
    }

    void fail(ConnectionId id, std::string why)
    {
        Event e;
        e.type = Event::Type::Disconnected;
        e.connection = id;
        e.text = std::move(why);
        push(std::move(e));
    }

    bool openServerConfig(const ServerCertificate& cert)
    {
        if (serverConfig_)
            return true;
        // MsQuic's OpenSSL build reads PEM files: written to a private folder, read, removed.
        std::error_code ec;
        static std::atomic<uint32_t> serial{0};
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
                                          ("falcon-quic-" + std::to_string(platform::processId()) + "-" + std::to_string(serial++));
        std::filesystem::create_directories(dir, ec);
        const std::string certPath = (dir / "certificate.pem").string(), keyPath = (dir / "key.pem").string();
        const auto write = [](const std::string& path, const std::string& text) {
            FILE* f = platform::openFile(std::filesystem::path(path), "wb");
            if (!f)
                return false;
            const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            std::fclose(f);
            return ok;
        };
        bool ok = write(certPath, cert.certificate) && write(keyPath, cert.key);
        if (ok) {
            const QUIC_SETTINGS s = settings();
            ok = QUIC_SUCCEEDED(api_->ConfigurationOpen(g_registration, &kAlpn, 1, &s, sizeof(s), nullptr, &serverConfig_));
            if (ok) {
                QUIC_CERTIFICATE_FILE files = {keyPath.c_str(), certPath.c_str()};
                QUIC_CREDENTIAL_CONFIG credentials = {};
                credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
                credentials.Flags = QUIC_CREDENTIAL_FLAG_NONE;
                credentials.CertificateFile = &files;
                const QUIC_STATUS status = api_->ConfigurationLoadCredential(serverConfig_, &credentials);
                if (QUIC_FAILED(status)) {
                    FX_ERROR("quic: the server certificate would not load (0x%08lx)", (unsigned long)status);
                    api_->ConfigurationClose(serverConfig_);
                    serverConfig_ = nullptr;
                    ok = false;
                }
            }
        }
        std::filesystem::remove_all(dir, ec);
        return ok;
    }

    bool openClientConfig()
    {
        if (clientConfig_)
            return true;
        const QUIC_SETTINGS s = settings();
        if (QUIC_FAILED(api_->ConfigurationOpen(g_registration, &kAlpn, 1, &s, sizeof(s), nullptr, &clientConfig_)))
            return false;
        // Not the system's trust: game servers sign themselves. A pinned address checks the
        // certificate's SHA-256 (onConnection); the session authenticates either way.
        QUIC_CREDENTIAL_CONFIG credentials = {};
        credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION |
                            QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED | QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
        if (QUIC_FAILED(api_->ConfigurationLoadCredential(clientConfig_, &credentials))) {
            api_->ConfigurationClose(clientConfig_);
            clientConfig_ = nullptr;
            return false;
        }
        return true;
    }

    static QUIC_STATUS QUIC_API onListener(HQUIC, void* context, QUIC_LISTENER_EVENT* event)
    {
        auto* self = static_cast<QuicTransport*>(context);
        if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
            return QUIC_STATUS_SUCCESS;
        auto c = std::make_shared<Connection>();
        c->owner = self;
        c->handle = event->NEW_CONNECTION.Connection;
        {
            std::lock_guard g(self->lock_);
            c->id = self->next_++;
            self->connections_[c->id] = c;
        }
        self->api_->SetCallbackHandler(c->handle, reinterpret_cast<void*>(&QuicTransport::onConnection), c.get());
        const QUIC_STATUS status = self->api_->ConnectionSetConfiguration(c->handle, self->serverConfig_);
        if (QUIC_FAILED(status)) {
            std::lock_guard g(self->lock_);
            self->connections_.erase(c->id);
        }
        return status;
    }

    static QUIC_STATUS QUIC_API onConnection(HQUIC handle, void* context, QUIC_CONNECTION_EVENT* event)
    {
        auto* c = static_cast<Connection*>(context);
        QuicTransport* self = c->owner;
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED: {
            c->connected = true;
            Event e;
            e.type = Event::Type::Connected;
            e.connection = c->id;
            self->push(std::move(e));
            break;
        }
        case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED: {
            if (c->pin.empty())
                return QUIC_STATUS_SUCCESS;
            const auto* der = static_cast<const QUIC_BUFFER*>(event->PEER_CERTIFICATE_RECEIVED.Certificate);
            const std::string got = der ? sha256Hex(der->Buffer, der->Length) : std::string();
            if (got != c->pin) {
                std::lock_guard g(c->lock);
                c->reason = "the server's certificate is not the pinned one (sha256 " + got + ")";
                return QUIC_STATUS_BAD_CERTIFICATE;
            }
            return QUIC_STATUS_SUCCESS;
        }
        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
            c->maxDatagram = event->DATAGRAM_STATE_CHANGED.SendEnabled ? event->DATAGRAM_STATE_CHANGED.MaxSendLength : 0;
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            const QUIC_BUFFER* b = event->DATAGRAM_RECEIVED.Buffer;
            Event e;
            e.type = Event::Type::Datagram;
            e.connection = c->id;
            e.data.assign(b->Buffer, b->Buffer + b->Length);
            self->push(std::move(e));
            break;
        }
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State))
                std::free(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
            break;
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            auto* in = new Incoming;
            in->connection = c;
            self->api_->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream, reinterpret_cast<void*>(&QuicTransport::onReceiveStream), in);
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
            std::lock_guard g(c->lock);
            if (c->reason.empty()) {
                const QUIC_STATUS s = event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
                char text[96];
                std::snprintf(text, sizeof(text), "%s (0x%08lx)",
                              s == QUIC_STATUS_CONNECTION_IDLE      ? "idle timeout"
                              : s == QUIC_STATUS_CONNECTION_TIMEOUT ? "no answer"
                              : s == QUIC_STATUS_CONNECTION_REFUSED ? "refused"
                                                                    : "the transport closed it",
                              (unsigned long)s);
                c->reason = text;
            }
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER: {
            std::lock_guard g(c->lock);
            if (c->reason.empty())
                c->reason = "the other side closed it";
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
            std::string reason;
            {
                std::lock_guard g(c->lock);
                reason = c->reason.empty() ? "closed" : c->reason;
            }
            c->shutDown = true;
            if (event->SHUTDOWN_COMPLETE.AppCloseInProgress)
                break;  // (the transport is closing it: no event)
            Event e;
            e.type = Event::Type::Disconnected;
            e.connection = c->id;
            e.text = std::move(reason);
            self->push(std::move(e));  // (poll closes its handle on the game's thread)
            break;
        }
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API onSendStream(HQUIC stream, void*, QUIC_STREAM_EVENT* event)
    {
        (void)stream;  // (closed with its connection, on the game's thread)
        if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE)
            std::free(event->SEND_COMPLETE.ClientContext);
        return QUIC_STATUS_SUCCESS;
    }

    static QUIC_STATUS QUIC_API onReceiveStream(HQUIC stream, void* context, QUIC_STREAM_EVENT* event)
    {
        auto* in = static_cast<Incoming*>(context);
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER& b = event->RECEIVE.Buffers[i];
                in->bytes.insert(in->bytes.end(), b.Buffer, b.Buffer + b.Length);
            }
            if (in->channel < 0 && in->bytes.size() > in->read)
                in->channel = in->bytes[in->read++];
            while (in->channel >= 0 && in->bytes.size() - in->read >= 4) {
                uint32_t n;
                std::memcpy(&n, in->bytes.data() + in->read, 4);
                if (n > kMaxMessage || in->channel >= kChannels) {
                    g_api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 1);  // (not ours)
                    break;
                }
                if (in->bytes.size() - in->read - 4 < n)
                    break;
                Event e;
                e.type = Event::Type::Message;
                e.connection = in->connection->id;
                e.channel = uint8_t(in->channel);
                e.data.assign(in->bytes.begin() + ptrdiff_t(in->read + 4), in->bytes.begin() + ptrdiff_t(in->read + 4 + n));
                in->read += 4 + n;
                in->connection->owner->push(std::move(e));
            }
            if (in->read > 65536 && in->read * 2 > in->bytes.size()) {  // (what was read goes)
                in->bytes.erase(in->bytes.begin(), in->bytes.begin() + ptrdiff_t(in->read));
                in->read = 0;
            }
            break;
        }
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress)
                g_api->StreamClose(stream);
            delete in;
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    const QUIC_API_TABLE* api_ = nullptr;
    HQUIC listener_ = nullptr, serverConfig_ = nullptr, clientConfig_ = nullptr;
    uint16_t port_ = 0;
    std::string listenHost_;
    std::string certificate_;
    mutable std::mutex lock_;
    std::unordered_map<ConnectionId, std::shared_ptr<Connection>> connections_;
    std::deque<Event> events_;
    ConnectionId next_ = 1;
};

}  // namespace

void setQuicLibrary(const std::string& path)
{
    std::lock_guard g(g_quicLock);
    g_libraryPath = path;
}

void setQuicTimeouts(uint32_t idleMs, uint32_t keepAliveMs)
{
    g_idleMs = idleMs;
    g_keepAliveMs = keepAliveMs;
}

std::unique_ptr<Transport> makeQuicTransport(const std::string& rest)
{
    // The address's host ("127.0.0.1:27015#pin", "[::1]:0", "127.0.0.1"): where a server listens.
    std::string host = rest.substr(0, rest.find('#'));
    if (!host.empty() && host[0] == '[')
        host = host.substr(1, host.find(']') == std::string::npos ? std::string::npos : host.find(']') - 1);
    else if (host.find(':') != std::string::npos && host.find(':') == host.rfind(':'))
        host = host.substr(0, host.find(':'));
    return std::make_unique<QuicTransport>(host);
}

}  // namespace fx::net

#else  // (built without external/msquic: no quic://)

#include "core/log.h"

namespace fx::net {

void setQuicLibrary(const std::string&) {}
void setQuicTimeouts(uint32_t, uint32_t) {}
std::unique_ptr<Transport> makeQuicTransport(const std::string&)
{
    FX_ERROR("quic: this build has no QUIC (run scripts/fetch-msquic.ps1, then build again)");
    return nullptr;
}

}  // namespace fx::net

#endif
