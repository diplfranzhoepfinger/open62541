/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <open62541/plugin/eventloop.h>
#include <open62541/plugin/log_stdout.h>
#include "open62541/types.h"
#include "open62541/util.h"

#include <check.h>
#include <string.h>

/* Shared state for the test */
typedef struct {
    UA_Boolean clientEstablished;
    UA_Boolean clientClosed;
    UA_ByteString clientEchoReceived;
    uintptr_t clientConnectionId;

    UA_Boolean serverIncomingEstablished;
    UA_ByteString serverReceived;
    uintptr_t serverIncomingConnectionId;

    /* Sawtooth reception tracking (wsSawtoothTransfer) */
    UA_UInt16 lastValue;
    UA_UInt16 valuesReceived;
    UA_DateTime firstReceiveTime;
    UA_DateTime lastReceiveTime;
    UA_Boolean sawtoothValid;
} TestState;

static void
encodeSawtooth(UA_Byte *buf, UA_UInt16 value) {
    buf[0] = (UA_Byte)(value & 0xFF);
    buf[1] = (UA_Byte)((value >> 8) & 0xFF);
}

static UA_UInt16
decodeSawtooth(const UA_Byte *buf) {
    return (UA_UInt16)(buf[0] | (buf[1] << 8));
}

static void
clientSawtoothCallback(UA_ConnectionManager *cm, uintptr_t connectionId,
                       void *application, void **connectionContext,
                       UA_ConnectionState state,
                       const UA_KeyValueMap *params,
                       UA_ByteString msg) {
    (void)cm; (void)connectionContext; (void)params;
    TestState *ts = (TestState*)application;
    if(state == UA_CONNECTIONSTATE_ESTABLISHED && !ts->clientEstablished) {
        ts->clientEstablished = true;
        ts->clientConnectionId = connectionId;
    } else if(state == UA_CONNECTIONSTATE_CLOSED) {
        ts->clientClosed = true;
    }
    if(msg.length >= 2) {
        UA_DateTime now = UA_DateTime_now();
        if(ts->firstReceiveTime == 0)
            ts->firstReceiveTime = now;
        ts->lastReceiveTime = now;
        UA_UInt16 value = decodeSawtooth(msg.data);
        UA_UInt16 expected = (ts->lastValue + 1) % 2001;
        if(ts->valuesReceived > 0 && value != expected) {
            UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                           "Sawtooth discontinuity: expected %d, got %d",
                           expected, value);
            ts->sawtoothValid = false;
        }
        ts->lastValue = value;
        ts->valuesReceived++;
    }
}

static TestState testState;

static void
resetTestState(void) {
    memset(&testState, 0, sizeof(TestState));
    testState.sawtoothValid = true;
}

static void
serverConnectionCallback(UA_ConnectionManager *cm, uintptr_t connectionId,
                         void *application, void **connectionContext,
                         UA_ConnectionState state,
                         const UA_KeyValueMap *params,
                         UA_ByteString msg) {
    (void)cm; (void)params;
    TestState *ts = (TestState*)application;

    if(state == UA_CONNECTIONSTATE_ESTABLISHED) {
        if(connectionContext && *connectionContext == (void*)1) {
            /* Listen socket - ignore */
        } else {
            /* Incoming client connection */
            ts->serverIncomingConnectionId = connectionId;
            ts->serverIncomingEstablished = true;
            UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                        "SERVER: Incoming connection id=%lu", (unsigned long)connectionId);
        }
    } else if(state == UA_CONNECTIONSTATE_CLOSED) {
        /* Nothing */
    }
    if(msg.length > 0) {
        /* Server received data from client - echo it back */
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                    "SERVER: Received %zu bytes, echoing back", msg.length);
        UA_ByteString_clear(&ts->serverReceived);
        UA_ByteString_copy(&msg, &ts->serverReceived);
        /* Echo back on same connection */
        UA_ByteString echo;
        UA_ByteString_copy(&msg, &echo);
        cm->sendWithConnection(cm, connectionId, &UA_KEYVALUEMAP_NULL, &echo);
    }
}

