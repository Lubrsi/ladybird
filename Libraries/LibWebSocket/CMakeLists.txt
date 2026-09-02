set(SOURCES
    ConnectionInfo.cpp
    Impl/WebSocketImpl.cpp
    WebSocket.cpp
)

ladybird_lib(LibWebSocket websocket)
target_link_libraries(LibWebSocket PRIVATE libwebsocket_rust LibCore LibHTTP LibURL LibDNS)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libwebsocket_rust FFI_HEADER RustFFI.h)
