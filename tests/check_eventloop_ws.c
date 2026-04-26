/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <open62541/plugin/eventloop.h>
#include <open62541/plugin/log_stdout.h>
#include "open62541/types.h"
#include "open62541/util.h"

#include <check.h>

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

    cm->eventSource.stop(&cm->eventSource);

    el->free(el);
} END_TEST

int main(void) {
    Suite *s = suite_create("Test WS EventLoop");
    TCase *tc = tcase_create("test cases");
    tcase_set_timeout(tc, 5);
    tcase_add_test(tc, wsCreateManager);
    suite_add_tcase(s, tc);

    SRunner *sr = srunner_create(s);
    srunner_set_fork_status(sr, CK_NOFORK);
    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
