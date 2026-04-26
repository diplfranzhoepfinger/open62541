# OPC UA WebSocket Implementation Plan

This document outlines the implementation of WebSocket support for the open62541
OPC UA stack, enabling `opc.ws://` and `opc.wss://` endpoints for both server
and client.

---

## 1. Design Decisions

The following design decisions have been finalized and will be used throughout
the implementation.

| Decision | Value | Rationale |
|----------|-------|-----------|
| **Manager Protocol String** | `"ws"` | A single `ConnectionManager` handles both `ws` and `wss`. This is consistent with the existing HTTP ConnectionManager (`protocol = "http"`, SSL controlled by a `useSSL` parameter). |
| **New Source Files** | `arch/posix/eventloop_posix_lws_ws.c`<br>`arch/posix/eventloop_posix_lws_ws.h` | Clear naming indicating the use of the `libwebsockets` (lws) backend and the POSIX architecture. |
| **TLS Control** | Parameter `useSSL` (`UA_Boolean`) | Passed to `openConnection`. When `true`, lws handles TLS internally. This avoids duplicating the entire manager for encrypted vs. unencrypted connections. |
| **Scope** | Server and Client | Both incoming (`listen=true`) and outgoing (`listen=false`) WebSocket connections are supported. |
| **WebSocket Subprotocol** | `{"opcua", ...}` and `{"", ...}` | The OPC UA Part 6 specification does not mandate a fixed subprotocol. Registering `"opcua"` ensures compatibility with clients that explicitly request it. The empty-string fallback `""` ensures compatibility with clients that do not request any subprotocol. |
| **Frame Type** | `LWS_WRITE_BINARY` | OPC UA Binary Protocol payloads must be transported in WebSocket binary frames, not text frames. |
| **EventLoop Integration** | Reuse `eventloop_posix_lws.c` | The existing custom event loop plugin (`evlib_open62541`) is fully generic and will be used as-is. It binds lws to the open62541 POSIX EventLoop. |

---

## 2. Architectural Overview

The WebSocket ConnectionManager is a hybrid solution:

1. **libwebsockets** handles TCP accept, the WebSocket handshake, TLS termination
   (for `wss://`), and frame encoding/decoding.
2. The **open62541 EventLoop** handles file descriptor polling via the existing
   `eventloop_posix_lws.c` integration layer.
3. The **ConnectionManager** (`eventloop_posix_lws_ws.c`) bridges these worlds:
   it maps lws connection events to `UA_ConnectionManager_connectionCallback`
   calls used by the rest of the stack.

---

## 3. Implementation Phases

### Phase 1: Extend URL Parser

**Files:**
- `src/util/ua_util.c`
- `tests/check_utils_url_kvm.c`

**Changes:**
- Extend the `schemas[]` array in `UA_parseEndpointUrl()` to include
  `"opc.ws://"` and `"opc.wss://"`.
- Increase `UA_SCHEMAS_SIZE` accordingly.
- The existing hostname/port/path parsing logic after the scheme prefix is
  identical to `opc.tcp://` and requires no changes.
- Update unit tests in `check_utils_url_kvm.c` so that `parse_url_ws` and
  `parse_url_wss` expect `UA_STATUSCODE_GOOD`.

---

### Phase 2: WebSocket ConnectionManager

**New Files:**
- `arch/posix/eventloop_posix_lws_ws.h`
- `arch/posix/eventloop_posix_lws_ws.c`

**API:**
```c
UA_ConnectionManager *
UA_ConnectionManager_new_POSIX_WS(const UA_String eventSourceName);
```

**Internal Structure:**
```c
typedef struct {
    UA_POSIXConnectionManager cm; /* Base: eventSource, FD tracking */
    struct lws_context *lwsContext;
    UA_EventLoop *foreign_loop;   /* Reference passed to lws */
} WSConnectionManager;
```

