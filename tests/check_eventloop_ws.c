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
    UA_ByteString clientReceived;
    uintptr_t clientConnectionId;
    uintptr_t serverConnectionId; /* For sending data back to client */

    /* Sawtooth reception tracking */
    UA_UInt16 lastValue;
    UA_UInt16 valuesReceived;
    UA_DateTime firstReceiveTime;
    UA_DateTime lastReceiveTime;
    UA_Boolean sawtoothValid;
} TestState;

static TestState testState;

static void
resetTestState(void) {
    memset(&testState, 0, sizeof(TestState));
    testState.sawtoothValid = true;
}

/* Helper: Encode sawtooth value into byte array */
static void
encodeSawtooth(UA_Byte *buf, UA_UInt16 value) {
    buf[0] = (UA_Byte)(value & 0xFF);
    buf[1] = (UA_Byte)((value >> 8) & 0xFF);
}

/* Helper: Decode sawtooth value from byte array */
static UA_UInt16
decodeSawtooth(const UA_Byte *buf) {
    return (UA_UInt16)(buf[0] | (buf[1] << 8));
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
        /* Store any connection ID (listen socket will be overwritten by incoming) */
        ts->serverConnectionId = connectionId;
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,
                    "SERVER: Connection established id=%lu", (unsigned long)connectionId);
    } else if(state == UA_CONNECTIONSTATE_CLOSED) {
        /* Nothing */
    } else if(msg.length > 0) {
        /* Server received data - ignore for this test */
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
    if(msg.length >= 2) {
        UA_DateTime now = UA_DateTime_now();
        if(ts->firstReceiveTime == 0)
            ts->firstReceiveTime = now;
        ts->lastReceiveTime = now;

        UA_UInt16 value = decodeSawtooth(msg.data);

        /* Check sawtooth continuity: should be lastValue+1 (mod 2001) */
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
                             clientConnectionCallback);
    ck_assert_uint_eq(res, UA_STATUSCODE_GOOD);

    /* 3) Run EventLoop until client connects */
    for(int i = 0; i < 100 && !testState.clientEstablished; i++) {
        el->run(el, 50);
    }
    ck_assert_msg(testState.clientEstablished, "Client did not connect");

    /* 4) Send sawtooth data from server to client */
    UA_DateTime startTime = UA_DateTime_now();
    UA_UInt16 sawtoothValue = 0;
    int maxIterations = 500; /* Max 25 seconds */

    for(int i = 0; i < maxIterations; i++) {
        /* Send next value if we have a server connection */
        if(testState.serverConnectionId != 0) {
            UA_Byte data[2];
            encodeSawtooth(data, sawtoothValue);
            UA_ByteString msg = {2, data};

            UA_ByteString msgCopy;
            UA_ByteString_copy(&msg, &msgCopy);

            res = cm->sendWithConnection(cm, testState.serverConnectionId,
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
    Suite *s = suite_create("Test WS Sawtooth");
    TCase *tc = tcase_create("test cases");
    tcase_set_timeout(tc, 30); /* 30 seconds timeout */
	tcase_add_test(tc, wsCreateManager);
    tcase_add_test(tc, wsSawtoothTransfer);
    suite_add_tcase(s, tc);

    SRunner *sr = srunner_create(s);
    srunner_set_fork_status(sr, CK_NOFORK);
    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
