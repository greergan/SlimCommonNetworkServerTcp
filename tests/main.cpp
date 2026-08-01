#include <catch2/catch_test_macros.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <stop_token>
#include <openssl/ssl.h>
#include <slim/common/io/runtime.h>
#include <slim/common/io/scheduler.h>
#include <slim/common/io/operations.h>
#include <slim/common/io/task.h>
#include <slim/common/network/server/tcp.h>

using namespace slim::common::io;
using namespace slim::common::network::server;
using namespace std::chrono_literals;

// ─── Helpers ────────────────────────────────────────────────────────────────

static int connect_to(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static void generate_self_signed_cert(const std::string& cert_path, const std::string& key_path) {
    std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -keyout " + key_path +
        " -out " + cert_path +
        " -days 1 -nodes -subj \"/CN=localhost\" 2>/dev/null";
    REQUIRE(std::system(cmd.c_str()) == 0);
}

// ─── Plain TCP ──────────────────────────────────────────────────────────────

TEST_CASE("Tcp server accepts connection and invokes handler", "[tcp][server]") {
    Runtime runtime(2);
    runtime.start();

    std::atomic<bool>     handler_called{false};
    std::atomic<SSL_CTX*> received_ctx{reinterpret_cast<SSL_CTX*>(0x1)}; // sentinel

    tcp::Config config{"127.0.0.1", 19950, {}, {}};
    std::stop_source source;

    auto handler = [&](Scheduler&, int fd, SSL_CTX* ctx) -> Task<void> {
        handler_called.store(true);
        received_ctx.store(ctx);
        ::close(fd);
        co_return;
    };

    Tcp server(config, runtime, source.get_token(), handler);

    std::this_thread::sleep_for(50ms);
    int client = connect_to(19950);
    REQUIRE(client >= 0);
    std::this_thread::sleep_for(100ms);

    REQUIRE(handler_called.load());
    REQUIRE(received_ctx.load() == nullptr);

    ::close(client);
    source.request_stop();
    runtime.stop();
}

TEST_CASE("Tcp server distributes connections across workers", "[tcp][server]") {
    Runtime runtime(4);
    runtime.start();

    std::atomic<int> handler_count{0};

    tcp::Config config{"127.0.0.1", 19951, {}, {}};
    std::stop_source source;

    auto handler = [&](Scheduler&, int fd, SSL_CTX*) -> Task<void> {
        handler_count.fetch_add(1);
        ::close(fd);
        co_return;
    };

    Tcp server(config, runtime, source.get_token(), handler);

    std::this_thread::sleep_for(50ms);

    std::vector<int> clients;
    for (int i = 0; i < 8; ++i) {
        int fd = connect_to(19951);
        REQUIRE(fd >= 0);
        clients.push_back(fd);
    }

    std::this_thread::sleep_for(150ms);

    REQUIRE(handler_count.load() == 8);

    for (int fd : clients) ::close(fd);
    source.request_stop();
    runtime.stop();
}

TEST_CASE("Tcp server stops accepting after stop_token requested", "[tcp][server]") {
    Runtime runtime(2);
    runtime.start();

    std::atomic<int> handler_count{0};

    tcp::Config config{"127.0.0.1", 19952, {}, {}};
    std::stop_source source;

    auto handler = [&](Scheduler&, int fd, SSL_CTX*) -> Task<void> {
        handler_count.fetch_add(1);
        ::close(fd);
        co_return;
    };

    Tcp server(config, runtime, source.get_token(), handler);

    std::this_thread::sleep_for(50ms);
    int c1 = connect_to(19952);
    REQUIRE(c1 >= 0);
    std::this_thread::sleep_for(100ms);
    ::close(c1);

    int before_stop = handler_count.load();
    REQUIRE(before_stop == 1);

    source.request_stop();
    std::this_thread::sleep_for(100ms);
    runtime.stop();

    // Connection attempts after stop may still hit the listen backlog
    // (kernel-level), but the accept loop coroutine should not process them.
    int c2 = connect_to(19952);
    std::this_thread::sleep_for(50ms);
    if (c2 >= 0) ::close(c2);

    REQUIRE(handler_count.load() == before_stop);
}

TEST_CASE("Tcp server throws on invalid host", "[tcp][server]") {
    Runtime runtime(1);
    runtime.start();

    tcp::Config config{"not_a_valid_host", 19953, {}, {}};
    std::stop_source source;

    auto handler = [](Scheduler&, int fd, SSL_CTX*) -> Task<void> {
        ::close(fd);
        co_return;
    };

    REQUIRE_THROWS_AS(
        Tcp(config, runtime, source.get_token(), handler),
        std::runtime_error);

    runtime.stop();
}

TEST_CASE("Tcp server throws on invalid cert path", "[tcp][server]") {
    Runtime runtime(1);
    runtime.start();

    tcp::Config config{"127.0.0.1", 19954, "/nonexistent/cert.pem", "/nonexistent/key.pem"};
    std::stop_source source;

    auto handler = [](Scheduler&, int fd, SSL_CTX*) -> Task<void> {
        ::close(fd);
        co_return;
    };

    REQUIRE_THROWS_AS(
        Tcp(config, runtime, source.get_token(), handler),
        std::runtime_error);

    runtime.stop();
}

// ─── TLS ────────────────────────────────────────────────────────────────────

TEST_CASE("Tcp server passes non-null SSL_CTX when cert/key configured", "[tcp][server][tls]") {
    std::string cert_path = "/tmp/slim_tcp_test_cert.pem";
    std::string key_path  = "/tmp/slim_tcp_test_key.pem";
    generate_self_signed_cert(cert_path, key_path);

    Runtime runtime(2);
    runtime.start();

    std::atomic<bool>     handler_called{false};
    std::atomic<SSL_CTX*> received_ctx{nullptr};

    tcp::Config config{"127.0.0.1", 19955, cert_path, key_path};
    std::stop_source source;

    auto handler = [&](Scheduler&, int fd, SSL_CTX* ctx) -> Task<void> {
        handler_called.store(true);
        received_ctx.store(ctx);
        ::close(fd);
        co_return;
    };

    Tcp server(config, runtime, source.get_token(), handler);

    std::this_thread::sleep_for(50ms);
    int client = connect_to(19955);
    REQUIRE(client >= 0);
    std::this_thread::sleep_for(100ms);

    REQUIRE(handler_called.load());
    REQUIRE(received_ctx.load() != nullptr);

    ::close(client);
    source.request_stop();
    runtime.stop();

    ::unlink(cert_path.c_str());
    ::unlink(key_path.c_str());
}

// ─── Handler runs on worker thread, not dispatcher ─────────────────────────

TEST_CASE("Tcp server handler runs on a worker Scheduler distinct from dispatcher", "[tcp][server]") {
    Runtime runtime(2);
    runtime.start();

    Scheduler* seen_scheduler{nullptr};

    tcp::Config config{"127.0.0.1", 19956, {}, {}};
    std::stop_source source;

    auto handler = [&](Scheduler& sched, int fd, SSL_CTX*) -> Task<void> {
        seen_scheduler = &sched;
        ::close(fd);
        co_return;
    };

    Tcp server(config, runtime, source.get_token(), handler);

    std::this_thread::sleep_for(50ms);
    int client = connect_to(19956);
    REQUIRE(client >= 0);
    std::this_thread::sleep_for(100ms);

    REQUIRE(seen_scheduler != nullptr);
    REQUIRE(seen_scheduler != &runtime.dispatcher_scheduler());

    ::close(client);
    source.request_stop();
    runtime.stop();
}

TEST_CASE("Tcp server accept_loop does not dispatch after Tcp destruction", "[tcp][server]") {
    Runtime runtime(2);
    runtime.start();

    std::atomic<int> handler_count{0};

    std::stop_source source;

    {
        tcp::Config config{"127.0.0.1", 19957, {}, {}};
        Tcp server(config, runtime, source.get_token(), [&](Scheduler&, int fd, SSL_CTX*) -> Task<void> {
            handler_count.fetch_add(1);
            ::close(fd);
            co_return;
        });

        std::this_thread::sleep_for(50ms);
        int c1 = connect_to(19957);
        REQUIRE(c1 >= 0);
        std::this_thread::sleep_for(100ms);
        REQUIRE(handler_count.load() == 1);
        ::close(c1);
    } // Tcp destructs here

    std::this_thread::sleep_for(100ms);
    int c2 = connect_to(19957);
    std::this_thread::sleep_for(100ms);
    if (c2 >= 0) ::close(c2);

    REQUIRE(handler_count.load() == 1);

    runtime.stop();
}
