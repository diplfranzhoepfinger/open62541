# WebSocket Implementation Progress

This document tracks the current progress of the OPC UA WebSocket (`opc.ws://` / `opc.wss://`) implementation for open62541.

**Branch:** `feature/ws_serv`  
**Last Updated:** 2026-04-26

---

## Completed Phases

### Phase 1: URL Parser Extension ✅ COMPLETE

**Goal:** Enable `UA_parseEndpointUrl()` to recognize `opc.ws://` and `opc.wss://` as valid OPC UA endpoint URL schemes.

#### Changes Made

| File | Change |
|------|--------|
| `src/util/ua_util.c` | Extended `schemas[]` array to include `"opc.ws://"` and `"opc.wss://"`. Increased `UA_SCHEMAS_SIZE` from 4 to 6. Adjusted minimum URL length check from 11 to 10 characters. |
| `tests/check_utils_url_kvm.c` | Added new test `parse_url_ws`. Updated `parse_url_wss` to expect `UA_STATUSCODE_GOOD` (was: expected failure as known bug). Registered `parse_url_ws` in test suite. |

#### Commit
```
feat(util): Add opc.ws:// and opc.wss:// URL scheme support
Commit: bb09fe210
Files changed: 2 (+17 / -7)
```

#### Tests Executed

**Test Binary:** `build/bin/tests/check_utils_url_kvm`

```
Running suite(s): Util Ext2
100%: Checks: 45, Failures: 0, Errors: 0
```

**Result:** All 45 URL parser tests pass, including the new WebSocket URL tests.

#### Test Coverage for WebSocket URLs

| Test | URL | Expected | Status |
|------|-----|----------|--------|
| `parse_url_ws` | `opc.ws://host:8080` | Port=8080, GOOD | ✅ Pass |
| `parse_url_wss` | `opc.wss://host:8080` | Port=8080, GOOD | ✅ Pass |
| `parse_url_tcp` | `opc.tcp://host:4840` | Port=4840, GOOD | ✅ Pass (unchanged) |

---

### Phase 2: WebSocket ConnectionManager ✅ COMPLETE

**Goal:** Implement a single ConnectionManager that handles both `ws://` and `wss://` connections (server and client) using libwebsockets.

#### Changes Made

| File | Change |
|------|--------|
| `arch/posix/eventloop_posix_lws_ws.h` | **New** – Internal header with `UA_ConnectionManager_new_WS()` declaration |
| `arch/posix/eventloop_posix_lws_ws.c` | **New** – Full ConnectionManager implementation using libwebsockets |
| `include/open62541/plugin/eventloop.h` | Added public API declaration for `UA_ConnectionManager_new_WS()` with documentation |
| `CMakeLists.txt` | Added WS source files to `UA_ENABLE_LWS` build block |

#### Features
- Server mode (`listen=true`): Opens listening port, accepts incoming WebSocket connections
- Client mode (`listen=false`): Connects to remote WebSocket endpoints via `lws_client_connect_via_info()`
- Subprotocols: `{"opcua", ""}` (max compatibility)
- Binary frames only (`LWS_WRITE_BINARY`) for OPC UA binary protocol
- TLS/WSS via `useSSL` parameter + optional `ssl-cert`/`ssl-key` for server
- Reuses existing `eventloop_posix_lws.c` for EventLoop integration

#### Commit
```
feat(ws): Add WebSocket ConnectionManager for opc.ws:// and opc.wss://
Commit: 1777422df
Files changed: 4 (+659 / -1)
```

---

### Phase 3: Server Binary Protocol Integration ✅ COMPLETE

**Goal:** Update `createServerConnection()` to select the `ws` ConnectionManager for `opc.ws://` / `opc.wss://` URLs.

#### Changes Made

| File | Change |
|------|--------|
| `src/server/ua_server_binary.c` | `createServerConnection()`: Parse URL scheme (`tcp`/`ws`) and select matching ConnectionManager. Pass `useSSL=true` for `opc.wss://`. `addDiscoveryUrl()`: Preserve original scheme in discovery URLs instead of hardcoded `opc.tcp://`. Added `currentScheme` field to `UA_BinaryProtocolManager`. |

#### Commit
```
feat(server/client): Integrate WebSocket protocol selection
Commit: 6f46ee76b
Files changed: 2 (+94 / -17)
```

---

### Phase 4: Client Connection Integration ✅ COMPLETE

**Goal:** Update `initConnect()` to use the `ws` ConnectionManager for WebSocket endpoint URLs.

#### Changes Made

| File | Change |
|------|--------|
| `src/client/ua_client_connect.c` | `initConnect()`: Parse URL scheme to select `tcp` or `ws` ConnectionManager. Pass `useSSL=true` for `opc.wss://`. |

*(Same commit as Phase 3)*

---

### Phase 5: Build System & Default Configuration ✅ COMPLETE

**Goal:** Integrate WS ConnectionManager into default server/client configuration.

#### Changes Made

| File | Change |
|------|--------|
| `plugins/ua_config_default.c` | Under `#ifdef UA_ENABLE_LWS`: Register `UA_ConnectionManager_new_WS()` in default EventLoop setup |

**Note:** No separate `UA_ENABLE_WEBSOCKET_SERVER` CMake option needed. WS support is tied to existing `UA_ENABLE_LWS` which already includes libwebsockets.

---

### Phase 6: Tests & Examples ✅ COMPLETE

**Goal:** EventLoop integration tests and minimal server/client examples.

#### Changes Made

| File | Change |
|------|--------|
| `tests/check_eventloop_ws.c` | **New** – Unit test for WS ConnectionManager lifecycle (create/start/stop) |
| `examples/server_ws.c` | **New** – Minimal OPC UA server on `opc.ws://localhost:4840` |
| `examples/client_ws.c` | **New** – Minimal OPC UA client connecting via `opc.ws://` |
| `tests/CMakeLists.txt` | Added `check_eventloop_ws.c` to test suite under `UA_ENABLE_LWS` |
| `examples/CMakeLists.txt` | Added `server_ws` and `client_ws` example targets |

#### Test Results

```
Running suite(s): Test WS EventLoop
100%: Checks: 1, Failures: 0, Errors: 0
```

#### Commit
```
feat(tests/examples): Add WebSocket test and examples
Commit: 1c150f60e
Files changed: 6 (+224 / -11)
```

---

## Build Commands Used

```bash
# Configure
cd build
cmake .. -DUA_BUILD_UNIT_TESTS=ON

# Build all
make -j$(nproc)

# Run URL parser tests
./bin/tests/check_utils_url_kvm
```

---

## Design Decisions (Fixed)

See `WEBSOCKET_PLAN.md` for the full implementation plan. Key decisions remain:
- Single ConnectionManager (`protocol = "ws"`) for both `ws` and `wss`
- TLS controlled via `useSSL` parameter
- libwebsockets subprotocols: `{"opcua", ""}`
- Binary WebSocket frames (`LWS_WRITE_BINARY`)
- Reuse existing `eventloop_posix_lws.c` for EventLoop integration
