#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <slim/common/io/operations.h>
#include <slim/common/io/scheduler.h>
#include <slim/common/io/runtime.h>
#include <slim/common/io/task.h>
#include <slim/common/network/server/tcp.h>

namespace slim::common::network::server {

namespace {

int make_listen_fd(const tcp::Config& config) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(config.port));
    if (::inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        throw std::runtime_error("Invalid host address");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to bind");
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to listen");
    }
    return fd;
}

SSL_CTX* make_ssl_ctx(const tcp::Config& config) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        throw std::runtime_error("Failed to create SSL context");
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    if (SSL_CTX_use_certificate_file(ctx, config.cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("Failed to load certificate");
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, config.key.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("Failed to load private key");
    }
    return ctx;
}

} // namespace

slim::common::io::Task<void> Tcp::accept_loop(Tcp& self, slim::common::io::Runtime& runtime) {
    auto& dispatcher = runtime.dispatcher_scheduler();

    while (!self.stop_token_.stop_requested()) {
        slim::common::io::Accept accept_op{dispatcher, self.listen_fd_};
        int client_fd = co_await accept_op;
        if (client_fd < 0) continue;

        SSL_CTX* ctx = self.ssl_ctx_;
        runtime.post([client_fd, ctx, &handler = self.connection_handler_](
                         slim::common::io::Scheduler& worker_scheduler, size_t) {
            auto conn = handler(worker_scheduler, client_fd, ctx);
            worker_scheduler.spawn(std::move(conn));
        });
    }
}

Tcp::Tcp(const tcp::Config& config,
         slim::common::io::Runtime& runtime,
         std::stop_token stop_token,
         ConnectionHandler connection_handler)
    : stop_token_(std::move(stop_token)),
      stop_callback_(stop_token_, [this]{ ::shutdown(listen_fd_, SHUT_RDWR); }) {

    listen_fd_          = make_listen_fd(config);
    ssl_ctx_            = (!config.cert.empty() && !config.key.empty()) ? make_ssl_ctx(config) : nullptr;
    connection_handler_ = std::move(connection_handler);

    // Scheduler::spawn() mutates the dispatcher's internal task list and
    // stages the first SQE directly on the dispatcher's own IO ring. Both
    // are only safe to touch from the thread actually driving that
    // Scheduler's run() loop -- which, once runtime.start() has been
    // called, is the dispatcher's own jthread, not whichever thread is
    // constructing this Tcp object. Calling spawn() directly here would
    // race with that thread's concurrent drain()/reap() calls. Instead,
    // use the already-thread-safe post() so the actual spawn() call runs
    // via drain_inbox() on the dispatcher's own thread.
    //
    // accept_loop is a plain static function taking `self`/`runtime` by
    // reference, not a capturing lambda -- so its coroutine frame holds
    // real references into long-lived objects (this Tcp, and Runtime),
    // never a pointer into a throwaway closure that dies once this post()
    // callback returns.
    runtime.dispatcher_scheduler().post([this, &runtime]() {
        runtime.dispatcher_scheduler().spawn(accept_loop(*this, runtime));
    });
}

Tcp::~Tcp() {
    if (ssl_ctx_)        SSL_CTX_free(ssl_ctx_);
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

} // namespace slim::common::network::server
