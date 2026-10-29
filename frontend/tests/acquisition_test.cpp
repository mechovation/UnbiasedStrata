// Local-only PR3 acquisition contract tests. Run via acquisition.sh.
#include "acquisition.hpp"
#include "acquisition_request.hpp"
#include <nlohmann/json.hpp>
#include <poll.h>
#include "cpp-httplib/httplib.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono;
namespace acq = strata_acquisition;

static void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct LocalServer {
    httplib::Server &server;
    int port;
    std::thread worker;
    explicit LocalServer(httplib::Server &s, const char *address = "127.0.0.1")
        : server(s), port(s.bind_to_any_port(address)) {
        require(port > 0, "cannot bind local fixture");
        worker = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 1000 && !server.is_running(); ++i)
            std::this_thread::sleep_for(milliseconds(1));
        if (!server.is_running()) {
            server.stop();
            worker.join();
            throw std::runtime_error("local fixture did not start");
        }
    }
    ~LocalServer() { server.stop(); worker.join(); }
    std::string url(const std::string &path, bool tls = false) const {
        return std::string(tls ? "https://localhost:" : "http://127.0.0.1:") +
               std::to_string(port) + path;
    }
};

// Accept TCP but never complete TLS. In trickle mode send only the prefix of
// a large handshake record, one byte at a time, so progress cannot reset budget.
struct StalledTLS {
    int listener = -1, port = 0;
    std::atomic<bool> stop{false}, accepted{false};
    std::atomic<int> sent{0};
    std::thread worker;
    explicit StalledTLS(bool trickle) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        require(listener >= 0, "cannot create raw fixture socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 1)) {
            close(listener);
            throw std::runtime_error("cannot bind raw fixture");
        }
        socklen_t size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size);
        port = ntohs(address.sin_port);
        worker = std::thread([this, trickle] {
            pollfd pending{listener, POLLIN, 0};
            while (!stop && poll(&pending, 1, 20) <= 0) {}
            if (stop) return;
            int peer = accept(listener, nullptr, nullptr);
            if (peer < 0) return;
            accepted = true;
            const unsigned char prefix[] = {0x16, 0x03, 0x03, 0x40, 0x00, 0x02};
            size_t index = 0;
            auto next = steady_clock::now();
            while (!stop) {
                if (trickle && steady_clock::now() >= next) {
                    unsigned char byte = index < sizeof(prefix) ? prefix[index] : 0;
                    if (send(peer, &byte, 1, MSG_NOSIGNAL) != 1) break;
                    ++index; ++sent;
                    next = steady_clock::now() + milliseconds(75);
                }
                std::this_thread::sleep_for(milliseconds(10));
            }
            close(peer);
        });
    }
    ~StalledTLS() { stop = true; worker.join(); close(listener); }
    std::string url() const { return "https://127.0.0.1:" + std::to_string(port) + "/"; }
};

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: acquisition_test CA CERT KEY\n";
        return 2;
    }
    int failed = 0;
    auto test = [&](const char *name, const std::function<void()> &body) {
        try { body(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    // Status mappings are deliberately not over-specified by these tests.
    auto refused = [&](const std::string &url, const acq::Policy &policy,
                       const std::function<bool()> &cancelled = {}) {
        try { (void)acq::fetch(url, policy, cancelled); }
        catch (const acq::Error &e) {
            require(e.status >= 400 && e.status <= 599, "error lacks HTTP error status");
            require(std::string(e.what()).size() > 8, "error lacks useful diagnostic");
            require(std::string(e.what()).find("secret-password") == std::string::npos,
                    "credential leaked in diagnostic");
            require(std::string(e.what()).find("secret-token") == std::string::npos,
                    "query secret leaked in diagnostic");
            return;
        }
        throw std::runtime_error("fetch unexpectedly succeeded");
    };

    try {
        httplib::Server http, denied;
        httplib::SSLServer https(argv[2], argv[3]), mismatch_https(argv[2], argv[3]);
        require(https.is_valid() && mismatch_https.is_valid(), "invalid TLS fixture");
        std::atomic<int> denied_hits{0}, credential_hits{0}, image_hits{0}, authorization_hits{0}, budget_hits{0};
        const std::string image_bytes("\0\xff\x01image", 8);
        http.Get("/image", [&](const auto &req, auto &res) {
            ++image_hits;
            if (req.has_header("Authorization")) ++authorization_hits;
            res.set_content(image_bytes, "application/octet-stream");
        });
        http.Get("/budget", [&](const auto &, auto &res) {
            ++budget_hits;
            res.set_chunked_content_provider("text/plain", [](size_t offset, httplib::DataSink &sink) {
                if (offset >= 6) { sink.done(); return true; }
                std::this_thread::sleep_for(seconds(1));
                return sink.write("x", 1);
            });
        });
        http.Get("/target", [](const auto &req, auto &res) { res.set_content(req.target, "text/plain"); });
        http.Get("/ok", [](const auto &, auto &res) { res.set_content("local acquisition", "text/plain"); });
        https.Get("/ok", [](const auto &, auto &res) { res.set_content("local acquisition", "text/plain"); });
        denied.Get("/ok", [&](const auto &, auto &res) { ++denied_hits; res.set_content("forbidden", "text/plain"); });
        http.Get("/credential", [&](const auto &, auto &res) { ++credential_hits; res.set_content("forbidden", "text/plain"); });
        http.Get(R"(/body/(9|11))", [](const auto &req, auto &res) {
            const size_t size = std::stoul(req.matches[1]) * 1024 * 1024;
            res.set_content(std::string(size, 'x'), "application/octet-stream");
        });
        // Chunked responses also exercise limits without a Content-Length hint.
        http.Get("/chunked-large", [](const auto &, auto &res) {
            res.set_chunked_content_provider("application/octet-stream", [](size_t offset, httplib::DataSink &sink) {
                if (offset >= 11 * 1024 * 1024) { sink.done(); return true; }
                const std::string block(64 * 1024, 'x');
                return sink.write(block.data(), block.size());
            });
        });
        http.Get("/slow", [](const auto &, auto &res) {
            res.set_chunked_content_provider("text/plain", [](size_t offset, httplib::DataSink &sink) {
                if (offset >= 100) { sink.done(); return true; }
                std::this_thread::sleep_for(milliseconds(250));
                return sink.write("x", 1);
            });
        });
        // Bind first, install redirect routes, then start serving.
        LocalServer blocked(denied, "127.0.0.2");
        const std::string blocked_url = "http://127.0.0.2:" + std::to_string(blocked.port) + "/ok";
        http.Get("/egress", [&](const auto &, auto &res) { res.set_redirect(blocked_url); });
        http.Get(R"(/redirect/(\d+))", [](const auto &req, auto &res) {
            int n = std::stoi(req.matches[1]);
            if (n == 0) res.set_content("local acquisition", "text/plain");
            else res.set_redirect("/redirect/" + std::to_string(n - 1));
        });
        http.Get("/loop", [](const auto &, auto &res) { res.set_redirect("/loop"); });
        LocalServer plain(http);
        https.Get("/downgrade", [&](const auto &, auto &res) { res.set_redirect(plain.url("/ok")); });
        // Bind the same hostname used by TLS requests: localhost can resolve to
        // ::1 first in the CPU container, unlike the IPv4-only HTTP fixture.
        LocalServer secure(https, "localhost");
        // Keep an IPv4 TLS listener so hostname-mismatch coverage reaches TLS,
        // rather than merely failing to connect when localhost binds to IPv6.
        LocalServer mismatch_secure(mismatch_https);
        acq::Policy local;
        local.allowed_cidrs = "127.0.0.1/32,::1/128";
        local.ca_bundle = argv[1];

        test("mapped IPv6 loopback address policy", [&] {
            require(acq::Address("::ffff:127.0.0.1").family == AF_INET, "mapped address not normalized");
            bool rejected = false;
            try { acq::validate_address("::ffff:127.0.0.1", acq::Policy{}); }
            catch (const acq::Error &e) { rejected = true; require(e.status == 400, "wrong policy status"); }
            require(rejected, "mapped loopback allowed by default");
            acq::validate_address("::ffff:127.0.0.1", local);
        });
        test("metadata blocked even with allowlist", [&] {
            auto p = local; p.allowed_cidrs = "0.0.0.0/0,::/0";
            for (const auto *address : {"169.254.169.254", "100.100.100.200", "::ffff:169.254.169.254"}) {
                bool rejected = false;
                try { acq::validate_address(address, p); }
                catch (const acq::Error &e) { rejected = true; require(e.status == 400, "wrong metadata status"); }
                require(rejected, "metadata allowlisted");
            }
        });
        test("credential query redacted without network", [&] {
            try { (void)acq::parse("http://user:secret-password@example.test/image?token=secret-token"); }
            catch (const acq::Error &e) {
                const std::string diagnostic = e.what();
                require(diagnostic.find("secret-password") == std::string::npos &&
                        diagnostic.find("secret-token") == std::string::npos &&
                        diagnostic.find("example.test") == std::string::npos, "source leaked in diagnostic");
                return;
            }
            throw std::runtime_error("credential source accepted");
        });
        test("stalled TLS handshake cancellation", [&] {
            StalledTLS raw(false);
            auto start = acq::Clock::now();
            refused(raw.url(), local, [&] { return acq::Clock::now() - start >= milliseconds(500); });
            auto elapsed = acq::Clock::now() - start;
            require(raw.accepted, "raw fixture never accepted connection");
            require(elapsed >= milliseconds(500) && elapsed < seconds(2), "TLS cancellation not prompt");
        });
        test("trickling incomplete TLS record explicit deadline", [&] {
            StalledTLS raw(true);
            auto start = acq::Clock::now();
            bool expired = false;
            try { (void)acq::fetch(raw.url(), local, {}, start + milliseconds(500)); }
            catch (const acq::Error &e) { expired = true; require(e.status == 504, "not a deadline error"); }
            auto elapsed = acq::Clock::now() - start;
            require(expired && raw.accepted && raw.sent >= 5, "TLS trickle not exercised");
            require(elapsed >= milliseconds(500) && elapsed < seconds(2), "TLS deadline not prompt");
        });
        using Json = nlohmann::json;
        auto image = [](const std::string &url) { return Json{{"type", "image_url"}, {"image_url", {{"url", url}}}}; };
        test("acquire_images duplicate roles inline ordering bytes and no Authorization", [&] {
            const std::string inline_data = "data:image/png;base64,AAEC/w==";
            Json body = {{"messages", Json::array({
                {{"role", "system"}, {"content", Json::array({{{"type", "text"}, {"text", "keep"}}, image(plain.url("/image"))})}},
                {{"role", "user"}, {"content", Json::array({image(inline_data), image(plain.url("/image"))})}},
                {{"role", "assistant"}, {"content", Json::array({image(plain.url("/image"))})}}
            })}};
            auto expected = body;
            std::string encoded = "data:image/octet-stream;base64,";
            base64::encode(image_bytes.begin(), image_bytes.end(), std::back_inserter(encoded));
            for (auto position : {std::pair<size_t, size_t>{0, 1}, {1, 1}, {2, 0}})
                expected["messages"][position.first]["content"][position.second]["image_url"]["url"] = encoded;
            auto ordering = acq::acquire_images(body, local, {});
            require(body == expected, "acquisition changed bytes, inline data, or unrelated content");
            require(ordering.size() == 4, "occurrences deduplicated or inline omitted");
            const size_t messages[] = {0, 1, 1, 2}, parts[] = {1, 0, 1, 0};
            const char *roles[] = {"system", "user", "user", "assistant"};
            for (size_t i = 0; i < ordering.size(); ++i)
                require(ordering[i].message == messages[i] && ordering[i].part == parts[i] && ordering[i].role == roles[i], "occurrence order/role lost");
            require(image_hits == 3 && authorization_hits == 0, "fetch count or Authorization header incorrect");
        });
        test("acquire_images forbidden source maps Error status", [&] {
            Json body = {{"messages", Json::array({{{"role", "user"}, {"content", Json::array({image(blocked_url)})}}})}};
            try { (void)acq::acquire_images(body, local, {}); }
            catch (const acq::Error &e) {
                require(e.status == 400, "forbidden source has wrong mapped status");
                require(denied_hits == 0, "forbidden source contacted");
                return;
            }
            throw std::runtime_error("forbidden image accepted");
        });
        test("acquire_images shares request deadline", [&] {
            Json body = {{"messages", Json::array({{{"role", "user"}, {"content", Json::array({image(plain.url("/budget")), image(plain.url("/budget"))})}}})}};
            auto start = acq::Clock::now();
            bool expired = false;
            try { (void)acq::acquire_images(body, local, {}); }
            catch (const acq::Error &e) { expired = true; require(e.status == 504, "wrong shared deadline status"); }
            auto elapsed = acq::Clock::now() - start;
            require(expired && budget_hits == 2, "shared budget not exercised across two sources");
            require(elapsed >= seconds(9) && elapsed < seconds(12), "deadline reset per image");
        });

        test("escaped signed query target preserved", [&] {
            acq::Policy p; p.allowed_cidrs = "127.0.0.1/32";
            const std::string target = "/target?key=a%20b&signature=x%2By%2Fz%3D&literal=+&key=second";
            require(acq::fetch(plain.url(target), p) == target, "request target was rewritten");
        });
        test("HTTP allowed CIDRs", [&] { require(acq::fetch(plain.url("/ok"), local) == "local acquisition", "wrong body"); });
        test("loopback denied by default", [&] { refused(plain.url("/ok"), acq::Policy{}); });
        test("HTTPS trusted local CA", [&] { require(acq::fetch(secure.url("/ok", true), local) == "local acquisition", "wrong TLS body"); });
        test("HTTPS untrusted local CA", [&] { auto p = local; p.ca_bundle.clear(); refused(secure.url("/ok", true), p); });
        test("HTTPS hostname mismatch", [&] { refused("https://127.0.0.1:" + std::to_string(mismatch_secure.port) + "/ok", local); });
        test("redirect egress rechecked", [&] { refused(plain.url("/egress"), local); require(denied_hits == 0, "disallowed redirect was contacted"); });
        test("HTTPS downgrade rejected", [&] { refused(secure.url("/downgrade", true), local); });
        test("five redirects accepted", [&] { require(acq::fetch(plain.url("/redirect/5"), local) == "local acquisition", "wrong redirected body"); });
        test("six redirects rejected", [&] { refused(plain.url("/redirect/6"), local); });
        test("redirect loop bounded", [&] {
            auto start = steady_clock::now(); refused(plain.url("/loop"), local);
            require(steady_clock::now() - start < seconds(3), "redirect loop not promptly bounded");
        });
        test("9 MiB accepted intact", [&] { require(acq::fetch(plain.url("/body/9"), local) == std::string(9 * 1024 * 1024, 'x'), "large body corrupted"); });
        test("11 MiB rejected", [&] { refused(plain.url("/body/11"), local); });
        test("11 MiB chunked rejected", [&] { refused(plain.url("/chunked-large"), local); });
        test("10 second total deadline despite progress", [&] {
            auto start = steady_clock::now(); refused(plain.url("/slow"), local);
            auto elapsed = steady_clock::now() - start;
            require(elapsed >= seconds(9) && elapsed < seconds(13), "not a 10 second total deadline");
        });
        test("already cancelled", [&] {
            auto start = steady_clock::now(); refused(plain.url("/slow"), local, [] { return true; });
            require(steady_clock::now() - start < seconds(2), "pre-cancellation ignored");
        });
        test("cancellation during download", [&] {
            auto start = steady_clock::now();
            refused(plain.url("/slow"), local, [&] { return steady_clock::now() - start >= milliseconds(500); });
            require(steady_clock::now() - start < seconds(3), "cancellation not prompt");
        });
        test("credential URL rejected and redacted", [&] {
            refused("http://user:secret-password@127.0.0.1:" + std::to_string(plain.port) + "/credential?token=secret-token", local);
            require(credential_hits == 0, "credential URL was contacted");
        });
    } catch (const std::exception &e) {
        std::cerr << "fixture failure: " << e.what() << '\n';
        return 2;
    }
    return failed ? 1 : 0;
}
