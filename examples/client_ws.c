/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

/**
 * WebSocket Client Example
 * ------------------------
 *
 * This example demonstrates how to create an OPC UA client that connects to
 * a server via WebSocket (opc.ws://).
 *
 * Build with:
 *   cmake -DUA_ENABLE_LWS=ON ..
 *   make client_ws
 *
 * Run with a WebSocket server running on opc.ws://localhost:4840:
 *   ./client_ws
 */

#include <open62541/client.h>
#include <stdio.h>

int main(void) {
    /* Create client with default configuration */
    UA_Client *client = UA_Client_new();
    if(!client)
        return EXIT_FAILURE;

    UA_ClientConfig *config = UA_Client_getConfig(client);
    UA_ClientConfig_setDefault(config);

    /* Connect to WebSocket server */
    UA_StatusCode retval =
        UA_Client_connect(client, "opc.ws://localhost:4840");
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_CLIENT,
                     "Failed to connect to opc.ws://localhost:4840: %s",
                     UA_StatusCode_name(retval));
        UA_Client_delete(client);
        return EXIT_FAILURE;
    }

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_CLIENT,
                "Connected to opc.ws://localhost:4840");

    /* Read the variable node "MyVariable" (ns=1, i="my.variable") */
    UA_Variant value;
    UA_Variant_init(&value);
    retval = UA_Client_readValueAttribute(
        client, UA_NODEID_STRING(1, "my.variable"), &value);
    if(retval == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32])) {
        UA_Int32 intValue = *(UA_Int32*)value.data;
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_CLIENT,
                    "MyVariable value: %d", intValue);
    } else {
        UA_LOG_WARNING(UA_Log_Stdout, UA_LOGCATEGORY_CLIENT,
                       "Failed to read MyVariable: %s",
                       UA_StatusCode_name(retval));
    }
    UA_Variant_clear(&value);

    /* Disconnect and cleanup */
    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return EXIT_SUCCESS;
}