static void
clientConnectionCallback(UA_ConnectionManager *cm, uintptr_t connectionId,
                         void *application, void **connectionContext,
                         UA_ConnectionState state,
                         const UA_KeyValueMap *params,
                         UA_ByteString msg) {
    (void)cm; (void)connectionContext; (void)params;
    TestState *ts = (TestState*)application;

    if(state == UA_CONNECTIONSTATE_ESTABLISHED) {
        if(!ts->clientEstablished) {
            ts->clientEstablished = true;
            ts->clientConnectionId = connectionId;
        }
    } else if(state == UA_CONNECTIONSTATE_CLOSED) {
        ts->clientClosed = true;
    }
    if(msg.length > 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                    "CLIENT: Received echo %zu bytes", msg.length);
        UA_ByteString_clear(&ts->clientEchoReceived);
        UA_ByteString_copy(&msg, &ts->clientEchoReceived);
    }
}

START_TEST(wsRoundtripEcho) {
    resetTestState();

    /* Create EventLoop and WS ConnectionManager */
    UA_ConnectionManager *cm = UA_ConnectionManager_new_WS(UA_STRING("wsCM"));
    ck_assert_ptr_nonnull(cm);

    UA_EventLoop *el = UA_EventLoop_new_POSIX(UA_Log_Stdout);
    ck_assert_ptr_nonnull(el);

    UA_StatusCode res = el->registerEventSource(el, &cm->eventSource);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    res = el->start(el);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    /* 1) Open server listening connection */
    UA_String address = UA_STRING("127.0.0.1");
    UA_UInt16 port = 4845;
    UA_Boolean listen = true;
    UA_KeyValuePair sp[4];
    sp[0].key = UA_QUALIFIEDNAME(0, "address");
    UA_Variant_setScalar(&sp[0].value, &address, &UA_TYPES[UA_TYPES_STRING]);
    sp[1].key = UA_QUALIFIEDNAME(0, "port");
    UA_Variant_setScalar(&sp[1].value, &port, &UA_TYPES[UA_TYPES_UINT16]);
    sp[2].key = UA_QUALIFIEDNAME(0, "listen");
    UA_Variant_setScalar(&sp[2].value, &listen, &UA_TYPES[UA_TYPES_BOOLEAN]);
    sp[3].key = UA_QUALIFIEDNAME(0, "reuse");
    UA_Variant_setScalar(&sp[3].value, &listen, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_KeyValueMap skvm = {4, sp};

    void *sctx = (void*)1;
    res = cm->openConnection(cm, &skvm, &testState, &sctx, serverConnectionCallback);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    for(int i = 0; i < 20; i++) el->run(el, 10);

    /* 2) Open client connection */
    UA_Boolean clisten = false;
    UA_KeyValuePair cp[3];
    cp[0].key = UA_QUALIFIEDNAME(0, "address");
    UA_Variant_setScalar(&cp[0].value, &address, &UA_TYPES[UA_TYPES_STRING]);
    cp[1].key = UA_QUALIFIEDNAME(0, "port");
    UA_Variant_setScalar(&cp[1].value, &port, &UA_TYPES[UA_TYPES_UINT16]);
    cp[2].key = UA_QUALIFIEDNAME(0, "listen");
    UA_Variant_setScalar(&cp[2].value, &clisten, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_KeyValueMap ckvm = {3, cp};

    res = cm->openConnection(cm, &ckvm, &testState, NULL, clientConnectionCallback);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    /* 3) Wait for client to connect */
    for(int i = 0; i < 200 && !testState.clientEstablished; i++) {
        el->run(el, 50);
    }
    ck_assert_msg(testState.clientEstablished, "Client did not connect");
    ck_assert_msg(testState.serverIncomingEstablished, "Server did not accept incoming connection");

    /* 4) Client sends data */
    UA_ByteString clientMsg = UA_BYTESTRING("HelloRoundtrip");
    UA_ByteString clientMsgCopy;
    UA_ByteString_copy(&clientMsg, &clientMsgCopy);
    res = cm->sendWithConnection(cm, testState.clientConnectionId, &UA_KEYVALUEMAP_NULL, &clientMsgCopy);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "CLIENT: Sent HelloRoundtrip");

    /* 5) Run EventLoop to let data flow both ways */
    for(int i = 0; i < 100 && testState.clientEchoReceived.length == 0; i++) {
        el->run(el, 50);
    }

    /* 6) Validate echo */
    ck_assert_uint_eq(testState.clientEchoReceived.length, 14);
    ck_assert_msg(memcmp(testState.clientEchoReceived.data, "HelloRoundtrip", 14) == 0,
                  "Echo data mismatch");
    ck_assert_uint_eq(testState.serverReceived.length, 14);
    ck_assert_msg(memcmp(testState.serverReceived.data, "HelloRoundtrip", 14) == 0,
                  "Server received data mismatch");

    /* 7) Cleanup */
    if(testState.clientConnectionId != 0)
        cm->closeConnection(cm, testState.clientConnectionId);
    if(testState.serverIncomingConnectionId != 0)
        cm->closeConnection(cm, testState.serverIncomingConnectionId);

    for(int i = 0; i < 20; i++) el->run(el, 100);

    el->stop(el);
    int iteration = 0;
    while(el->state != UA_EVENTLOOPSTATE_STOPPED && iteration < 50) {
        el->run(el, 100);
        iteration++;
    }
    ck_assert(el->state == UA_EVENTLOOPSTATE_STOPPED);

    UA_ByteString_clear(&testState.clientEchoReceived);
    UA_ByteString_clear(&testState.serverReceived);
    el->free(el);
} END_TEST

START_TEST(wsSawtoothTransfer) {
    resetTestState();

    /* Create EventLoop and WS ConnectionManager */
    UA_ConnectionManager *cm = UA_ConnectionManager_new_WS(UA_STRING("wsCM"));
    ck_assert_ptr_nonnull(cm);

    UA_EventLoop *el = UA_EventLoop_new_POSIX(UA_Log_Stdout);
    ck_assert_ptr_nonnull(el);

    UA_StatusCode res = el->registerEventSource(el, &cm->eventSource);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    res = el->start(el);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    /* 1) Open server listening connection */
    UA_String address = UA_STRING("127.0.0.1");
    UA_UInt16 port = 4843; /* Non-standard port */
    UA_Boolean listen = true;

    UA_KeyValuePair serverParams[4];
    serverParams[0].key = UA_QUALIFIEDNAME(0, "address");
    UA_Variant_setScalar(&serverParams[0].value, &address, &UA_TYPES[UA_TYPES_STRING]);
    serverParams[1].key = UA_QUALIFIEDNAME(0, "port");
    UA_Variant_setScalar(&serverParams[1].value, &port, &UA_TYPES[UA_TYPES_UINT16]);
    serverParams[2].key = UA_QUALIFIEDNAME(0, "listen");
    UA_Variant_setScalar(&serverParams[2].value, &listen, &UA_TYPES[UA_TYPES_BOOLEAN]);
    serverParams[3].key = UA_QUALIFIEDNAME(0, "reuse");
    UA_Variant_setScalar(&serverParams[3].value, &listen, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_KeyValueMap serverKvm = {4, serverParams};

    void *serverContext = (void*)1;
    res = cm->openConnection(cm, &serverKvm, &testState, &serverContext,
                             serverConnectionCallback);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    /* Give server time to listen */
    for(int i = 0; i < 10; i++) el->run(el, 10);

    /* 2) Open client connection */
    UA_Boolean clientListen = false;
    UA_KeyValuePair clientParams[3];
    clientParams[0].key = UA_QUALIFIEDNAME(0, "address");
    UA_Variant_setScalar(&clientParams[0].value, &address, &UA_TYPES[UA_TYPES_STRING]);
    clientParams[1].key = UA_QUALIFIEDNAME(0, "port");
    UA_Variant_setScalar(&clientParams[1].value, &port, &UA_TYPES[UA_TYPES_UINT16]);
    clientParams[2].key = UA_QUALIFIEDNAME(0, "listen");
    UA_Variant_setScalar(&clientParams[2].value, &clientListen, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_KeyValueMap clientKvm = {3, clientParams};

    res = cm->openConnection(cm, &clientKvm, &testState, NULL,
                             clientSawtoothCallback);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    /* 3) Run EventLoop until client connects */
    for(int i = 0; i < 100 && !testState.clientEstablished; i++) {
        el->run(el, 50);
    }
    ck_assert_msg(testState.clientEstablished, "Client did not connect");

    /* 4) Send sawtooth data from server to client */
    UA_UInt16 sawtoothValue = 0;
    int maxIterations = 500; /* Max 25 seconds */

    for(int i = 0; i < maxIterations; i++) {
        /* Send next value if we have a server connection */
        if(testState.serverIncomingConnectionId != 0) {
            UA_Byte data[2];
            encodeSawtooth(data, sawtoothValue);
            UA_ByteString msg = {2, data};

            UA_ByteString msgCopy;
            UA_ByteString_copy(&msg, &msgCopy);

            res = cm->sendWithConnection(cm, testState.serverIncomingConnectionId,
                                         &UA_KEYVALUEMAP_NULL, &msgCopy);
            if(res == UA_STATUSCODE_GOOD) {
                sawtoothValue = (sawtoothValue + 1) % 2001;
            }
        }

        /* Run EventLoop */
        el->run(el, 50);

        /* Check if we received enough data */
        if(testState.valuesReceived >= 100)
            break;
    }

    /* 5) Validate results */
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                "TEST RESULT: valuesReceived=%u sawtoothValid=%d",
                testState.valuesReceived, testState.sawtoothValid);
    ck_assert_uint_ge(testState.valuesReceived, 50); /* At least 50 values */
    ck_assert_msg(testState.sawtoothValid, "Sawtooth data was corrupted");

    /* Check timing: data should arrive at roughly send rate */
    if(testState.firstReceiveTime > 0 && testState.lastReceiveTime > 0) {
        UA_Double durationSec = (UA_Double)(testState.lastReceiveTime - testState.firstReceiveTime)
            / (UA_Double)UA_DATETIME_SEC;
        UA_Double rate = (UA_Double)testState.valuesReceived / durationSec;
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                    "Received %u values in %.2f seconds (%.1f Hz)",
                    testState.valuesReceived, durationSec, rate);
        /* We send every 50ms (20 Hz), should receive at least 10 Hz */
        ck_assert_msg(rate >= 10.0, "Data rate too low: %.1f Hz", rate);
    }

    /* 6) Cleanup */
    if(testState.clientConnectionId != 0)
        cm->closeConnection(cm, testState.clientConnectionId);

    for(int i = 0; i < 20; i++) el->run(el, 100);

    el->stop(el);
    int iteration = 0;
    while(el->state != UA_EVENTLOOPSTATE_STOPPED && iteration < 50) {
        el->run(el, 100);
        iteration++;
    }
    ck_assert(el->state == UA_EVENTLOOPSTATE_STOPPED);
    el->free(el);
} END_TEST

START_TEST(wsCreateManager) {
    /* Simply test that the WS ConnectionManager can be created and started */
    UA_ConnectionManager *cm = UA_ConnectionManager_new_WS(UA_STRING("wsCM"));
    ck_assert_ptr_nonnull(cm);
    UA_String wsString = UA_STRING("ws");
    ck_assert(UA_String_equal(&cm->protocol, &wsString));

    UA_EventLoop *el = UA_EventLoop_new_POSIX(UA_Log_Stdout);
    ck_assert_ptr_nonnull(el);

    UA_StatusCode res = el->registerEventSource(el, &cm->eventSource);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    res = el->start(el);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    el->stop(el);
    int i = 0;
    while(el->state != UA_EVENTLOOPSTATE_STOPPED && i < 20) {
        el->run(el, 100);
        i++;
    }
    ck_assert(el->state == UA_EVENTLOOPSTATE_STOPPED);
    el->free(el);
} END_TEST

int main(void) {
    Suite *s = suite_create("Test WS");
    TCase *tc = tcase_create("test cases");
    tcase_set_timeout(tc, 30); /* 30 seconds timeout */
	tcase_add_test(tc, wsCreateManager);
    tcase_add_test(tc, wsRoundtripEcho);
    tcase_add_test(tc, wsSawtoothTransfer);
    suite_add_tcase(s, tc);

    SRunner *sr = srunner_create(s);
    srunner_set_fork_status(sr, CK_NOFORK);
    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