**lws Protocols Array:**
```c
static const struct lws_protocols protocols[] = {
    {"opcua", callback_ws, sizeof(WSConnection), 0, 0, NULL, 0},
    {"",      callback_ws, sizeof(WSConnection), 0, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM
};
```

**lws Callback (`callback_ws`):**
Handles both server and client reasons:

| Reason | Action |
|--------|--------|
| `LWS_CALLBACK_ESTABLISHED` | New incoming server connection. Notify application callback with `ESTABLISHED`. |
| `LWS_CALLBACK_RECEIVE` | Binary data received. Forward to application callback with message payload. |
| `LWS_CALLBACK_SERVER_WRITEABLE` | Flush buffered data via `lws_write(..., LWS_WRITE_BINARY)`. |
| `LWS_CALLBACK_CLOSED` | Connection closed. Notify application callback with `CLOSING`. |
| `LWS_CALLBACK_CLIENT_ESTABLISHED` | Outgoing client connection established. |
| `LWS_CALLBACK_CLIENT_RECEIVE` | Data received on client connection. |
| `LWS_CALLBACK_CLIENT_WRITEABLE` | Client ready to send. |
| `LWS_CALLBACK_CLIENT_CONNECTION_ERROR` | Outgoing connection failed. Close and notify. |

**ConnectionManager Methods:**

| Method | Behavior |
|--------|----------|
| `WS_openConnection()` | If `listen=true`: create lws context bound to a real port.<br>If `listen=false`: call `lws_client_connect_via_info()`. |
| `WS_sendWithConnection()` | Copy data to an internal per-connection buffer, then call `lws_callback_on_writable()` to trigger the lws write callback. |
| `WS_closeConnection()` | Call `lws_set_timeout(wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC)`. |
| `WS_allocNetworkBuffer` | `NULL` (application allocates its own buffers; data is copied into lws buffers at send time). |
| `WS_freeNetworkBuffer` | `NULL` |
| `WS_eventSourceStart()` | Create lws context with `info.event_lib_custom = &evlib_open62541`. Set SSL options if `useSSL` is configured. |
| `WS_eventSourceStop()` | Call `lws_context_destroy()` and clean up any remaining connections. |

**Parameters:**
```c
static UA_KeyValueRestriction wsConnectionParams[] = {
    {{0, UA_STRING_STATIC("address")},    &UA_TYPES[UA_TYPES_STRING],   true,  true, true},
    {{0, UA_STRING_STATIC("port")},       &UA_TYPES[UA_TYPES_UINT16],   true,  true, false},
    {{0, UA_STRING_STATIC("listen")},     &UA_TYPES[UA_TYPES_BOOLEAN],  false, true, false},
    {{0, UA_STRING_STATIC("reuse")},      &UA_TYPES[UA_TYPES_BOOLEAN],  false, true, false},
    {{0, UA_STRING_STATIC("useSSL")},     &UA_TYPES[UA_TYPES_BOOLEAN],  false, true, false},
    {{0, UA_STRING_STATIC("ssl-cert")},   &UA_TYPES[UA_TYPES_STRING],   false, true, false}, /* Server only */
    {{0, UA_STRING_STATIC("ssl-key")},    &UA_TYPES[UA_TYPES_STRING],   false, true, false}, /* Server only */
};
```

---

### Phase 3: Server Binary Protocol Integration

**File:** `src/server/ua_server_binary.c`

**Changes:**
- **`createServerConnection()`**: Parse the scheme prefix of the server URL to
  determine the required `ConnectionManager` protocol and whether SSL is needed.
  - `opc.tcp://`  → `protocol = "tcp"`, `useSSL = false`
  - `opc.ws://`   → `protocol = "ws"`, `useSSL = false`
  - `opc.wss://`  → `protocol = "ws"`, `useSSL = true`
- **`addDiscoveryUrl()`**: Preserve the original scheme in the generated
  discovery URL instead of hardcoding `opc.tcp://`.

---

### Phase 4: Client Connection Integration

**File:** `src/client/ua_client_connect.c`

