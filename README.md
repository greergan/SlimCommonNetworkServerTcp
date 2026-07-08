<a href="https://codeberg.org/greergan/SlimTS">
  <img src="https://raw.githubusercontent.com/greergan/SlimTS/master/assets/slimts_logo.png" width="75" alt="SlimTS Logo">
</a>

# SlimCommonNetworkServerTcp

An asynchronous, io_uring-backed TCP server with optional TLS support in modern C++.  
Part of the [SlimCommon](https://codeberg.org/greergan/SlimCommon) library.  
Built using [SlimLibraryPackager](https://codeberg.org/greergan/SlimLibraryPackager).  
CI/CD supplied by unified workflows provided by [SlimLibraryPackager](https://codeberg.org/greergan/SlimLibraryPackager).

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Core API](#core-api)
  - [ErrorStatus enum](#errorstatus-enum)
  - [NetworkException](#networkexception)
  - [Config struct](#config-struct)
  - [ConnectionHandler](#connectionhandler)
  - [Tcp class](#tcp-class)
  - [Constructor and object lifetime](#constructor-and-object-lifetime)
  - [Shutdown behaviour](#shutdown-behaviour)
- [Building](#building)
- [Dependencies](#dependencies)
  - [required_packages](#required_packages)
  - [external_dependencies](#external_dependencies)
  - [slim_flags](#slim_flags)
- [Examples](#examples)

## Overview

This library provides an asynchronous TCP server built on `slim::common::io`'s coroutine-based `Runtime`/`Scheduler`. A single dispatcher coroutine accepts incoming connections and hands each one off to a worker thread's scheduler, where a caller-supplied handler coroutine drives the connection. Optional TLS is supported via OpenSSL, with the handshake left to the handler.

The server does no per-connection work itself beyond `accept()` and dispatch — all connection logic lives in the `ConnectionHandler` supplied at construction.

[↑ Top](#table-of-contents)

## Features

| Feature | Description |
|---------|-------------|
| Async accept loop | Runs as a coroutine on the `Runtime`'s dispatcher, never blocks a thread |
| Work distribution | Each accepted connection is posted to a worker `Scheduler` via `Runtime::post` |
| Optional TLS | `SSL_CTX` created from `Config::cert`/`Config::key` when both are set; `nullptr` otherwise |
| Clean shutdown | A `std::stop_token` triggers an immediate `shutdown()` of the listening socket, unblocking the accept loop right away |
| Pluggable handling | Connection logic is entirely caller-defined via `ConnectionHandler` |
| No copies | `Tcp` is non-copyable; the object owns its listening socket and TLS context for its lifetime |

[↑ Top](#table-of-contents)

## Core API

### ErrorStatus enum

`ErrorStatus` is the scoped enum provided by [SlimCommonNetwork](https://codeberg.org/greergan/SlimCommonNetwork). Server-side setup errors are reported through the same enum used by the client library:

| Group | Values | Meaning |
|-------|--------|---------|
| Listen socket | `ListenSocketCreationFailed`, `ListenAddressInvalid`, `ListenSocketBindFailed`, `ListenSocketListenFailed` | Failed to create, bind, or listen on the server socket |
| TLS | `TlsContextCreationFailed`, `TlsCertificateLoadFailed`, `TlsPrivateKeyLoadFailed` | Failed to create the TLS context or load the certificate/key |

Internal helpers (`make_listen_fd`, `make_ssl_ctx`) return `ErrorStatus` directly and write their result through an out-parameter, rather than throwing. The `Tcp` constructor checks these and throws `NetworkException` on failure — errors never surface as raw `ErrnoException`/`std::runtime_error` strings.

[↑ Top](#table-of-contents)

### NetworkException

`NetworkException` is the exception class provided by [SlimCommonNetwork](https://codeberg.org/greergan/SlimCommonNetwork). Thrown by the `Tcp` constructor on setup failure, carrying the corresponding `ErrorStatus`. Call `.error()` to inspect it, or `.what()` for the human-readable message.

```cpp
try {
    slim::common::network::server::Tcp server(config, runtime, source.get_token(), handler);
}
catch (const slim::common::network::NetworkException& e) {
    std::cerr << e.what() << " (" << static_cast<int>(e.error()) << ")\n";
}
```

[↑ Top](#table-of-contents)

### Config struct

```cpp
namespace slim::common::network::server::tcp {
    struct Config {
        std::string host;
        int         port{0};
        std::string cert;
        std::string key;
    };
}
```

`cert` and `key` are file paths. TLS is enabled only when both are non-empty; otherwise the server runs as plain TCP and handlers receive a `nullptr` `SSL_CTX*`.

[↑ Top](#table-of-contents)

### ConnectionHandler

```cpp
using ConnectionHandler =
    std::function<slim::common::io::Task<void>(slim::common::io::Scheduler&, int, SSL_CTX*)>;
```

Invoked once per accepted connection, on a worker thread, with the worker's `Scheduler`, the connected socket's file descriptor, and the shared `SSL_CTX*` (or `nullptr` for plain TCP). The handler owns the file descriptor and is responsible for closing it.

[↑ Top](#table-of-contents)

### Tcp class

```cpp
slim::common::network::server::Tcp server(config, runtime, stop_token, handler);
```

[↑ Top](#table-of-contents)

### Constructor and object lifetime

| Form | Description |
|------|-------------|
| `Tcp(const tcp::Config& config, slim::common::io::Runtime& runtime, std::stop_token stop_token, ConnectionHandler connection_handler)` | Binds and listens immediately; the accept loop is scheduled on `runtime`'s dispatcher before the constructor returns |
| `Tcp(const Tcp&)` | Deleted — copies are not allowed |
| `Tcp& operator=(const Tcp&)` | Deleted |

`runtime` must already be running (`Runtime::start()` called) before constructing a `Tcp`, and must outlive it. The constructor throws `NetworkException` (see [ErrorStatus enum](#errorstatus-enum)) on socket, bind, listen, or TLS context creation failure.

[↑ Top](#table-of-contents)

### Shutdown behaviour

Requesting stop on the `std::stop_token` passed at construction immediately calls `shutdown()` on the listening socket. This interrupts a pending `accept()` right away rather than waiting for the next incoming connection, so the accept loop exits promptly and the server can be torn down deterministically.

[↑ Top](#table-of-contents)

## Building

This library is built using [SlimLibraryPackager](https://codeberg.org/greergan/SlimLibraryPackager). See that repository for build instructions.

To enable TLS support, provide non-empty `cert`/`key` paths in `Config`; OpenSSL must be available at build time.

[↑ Top](#table-of-contents)

## Dependencies

### required_packages

External package dependencies for this library are declared in the [`required_packages`](required_packages) file at the repository root. This file is read by [SlimLibraryPackager](https://codeberg.org/greergan/SlimLibraryPackager) during the build process to resolve dependencies and install them if not present.

```
SlimCommonIo
SlimCommonNetwork
```

- [SlimCommonIo](https://codeberg.org/greergan/SlimCommonIo)
- [SlimCommonNetwork](https://codeberg.org/greergan/SlimCommonNetwork)

[↑ Top](#table-of-contents)

### external_dependencies

External (non-SlimCommon) dependencies are declared in the [`external_dependencies`](external_dependencies) file at the repository root. This file is read by [SlimLibraryPackager](https://codeberg.org/greergan/SlimLibraryPackager) during the build process to resolve and install them if not present.

```
boringssl
```

- [BoringSSL](https://boringssl.googlesource.com/boringssl) — used for optional TLS support

[↑ Top](#table-of-contents)

### slim_flags

Compiler and linker flags are declared in the [`slim_flags`](slim_flags) file at the repository root. This file is read by [SlimLibraryPackager](https://codeberg.org/greergan/SlimLibraryPackager) during the build process to apply the necessary flags.

```
LD_FLAGS -lssl -lcrypto
```

[↑ Top](#table-of-contents)

## Examples

```cpp
// Plain TCP echo server
slim::common::io::Runtime runtime(4);
runtime.start();

std::stop_source source;

slim::common::network::server::tcp::Config config{
    .host = "0.0.0.0",
    .port = 8080
};

slim::common::network::server::Tcp server(
    config,
    runtime,
    source.get_token(),
    [](slim::common::io::Scheduler& scheduler, int fd, SSL_CTX* ctx) -> slim::common::io::Task<void> {
        // read/write on fd using scheduler-driven ops, then close(fd)
        co_return;
    });

// ... serve until shutdown ...
source.request_stop();
runtime.stop();
```

```cpp
// TLS server
slim::common::network::server::tcp::Config config{
    .host = "0.0.0.0",
    .port = 8443,
    .cert = "/etc/ssl/certs/server.pem",
    .key  = "/etc/ssl/private/server.key"
};

std::stop_source source;
slim::common::network::server::Tcp server(
    config,
    runtime,
    source.get_token(),
    [](slim::common::io::Scheduler& scheduler, int fd, SSL_CTX* ctx) -> slim::common::io::Task<void> {
        // perform TLS handshake using ctx, then serve the connection
        co_return;
    });
```

```cpp
// Graceful shutdown
std::stop_source source;
slim::common::network::server::Tcp server(config, runtime, source.get_token(), handler);

// later, e.g. on SIGINT
source.request_stop();  // unblocks the accept loop immediately
runtime.stop();
```

[↑ Top](#table-of-contents)
