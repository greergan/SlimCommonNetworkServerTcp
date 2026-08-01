#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <slim/common/io/operations.h>
#include <slim/common/io/scheduler.h>
#include <slim/common/io/runtime.h>
#include <slim/common/io/task.h>
#include <slim/common/network/server/tcp.h>
#include <slim/common/log.h>

namespace slim::common::network::server {
using namespace slim::common;
std::atomic<bool> stop_requested{false};

namespace {

ErrorStatus make_listen_fd(const tcp::Config& config, int& out_fd) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return ErrorStatus::ListenSocketCreationFailed;
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(config.port));
    if (::inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return ErrorStatus::ListenAddressInvalid;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return ErrorStatus::ListenSocketBindFailed;
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return ErrorStatus::ListenSocketListenFailed;
    }
    out_fd = fd;
    return ErrorStatus::OK;
}

ErrorStatus make_ssl_ctx(const tcp::Config& config, SSL_CTX*& out_ctx) noexcept {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        return ErrorStatus::TlsContextCreationFailed;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, config.cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        return ErrorStatus::TlsCertificateLoadFailed;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, config.key.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        return ErrorStatus::TlsPrivateKeyLoadFailed;
    }
    out_ctx = ctx;
    return ErrorStatus::OK;
}

} // namespace

slim::common::io::Task<void> Tcp::accept_loop(Tcp& self, slim::common::io::Runtime& runtime) {
    log::trace(log::Message(__func__, "begins", __FILE__, __LINE__));
    auto& dispatcher = runtime.dispatcher_scheduler();
    while (!self.stop_token_.stop_requested() || !stop_requested) {
        slim::common::io::Accept accept_op{dispatcher, self.listen_fd_};
        int client_fd = co_await accept_op;
        log::debug(log::Message(__func__, "co_await returned client_fd => " + std::to_string(client_fd), __FILE__, __LINE__));
        if (client_fd < 0) continue;
        log::debug(log::Message(__func__, "posting connection handler for client_fd => " + std::to_string(client_fd), __FILE__, __LINE__));
        SSL_CTX* ctx = self.ssl_ctx_;
        runtime.post([client_fd, ctx, &handler = self.connection_handler_](slim::common::io::Scheduler& worker_scheduler, size_t) {
            auto conn = handler(worker_scheduler, client_fd, ctx);
            worker_scheduler.spawn(std::move(conn));
        });
    }
    log::trace(log::Message(__func__, "ends", __FILE__, __LINE__));
}

Tcp::Tcp(const tcp::Config& config, slim::common::io::Runtime& runtime, std::stop_token stop_token,
    ConnectionHandler connection_handler) : stop_token_(std::move(stop_token)),
    stop_callback_(stop_token_, [this] {
        log::debug(log::Message(__func__, "stop_callback_ invoked, shutdown listen_fd_ => " + std::to_string(listen_fd_), __FILE__, __LINE__));
        ::shutdown(listen_fd_, SHUT_RDWR);
    }) {
    log::trace(log::Message(__func__, "begins", __FILE__, __LINE__));
    ErrorStatus status = make_listen_fd(config, listen_fd_);
    if (status != ErrorStatus::OK) {
        throw NetworkException(status);
    }
    log::debug(log::Message(__func__, "listen_fd_ => " + std::to_string(listen_fd_), __FILE__, __LINE__));
    if (!config.cert.empty() && !config.key.empty()) {
        status = make_ssl_ctx(config, ssl_ctx_);
        if (status != ErrorStatus::OK) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw NetworkException(status);
        }
    }
    connection_handler_ = std::move(connection_handler);
    runtime.dispatcher_scheduler().post([this, &runtime]() {
        log::debug(log::Message(__func__, "spawning accept_loop", __FILE__, __LINE__));
        runtime.dispatcher_scheduler().spawn(accept_loop(*this, runtime));
    });
    log::trace(log::Message(__func__, "ends", __FILE__, __LINE__));
}

Tcp::~Tcp() noexcept {
    log::trace(log::Message(__func__, "begins", __FILE__, __LINE__));
    if (ssl_ctx_)        SSL_CTX_free(ssl_ctx_);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    stop_requested = true;
    log::trace(log::Message(__func__, "ends", __FILE__, __LINE__));
}

} // namespace slim::common::network::server
