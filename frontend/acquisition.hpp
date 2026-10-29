#pragma once

// Frontend-owned URL acquisition. Never include source URLs in exceptions/logs.
#include <cpp-httplib/httplib.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>

namespace strata_acquisition {
using Clock = std::chrono::steady_clock;
struct Error : std::runtime_error {
    int status;
    Error(int s, const char * message) : std::runtime_error(message), status(s) {}
};
struct Policy {
    std::string allowed_cidrs;
    std::string ca_bundle;
    static Policy environment() {
        Policy p;
        if (auto v = std::getenv("STRATA_IMAGE_ALLOWED_CIDRS")) p.allowed_cidrs = v;
        if (auto v = std::getenv("STRATA_IMAGE_CA_BUNDLE")) p.ca_bundle = v;
        return p;
    }
};
struct Address {
    int family = 0;
    unsigned char bytes[16] = {};
    explicit Address(const std::string & text) {
        if (inet_pton(AF_INET, text.c_str(), bytes) == 1) family = AF_INET;
        else if (inet_pton(AF_INET6, text.c_str(), bytes) == 1) family = AF_INET6;
        else throw Error(400, "image source has an invalid address");
        // Normalize IPv4-mapped IPv6 before applying IPv4 egress rules.
        static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        if (family == AF_INET6 && !memcmp(bytes, mapped, 12)) {
            memmove(bytes, bytes + 12, 4); family = AF_INET;
        }
    }
    bool in(const std::string & cidr) const {
        auto slash = cidr.find('/');
        Address net(cidr.substr(0, slash));
        int bits = net.family == AF_INET ? 32 : 128;
        if (slash != std::string::npos) {
            auto suffix = cidr.substr(slash + 1);
            if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string::npos || suffix.size() > 3)
                throw Error(500, "invalid image egress CIDR configuration");
            bits = std::stoi(suffix);
        }
        if (bits > (net.family == AF_INET ? 32 : 128)) throw Error(500, "invalid image egress CIDR configuration");
        if (net.family != family) return false;
        for (int i = 0; i < bits; ++i)
            if ((bytes[i/8] ^ net.bytes[i/8]) & (128 >> (i%8))) return false;
        return true;
    }
};
inline void validate_address(const std::string & text, const Policy & policy) {
    Address a(text);
    // Non-routable, multicast, transition and metadata/link-local destinations
    // cannot be enabled by an allowlist. Include Alibaba's non-link-local metadata IP.
    for (auto net : {"0.0.0.0/8", "169.254.0.0/16", "224.0.0.0/3", "100.100.100.200/32",
                     "192.0.0.0/24", "192.0.2.0/24", "198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24",
                     "::/128", "fe80::/10", "ff00::/8", "64:ff9b::/96", "2002::/16", "2001::/32"})
        if (a.in(net)) throw Error(400, "image source destination is prohibited");
    if (a.family == AF_INET6 && !a.in("2000::/3") && !a.in("fc00::/7") && !a.in("::1/128"))
        throw Error(400, "image source IPv6 address is not global or explicitly supported LAN space");
    bool private_address = false;
    for (auto net : {"10.0.0.0/8", "127.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10", "::1/128", "fc00::/7"})
        private_address |= a.in(net);
    bool allowed = false;
    std::istringstream list(policy.allowed_cidrs);
    std::string cidr;
    while (std::getline(list, cidr, ',')) {
        try { allowed |= a.in(cidr); }
        catch (...) { throw Error(500, "invalid image egress CIDR configuration"); }
    }
    if (private_address && !allowed) throw Error(400, "image source LAN address requires an allowed CIDR");
}
struct URL { std::string scheme, host, authority, path; int port; };
inline URL parse(const std::string & text) {
    if (text.size() > 8192 || text.find_first_of("\\\r\n\t ") != std::string::npos || text.find('\0') != std::string::npos)
        throw Error(400, "invalid image source URL");
    static const std::regex pattern(R"(^(https?)://(\[[0-9a-fA-F:]+\]|[a-zA-Z0-9.-]+)(:([0-9]{1,5}))?([/?#].*)?$)");
    std::smatch m;
    if (!std::regex_match(text, m, pattern)) throw Error(400, "image source requires HTTP(S) without credentials");
    URL u; u.scheme = m[1]; u.host = m[2]; u.authority = u.scheme + "://" + u.host + m[3].str();
    if (u.host.front() == '[') u.host = u.host.substr(1, u.host.size()-2);
    u.port = m[4].matched ? std::stoi(m[4]) : (u.scheme == "https" ? 443 : 80);
    if (!u.port || u.port > 65535) throw Error(400, "invalid image source port");
    u.path = m[5].str(); u.path = u.path.substr(0, u.path.find('#'));
    if (u.path.empty() || u.path.front() != '/') u.path = "/" + u.path;
    return u;
}
inline void check(Clock::time_point deadline, const std::function<bool()> & cancelled) {
    if (cancelled && cancelled()) throw Error(400, "image acquisition cancelled");
    if (Clock::now() >= deadline) throw Error(504, "image acquisition deadline exceeded");
}
// libc DNS cannot be interrupted portably. Allow at most one outstanding lookup;
// timed-out work owns no request references and cannot create an unbounded thread queue.
inline std::vector<std::string> resolve(const std::string & host, Clock::time_point deadline,
                                      const std::function<bool()> & cancelled) {
    try { Address numeric(host); return {host}; } catch (const Error &) {}
    static std::atomic<bool> busy{false};
    auto promise = std::make_shared<std::promise<std::vector<std::string>>>();
    auto result = promise->get_future();
    if (busy.exchange(true)) throw Error(502, "image DNS resolver busy; retry later");
    try {
        std::thread([host, promise] {
            try {
                std::vector<std::string> addresses;
                addrinfo hints{}, * raw = nullptr; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
                const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &raw);
                std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(raw, freeaddrinfo);
                if (!rc) {
                    for (auto p = raw; p; p = p->ai_next) {
                        char text[INET6_ADDRSTRLEN];
                        const void * addr = p->ai_family == AF_INET ? (void *)&((sockaddr_in *)p->ai_addr)->sin_addr :
                            p->ai_family == AF_INET6 ? (void *)&((sockaddr_in6 *)p->ai_addr)->sin6_addr : nullptr;
                        if (addr && inet_ntop(p->ai_family, addr, text, sizeof(text))) addresses.emplace_back(text);
                    }
                }
                promise->set_value(std::move(addresses));
            } catch (...) { promise->set_exception(std::current_exception()); }
            busy = false;
        }).detach();
    } catch (...) { busy = false; throw Error(502, "image DNS resolver unavailable"); }
    while (result.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) check(deadline, cancelled);
    check(deadline, cancelled);
    std::vector<std::string> addresses;
    try { addresses = result.get(); }
    catch (...) { throw Error(502, "image source DNS resolution failed"); }
    if (addresses.empty()) throw Error(502, "image source DNS resolution failed");
    return addresses;
}
inline std::string fetch(const std::string & source, const Policy & policy,
                         const std::function<bool()> & cancelled = {},
                         Clock::time_point deadline = Clock::now() + std::chrono::seconds(10)) {
    constexpr size_t limit = 10 * 1024 * 1024;
    std::string current = source;
    for (int hop = 0; ; ++hop) {
        check(deadline, cancelled);
        auto u = parse(current);
        auto addresses = resolve(u.host, deadline, cancelled);
        for (const auto & a : addresses) validate_address(a, policy);
        httplib::Client client(u.authority);
        client.set_hostname_addr_map({{u.host, addresses.front()}}); // pin validated DNS; preserve SNI/Host
        client.set_follow_location(false);
        client.set_path_encode(false); // retain escaped query bytes, including signed URL targets
        client.set_decompress(false);
        client.enable_server_certificate_verification(true);
        client.enable_server_hostname_verification(true);
        if (!policy.ca_bundle.empty()) client.set_ca_cert_path(policy.ca_bundle);
        auto remaining = deadline - Clock::now();
        check(deadline, cancelled);
        client.set_connection_timeout(remaining);
        client.set_read_timeout(remaining);
        client.set_write_timeout(remaining);
        client.set_max_timeout(remaining);
        std::string body;
        bool oversized = false;
        // httplib's stop() takes its socket mutex, held during connect/TLS. A duplicated
        // descriptor lets the watcher shut down that SAME socket without its mutex or
        // fd-reuse races. Keep checking until completion (including pre-connect expiry).
        std::mutex mutex; std::condition_variable cv; bool done = false;
        int watched_fd = -1;
        client.set_socket_options([&](socket_t socket) {
            std::lock_guard<std::mutex> lock(mutex);
            if (watched_fd >= 0) close(watched_fd);
            watched_fd = fcntl(socket, F_DUPFD_CLOEXEC, 0);
            if (watched_fd < 0) {
                // At this pinned httplib callback the newly created socket has not
                // been assigned to ClientImpl yet; throwing alone would leak it.
                close(socket);
                throw Error(502, "image source socket supervision unavailable");
            }
        });
        std::thread watcher([&] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return done; })) {
                if ((Clock::now() >= deadline || (cancelled && cancelled())) && watched_fd >= 0)
                    shutdown(watched_fd, SHUT_RDWR);
            }
        });
        auto finish_watch = [&] {
            { std::lock_guard<std::mutex> lock(mutex); done = true; }
            cv.notify_one(); watcher.join();
            if (watched_fd >= 0) close(watched_fd);
        };
        httplib::Result response;
        try {
            response = client.Get(u.path, {{"Accept-Encoding", "identity"}},
                [&](const char * data, size_t size) {
                    if (size > limit - body.size()) { oversized = true; return false; }
                    body.append(data, size); return true;
                });
        } catch (...) {
            finish_watch();
            check(deadline, cancelled);
            throw Error(502, "image source transport failed");
        }
        finish_watch();
        check(deadline, cancelled);
        if (oversized) throw Error(400, "image source exceeds 10 MiB limit");
        if (!response) {
            if (response.error() == httplib::Error::SSLServerVerification || response.error() == httplib::Error::SSLServerHostnameVerification)
                throw Error(400, "image source TLS certificate or hostname verification failed");
            throw Error(502, "image source connection or TLS handshake failed");
        }
        int status = response->status;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (hop == 5) throw Error(400, "image source exceeds 5 redirects");
            auto location = response->get_header_value("Location");
            if (location.empty()) throw Error(400, "image source redirect has no destination");
            if (location.compare(0, 2, "//") == 0) location = u.scheme + ":" + location;
            else if (location.front() == '/') location = u.authority + location;
            else if (location.front() == '?') location = u.authority + u.path.substr(0, u.path.find('?')) + location;
            else if (location.front() == '#') location = u.authority + u.path + location;
            else if (location.find("://") == std::string::npos) {
                auto path = u.path.substr(0, u.path.find('?'));
                location = u.authority + path.substr(0, path.rfind('/') + 1) + location;
            }
            auto next = parse(location);
            if (u.scheme == "https" && next.scheme != "https") throw Error(400, "image source HTTPS downgrade refused");
            current = std::move(location); continue;
        }
        if (status < 200 || status >= 300) throw Error(status >= 500 ? 502 : 400, "image source returned an unsuccessful HTTP status");
        if (response->has_header("Content-Encoding") && response->get_header_value("Content-Encoding") != "identity")
            throw Error(400, "image source content encoding is unsupported");
        return body;
    }
}
} // namespace strata_acquisition