**Changes:**
- **`initConnect()`**: Apply the same scheme-to-protocol mapping as the server.
  - `opc.tcp://`  → use `"tcp"` manager
  - `opc.ws://`   → use `"ws"` manager, `useSSL = false`
  - `opc.wss://`  → use `"ws"` manager, `useSSL = true`

---

### Phase 5: Build System & Default Configuration

**Files:**
- `CMakeLists.txt`
- `plugins/ua_config_default.c`

**Changes:**
- Add CMake option:
  ```cmake
  option(UA_ENABLE_WEBSOCKET_SERVER "Enable WebSocket server/client support" OFF)
  ```
- If enabled, search for `libwebsockets` and link it.
- In `UA_ServerConfig_setDefault()` / `UA_ClientConfig_setDefault()`:
  Conditionally register the new manager:
  ```c
  #ifdef UA_ENABLE_WEBSOCKET_SERVER
      UA_ConnectionManager *wsCM =
          UA_ConnectionManager_new_POSIX_WS(UA_STRING("ws connection manager"));
      if(wsCM)
          conf->eventLoop->registerEventSource(conf->eventLoop, (UA_EventSource *)wsCM);
  #endif
  ```

---

### Phase 6: Tests & Examples

**New Files:**
- `tests/check_eventloop_ws.c`
- `examples/server_ws.c`
- `examples/client_ws.c`

**Test Coverage:**
- `check_eventloop_ws.c`:
  1. Create EventLoop and register WS ConnectionManager.
  2. Open a server connection (`listen=true`, random port).
  3. Open a client connection (`listen=false`) to the server.
  4. Send binary data in both directions and verify payload integrity.
  5. Close both connections and stop the EventLoop.
- `check_utils_url_kvm.c`:
  - Update existing `parse_url_ws` and `parse_url_wss` tests to expect success.

**Examples:**
- `examples/server_ws.c`: Minimal OPC UA server listening on `opc.ws://0.0.0.0:4840`.
- `examples/client_ws.c`: Minimal OPC UA client connecting via `opc.ws://localhost:4840`.

---

## 4. File Checklist

| File | Action |
|------|--------|
| `arch/posix/eventloop_posix_lws_ws.h` | **New** – Public API declaration |
| `arch/posix/eventloop_posix_lws_ws.c` | **New** – ConnectionManager implementation |
| `src/util/ua_util.c` | **Modify** – Extend `UA_parseEndpointUrl()` with WS schemes |
| `src/server/ua_server_binary.c` | **Modify** – Protocol selection and discovery URL generation |
| `src/client/ua_client_connect.c` | **Modify** – Protocol selection for outgoing connections |
| `plugins/ua_config_default.c` | **Modify** – Conditional WS manager registration |
| `CMakeLists.txt` | **Modify** – `UA_ENABLE_WEBSOCKET_SERVER` option and lws dependency |
| `tests/check_utils_url_kvm.c` | **Modify** – Fix WS URL tests |
| `tests/check_eventloop_ws.c` | **New** – EventLoop integration tests |
| `examples/server_ws.c` | **New** – Server example |
| `examples/client_ws.c` | **New** – Client example |

---

## 5. Risks & Open Points

| Risk / Open Point | Mitigation |
|-------------------|------------|
| **lws TLS certificate handling** | For `wss://` server, certificate and key paths are passed via `ssl-cert` / `ssl-key` parameters. Self-signed certificates are acceptable for development. Production setups should use proper PKI. |
| **Buffer ownership** | `allocNetworkBuffer` / `freeNetworkBuffer` are set to `NULL`. The SecureChannel layer copies data into its own buffers before calling `sendWithConnection`, which then copies into lws-internal buffers. This avoids lifecycle issues. |
| **Compatibility with non-open62541 WS clients** | Using `"opcua"` + `""` subprotocols and binary frames provides maximum interoperability. |
| **Performance** | lws buffers and the extra copy into lws buffers may add slight overhead compared to raw TCP. This is acceptable for the initial implementation and can be optimized later if needed. |
