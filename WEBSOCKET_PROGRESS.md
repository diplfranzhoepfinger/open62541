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

## Open / Pending Phases

### Phase 2: WebSocket ConnectionManager
**Status:** 🔜 Not started  
**Files:** `arch/posix/eventloop_posix_lws_ws.c` (new), `arch/posix/eventloop_posix_lws_ws.h` (new)  
**Goal:** Implement a single ConnectionManager that handles both `ws://` and `wss://` connections (server and client) using libwebsockets.

### Phase 3: Server Binary Protocol Integration
**Status:** 🔜 Not started  
**File:** `src/server/ua_server_binary.c`  
**Goal:** Update `createServerConnection()` to select the `ws` ConnectionManager for `opc.ws://` / `opc.wss://` URLs.

### Phase 4: Client Connection Integration
**Status:** 🔜 Not started  
**File:** `src/client/ua_client_connect.c`  
**Goal:** Update `initConnect()` to use the `ws` ConnectionManager for WebSocket endpoint URLs.

### Phase 5: Build System & Default Configuration
**Status:** 🔜 Not started  
**Files:** `CMakeLists.txt`, `plugins/ua_config_default.c`  
**Goal:** Add `UA_ENABLE_WEBSOCKET_SERVER` CMake option and conditional manager registration.

### Phase 6: Tests & Examples
**Status:** 🔜 Not started  
**Files:** `tests/check_eventloop_ws.c` (new), `examples/server_ws.c` (new), `examples/client_ws.c` (new)  
**Goal:** EventLoop integration tests and minimal server/client examples.

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
