/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

/**
 * WebSocket Server Example
 * ------------------------
 *
 * This example demonstrates how to create an OPC UA server that listens on
 * a WebSocket endpoint (opc.ws://). The server provides a single variable
 * that can be read and written by clients.
 *
 * Build with:
 *   cmake -DUA_ENABLE_LWS=ON ..
 *   make server_ws
 *
 * Run with:
 *   ./server_ws
 *
 * The server listens on opc.ws://localhost:4840.
 *
 * Connect with a WebSocket-capable OPC UA client using the URL:
 *   opc.ws://localhost:4840
 */

#include <open62541/server.h>
#include <signal.h>

static volatile UA_Boolean running = true;

static void stopHandler(int sig) {
    (void)sig;
    running = false;
}

int main(void) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    /* Create server with default configuration */
    UA_Server *server = UA_Server_new();
    if(!server)
        return EXIT_FAILURE;

    UA_ServerConfig *config = UA_Server_getConfig(server);

    /* Set the server URL to use WebSocket instead of TCP */
    UA_String_clear(&config->serverUrls[0]);
    config->serverUrls[0] = UA_STRING("opc.ws://localhost:4840");

    /* Add a variable node */
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Int32 myInteger = 42;
    UA_Variant_setScalar(&attr.value, &myInteger, &UA_TYPES[UA_TYPES_INT32]);
    attr.description = UA_LOCALIZEDTEXT("en-US", "An example integer variable");
    attr.displayName = UA_LOCALIZEDTEXT("en-US", "MyVariable");
    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;

    UA_NodeId myNodeId = UA_NODEID_STRING(1, "my.variable");
    UA_QualifiedName myName = UA_QUALIFIEDNAME(1, "MyVariable");
    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_NodeId outNodeId;
    UA_Server_addVariableNode(server, myNodeId, parentNodeId, parentReferenceNodeId,
                              myName, UA_NODEID_NULL, attr, NULL, &outNodeId);

    /* Run the server */
    UA_StatusCode retval = UA_Server_runUntilInterrupt(server);
    if(retval != UA_STATUSCODE_GOOD)
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER,
                     "Server failed with status %s", UA_StatusCode_name(retval));

    UA_Server_delete(server);
    return retval == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}
