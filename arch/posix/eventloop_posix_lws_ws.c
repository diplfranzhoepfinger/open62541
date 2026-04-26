/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2025 (c) Fraunhofer IOSB (Author: Noel Graf)
 */

#include <open62541/plugin/eventloop.h>
#include <open62541/plugin/log_stdout.h>

#include "eventloop_posix_lws_ws.h"

#define WS_MAXSENDBUFSIZE (1 << 16) /* 64kB */

struct WSConnectionManager;
typedef struct WSConnectionManager WSConnectionManager;

struct WSConnection;
typedef struct WSConnection WSConnection;

/* The ConnectionManager holds a linked list of connections */
struct WSConnectionManager {
    UA_ConnectionManager cm;
    LIST_HEAD(, WSConnection) connections;
    uintptr_t lastConnectionId;
    struct lws_context *lwsContext;
    UA_EventLoop *foreign_loop;
    UA_Boolean hasListenPort;
    UA_UInt16 listenPort;
};

struct WSConnection {
    LIST_ENTRY(WSConnection) next;
    WSConnectionManager *wcm; /* Backpointer, always set */
    struct lws *wsi;          /* lws instance, may be NULL after close */
    uintptr_t connectionId;
    UA_ByteString sendBuffer; /* Data pending to be sent */
    UA_Boolean sendInProgress; /* LWS write callback triggered */

    /* Connection params */
    UA_KeyValueMap params;

    /* Callback back into the application */
    void *application;
    void *context;
    UA_ConnectionManager_connectionCallback applicationCB;
};

#define WS_PARAMETERSSIZE 7
static UA_KeyValueRestriction wsConnectionParams[WS_PARAMETERSSIZE] = {
    {{0, UA_STRING_STATIC("address")},    &UA_TYPES[UA_TYPES_STRING],   true,  true, true},
    {{0, UA_STRING_STATIC("port")},       &UA_TYPES[UA_TYPES_UINT16],   true,  true, false},
    {{0, UA_STRING_STATIC("listen")},     &UA_TYPES[UA_TYPES_BOOLEAN],  false, true, false},
    {{0, UA_STRING_STATIC("reuse")},      &UA_TYPES[UA_TYPES_BOOLEAN],  false, true, false},
    {{0, UA_STRING_STATIC("useSSL")},     &UA_TYPES[UA_TYPES_BOOLEAN],  false, true, false},
    {{0, UA_STRING_STATIC("ssl-cert")},   &UA_TYPES[UA_TYPES_STRING],   false, true, false},
    {{0, UA_STRING_STATIC("ssl-key")},    &UA_TYPES[UA_TYPES_STRING],   false, true, false}
};

static WSConnection *
findWSConnection(WSConnectionManager *wcm, uintptr_t id) {
    WSConnection *wc;
    LIST_FOREACH(wc, &wcm->connections, next) {
        if(wc->connectionId == id)
            return wc;
    }
    return NULL;
}

static void
removeWSConnection(WSConnectionManager *wcm, WSConnection *wc);

static void
notifyConnectionState(WSConnectionManager *wcm, WSConnection *wc,
                        UA_ConnectionState state,
                        const UA_KeyValueMap *params,
                        UA_ByteString msg) {
    if(!wc->applicationCB)
        return;
    wc->applicationCB(&wcm->cm, wc->connectionId,
                      wc->application, &wc->context,
                      state, params, msg);
}

/* User protocol callback. The protocol callback is the primary way lws
 * interacts with user code. */
static int
callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
            void *user, void *in, size_t len) {
    struct lws_context *lws_cx = lws_get_context(wsi);
    WSConnectionManager *wcm = (WSConnectionManager*)lws_context_user(lws_cx);
    if(!wcm)
        return -1;
    UA_EventLoop *el = wcm->cm.eventSource.eventLoop;
    WSConnection *wc = (WSConnection*)user;

    switch(reason) {
    case LWS_CALLBACK_WSI_DESTROY:
        /* Last callback for the wsi. Clean up if still present. */
        if(wc) {
            LIST_REMOVE(wc, next);
            UA_ByteString_clear(&wc->sendBuffer);
            UA_KeyValueMap_clear(&wc->params);
            UA_free(wc);
        }
        break;

    case LWS_CALLBACK_ESTABLISHED: {
        /* New incoming server connection. The wc is pre-allocated in
         * the per-session data (user). */
        if(!wc)
            return -1;
        wc->wsi = wsi;

        /* Extract remote address */
        char rip[64];
        lws_get_peer_simple(wsi, rip, sizeof(rip));
        UA_String remoteAddress = UA_STRING(rip);
        UA_KeyValuePair kvp;
        kvp.key = UA_QUALIFIEDNAME(0, "remote-address");
        UA_Variant_setScalar(&kvp.value, &remoteAddress, &UA_TYPES[UA_TYPES_STRING]);
        UA_KeyValueMap kvm = {1, &kvp};

        notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_ESTABLISHED, &kvm,
                              UA_BYTESTRING_NULL);
        break;
    }

    case LWS_CALLBACK_RECEIVE: {
        if(!wc)
            return -1;
        /* Only accept binary frames for OPC UA */
        if(lws_frame_is_binary(wsi) == 0) {
            UA_LOG_WARNING(el->logger, UA_LOGCATEGORY_NETWORK,
                           "WS %u\t| Received non-binary frame, closing",
                           (unsigned)wc->connectionId);
            return -1; /* Close connection */
        }
        UA_ByteString msg = {len, (UA_Byte*)in};
        notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_ESTABLISHED,
                              &UA_KEYVALUEMAP_NULL, msg);
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE:
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if(!wc)
            return -1;
        if(wc->sendBuffer.length == 0) {
            wc->sendInProgress = false;
            break;
        }
        /* Write data. lws_write may send less than the full buffer. */
        unsigned char *buf = wc->sendBuffer.data + LWS_PRE;
        size_t total = wc->sendBuffer.length;
        size_t written = lws_write(wsi, buf, total, LWS_WRITE_BINARY);
        if(written < total) {
            /* Partial write: move remaining data to the front.
             * lws guarantees that at least some data was consumed. */
            size_t remaining = total - written;
            memmove(buf, buf + written, remaining);
            wc->sendBuffer.length = remaining;
            lws_callback_on_writable(wsi);
        } else {
            /* All data sent */
            UA_ByteString_clear(&wc->sendBuffer);
            wc->sendInProgress = false;
        }
        break;
    }

    case LWS_CALLBACK_CLOSED:
        if(wc) {
            wc->wsi = NULL;
            notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_CLOSING,
                                  &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        }
        break;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if(!wc)
            return -1;
        wc->wsi = wsi;
        notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_ESTABLISHED,
                              &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        if(!wc)
            return -1;
        if(lws_frame_is_binary(wsi) == 0) {
            UA_LOG_WARNING(el->logger, UA_LOGCATEGORY_NETWORK,
                           "WS %u\t| Received non-binary frame, closing",
                           (unsigned)wc->connectionId);
            return -1;
        }
        UA_ByteString msg = {len, (UA_Byte*)in};
        notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_ESTABLISHED,
                              &UA_KEYVALUEMAP_NULL, msg);
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if(wc) {
            UA_LOG_WARNING(el->logger, UA_LOGCATEGORY_NETWORK,
                           "WS %u\t| Client connection error",
                           (unsigned)wc->connectionId);
            wc->wsi = NULL;
            notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_CLOSING,
                                  &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        }
        break;

    default:
        break;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    {"opcua", callback_ws, sizeof(WSConnection), 0, 0, NULL, 0},
    {"",      callback_ws, sizeof(WSConnection), 0, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM
};

static void
removeWSConnection(WSConnectionManager *wcm, WSConnection *wc) {
    if(!wc)
        return;

    /* Cancel any pending writable callback */
    if(wc->wsi) {
        lws_set_timeout(wc->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
        wc->wsi = NULL;
    }

    LIST_REMOVE(wc, next);

    /* Notify if not already closed */
    notifyConnectionState(wcm, wc, UA_CONNECTIONSTATE_CLOSED,
                          &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);

    UA_ByteString_clear(&wc->sendBuffer);
    UA_KeyValueMap_clear(&wc->params);
    UA_free(wc);

    /* If we are stopping and no connections remain, mark stopped */
    if(wcm->cm.eventSource.state == UA_EVENTSOURCESTATE_STOPPING &&
       LIST_EMPTY(&wcm->connections) && !wcm->hasListenPort) {
        wcm->cm.eventSource.state = UA_EVENTSOURCESTATE_STOPPED;
    }
}

static UA_StatusCode
WS_openConnection(UA_ConnectionManager *cm, const UA_KeyValueMap *params,
                  void *application, void *context,
                  UA_ConnectionManager_connectionCallback connectionCallback) {
    if(cm->eventSource.state != UA_EVENTSOURCETYPE_CONNECTIONMANAGER)
        return UA_STATUSCODE_BADINTERNALERROR;

    /* Check the parameters */
    WSConnectionManager *wcm = (WSConnectionManager*)cm;
    UA_EventLoop *el = cm->eventSource.eventLoop;
    UA_StatusCode res =
        UA_KeyValueRestriction_validate(el->logger, "WS",
                                        wsConnectionParams, WS_PARAMETERSSIZE,
                                        params);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    /* Extract parameters */
    const UA_String *address = (const UA_String*)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "address"),
                                 &UA_TYPES[UA_TYPES_STRING]);
    const UA_UInt16 *port = (const UA_UInt16*)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "port"),
                                 &UA_TYPES[UA_TYPES_UINT16]);
    UA_Boolean listen = false;
    const UA_Boolean *listenParam = (const UA_Boolean*)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "listen"),
                                 &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(listenParam)
        listen = *listenParam;

    const UA_Boolean *useSSL = (const UA_Boolean*)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "useSSL"),
                                 &UA_TYPES[UA_TYPES_BOOLEAN]);
    const UA_String *sslCert = (const UA_String*)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "ssl-cert"),
                                 &UA_TYPES[UA_TYPES_STRING]);
    const UA_String *sslKey = (const UA_String*)
        UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0, "ssl-key"),
                                 &UA_TYPES[UA_TYPES_STRING]);

    /* Create the lws context if not yet present */
    if(!wcm->lwsContext) {
        struct lws_context_creation_info info;
        memset(&info, 0, sizeof(info));
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                       LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
        info.port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server by default */
        info.protocols = protocols;
        info.connect_timeout_secs = 30;
        info.keepalive_timeout = 20;
        info.user = wcm;
        info.log_cx = &open62541_log_cx;
        info.event_lib_custom = &evlib_open62541;
        info.foreign_loops = (void**)&wcm->foreign_loop;

        /* SSL certificate for server */
        if(useSSL && *useSSL && sslCert && sslCert->length > 0) {
            info.ssl_cert_filepath = (const char*)sslCert->data;
            if(sslKey && sslKey->length > 0)
                info.ssl_private_key_filepath = (const char*)sslKey->data;
        }

        wcm->lwsContext = lws_create_context(&info);
        if(!wcm->lwsContext) {
            UA_LOG_ERROR(el->logger, UA_LOGCATEGORY_NETWORK,
                         "WS\t| Could not create lws context");
            return UA_STATUSCODE_BADCONNECTIONREJECTED;
        }
    }

    if(listen) {
        /* Server: configure a listen port on the existing context.
         * libwebsockets allows dynamic vhost creation. */
        if(wcm->hasListenPort) {
            UA_LOG_WARNING(el->logger, UA_LOGCATEGORY_NETWORK,
                           "WS\t| Listen port already configured (%d). "
                           "Replacing with new port %d.",
                           wcm->listenPort, *port);
        }

        struct lws_context_creation_info vhost_info;
        memset(&vhost_info, 0, sizeof(vhost_info));
        vhost_info.port = *port;
        vhost_info.protocols = protocols;
        vhost_info.user = wcm;

        /* SSL for server */
        if(useSSL && *useSSL && sslCert && sslCert->length > 0) {
            vhost_info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            vhost_info.ssl_cert_filepath = (const char*)sslCert->data;
            if(sslKey && sslKey->length > 0)
                vhost_info.ssl_private_key_filepath = (const char*)sslKey->data;
        }

        /* For listen we need an address string */
        char addr_str[address->length + 1];
        memcpy(addr_str, address->data, address->length);
        addr_str[address->length] = '\0';
        vhost_info.iface = addr_str;

        struct lws_vhost *vhost = lws_create_vhost(wcm->lwsContext, &vhost_info);
        if(!vhost) {
            UA_LOG_ERROR(el->logger, UA_LOGCATEGORY_NETWORK,
                         "WS\t| Could not create listen vhost on port %d", *port);
            return UA_STATUSCODE_BADCONNECTIONREJECTED;
        }

        wcm->hasListenPort = true;
        wcm->listenPort = *port;

        /* Generate a connection ID for the server socket itself */
        uintptr_t serverConnId = ++wcm->lastConnectionId;

        /* Notify application that server socket is opening */
        connectionCallback(cm, serverConnId, application, context,
                           UA_CONNECTIONSTATE_OPENING, &UA_KEYVALUEMAP_NULL,
                           UA_BYTESTRING_NULL);

        /* Return the server connection ID as the context pointer */
        *(void**)context = (void*)(uintptr_t)serverConnId;

        return UA_STATUSCODE_GOOD;
    }

    /* Client connection */
    WSConnection *wc = (WSConnection*)UA_calloc(1, sizeof(WSConnection));
    if(!wc)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    wc->wcm = wcm;
    wc->connectionId = ++wcm->lastConnectionId;
    UA_KeyValueMap_copy(params, &wc->params);
    wc->application = application;
    wc->context = context;
    wc->applicationCB = connectionCallback;

    LIST_INSERT_HEAD(&wcm->connections, wc, next);

    /* Build connection info */
    char addr_str[address->length + 1];
    memcpy(addr_str, address->data, address->length);
    addr_str[address->length] = '\0';

    struct lws_client_connect_info i;
    memset(&i, 0, sizeof(i));
    i.context = wcm->lwsContext;
    i.address = addr_str;
    i.port = *port;
    i.path = "/"; /* OPC UA WebSocket typically uses root path */
    i.host = i.address;
    i.origin = i.address;
    i.protocol = "opcua"; /* Try subprotocol opcua first */
    i.userdata = wc;
    i.ssl_connection |= LCCSCF_H2_QUIRK_OVERFLOWS_TXCR |
                        LCCSCF_H2_QUIRK_NGHTTP2_END_STREAM;

    if(useSSL && *useSSL)
        i.ssl_connection |= LCCSCF_USE_SSL;

    wc->wsi = lws_client_connect_via_info(&i);
    if(!wc->wsi) {
        UA_LOG_ERROR(el->logger, UA_LOGCATEGORY_NETWORK,
                     "WS %u\t| Client connection to %s:%d failed",
                     (unsigned)wc->connectionId, addr_str, *port);
        LIST_REMOVE(wc, next);
        UA_KeyValueMap_clear(&wc->params);
        UA_free(wc);
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }

    /* Notify opening */
    connectionCallback(cm, wc->connectionId, application, &wc->context,
                       UA_CONNECTIONSTATE_OPENING, &UA_KEYVALUEMAP_NULL,
                       UA_BYTESTRING_NULL);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
WS_sendWithConnection(UA_ConnectionManager *cm, uintptr_t connectionId,
                      const UA_KeyValueMap *params, UA_ByteString *buf) {
    (void)params;
    WSConnectionManager *wcm = (WSConnectionManager*)cm;
    WSConnection *wc = findWSConnection(wcm, connectionId);
    if(!wc) {
        UA_LOG_ERROR(wcm->cm.eventSource.eventLoop->logger, UA_LOGCATEGORY_NETWORK,
                     "WS\t| Could not find connection with id %u",
                     (unsigned)connectionId);
        if(buf)
            UA_ByteString_clear(buf);
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if(!wc->wsi) {
        UA_LOG_WARNING(wcm->cm.eventSource.eventLoop->logger, UA_LOGCATEGORY_NETWORK,
                       "WS %u\t| Connection already closed",
                       (unsigned)connectionId);
        if(buf)
            UA_ByteString_clear(buf);
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    }

    /* Move data to send buffer. Need LWS_PRE bytes before data for lws headers. */
    if(buf && buf->length > 0) {
        /* Allocate new buffer with LWS_PRE padding */
        UA_ByteString newBuf;
        UA_StatusCode res = UA_ByteString_allocBuffer(&newBuf, buf->length + LWS_PRE);
        if(res != UA_STATUSCODE_GOOD) {
            UA_ByteString_clear(buf);
            return res;
        }
        memset(newBuf.data, 0, LWS_PRE);
        memcpy(newBuf.data + LWS_PRE, buf->data, buf->length);
        newBuf.length = buf->length + LWS_PRE;

        /* If there's already pending data, append (simplification: reject) */
        if(wc->sendBuffer.length > 0) {
            UA_ByteString_clear(&newBuf);
            UA_ByteString_clear(buf);
            return UA_STATUSCODE_BADINTERNALERROR; /* Would need buffering queue */
        }

        wc->sendBuffer = newBuf;
        UA_ByteString_init(buf); /* Clear original without freeing (moved) */
    }

    if(wc->sendBuffer.length > 0 && !wc->sendInProgress) {
        wc->sendInProgress = true;
        lws_callback_on_writable(wc->wsi);
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
WS_closeConnection(UA_ConnectionManager *cm, uintptr_t connectionId) {
    WSConnectionManager *wcm = (WSConnectionManager*)cm;
    WSConnection *wc = findWSConnection(wcm, connectionId);
    if(!wc) {
        /* Could be the server listen socket */
        if(wcm->hasListenPort) {
            UA_LOG_INFO(wcm->cm.eventSource.eventLoop->logger, UA_LOGCATEGORY_NETWORK,
                        "WS\t| Closing listen port %d", wcm->listenPort);
            wcm->hasListenPort = false;
            wcm->listenPort = 0;
            /* vhosts cannot be individually destroyed easily; full context
             * destroy will close them. For now, mark and clean on stop. */
            return UA_STATUSCODE_GOOD;
        }
        UA_LOG_ERROR(wcm->cm.eventSource.eventLoop->logger, UA_LOGCATEGORY_NETWORK,
                     "WS\t| Could not find connection with id %u",
                     (unsigned)connectionId);
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if(wc->wsi) {
        lws_set_timeout(wc->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
        wc->wsi = NULL;
    }

    removeWSConnection(wcm, wc);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
WS_eventSourceStart(UA_ConnectionManager *cm) {
    WSConnectionManager *wcm = (WSConnectionManager*)cm;
    UA_EventLoop *el = cm->eventSource.eventLoop;
    if(!el)
        return UA_STATUSCODE_BADINTERNALERROR;

    /* Check the state */
    if(cm->eventSource.state != UA_EVENTSOURCESTATE_STOPPED) {
        UA_LOG_ERROR(el->logger, UA_LOGCATEGORY_NETWORK,
                     "WS\t| To start the ConnectionManager, it has to be "
                     "registered in an EventLoop and not started yet");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    UA_LOG_INFO(el->logger, UA_LOGCATEGORY_EVENTLOOP,
                "WS\t| Starting the ConnectionManager");

    wcm->foreign_loop = el;
    cm->eventSource.state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

static void
WS_eventSourceStop(UA_ConnectionManager *cm) {
    WSConnectionManager *wcm = (WSConnectionManager*)cm;
    if(cm->eventSource.state == UA_EVENTSOURCESTATE_STOPPING ||
       cm->eventSource.state == UA_EVENTSOURCESTATE_STOPPED)
        return;

    UA_LOG_INFO(cm->eventSource.eventLoop->logger, UA_LOGCATEGORY_EVENTLOOP,
                "WS\t| Stopping the ConnectionManager");

    cm->eventSource.state = UA_EVENTSOURCESTATE_STOPPING;

    /* Close all connections */
    WSConnection *wc, *temp;
    LIST_FOREACH_SAFE(wc, &wcm->connections, next, temp) {
        if(wc->wsi) {
            lws_set_timeout(wc->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
            wc->wsi = NULL;
        }
        removeWSConnection(wcm, wc);
    }

    /* Destroy the lws context */
    if(wcm->lwsContext) {
        lws_context_destroy(wcm->lwsContext);
        wcm->lwsContext = NULL;
    }
    wcm->hasListenPort = false;
    wcm->listenPort = 0;

    cm->eventSource.state = UA_EVENTSOURCESTATE_STOPPED;
}

static UA_StatusCode
WS_eventSourceDelete(UA_ConnectionManager *cm) {
    if(cm->eventSource.state >= UA_EVENTSOURCESTATE_STARTING) {
        UA_LOG_ERROR(cm->eventSource.eventLoop->logger, UA_LOGCATEGORY_EVENTLOOP,
                     "WS\t| The EventSource must be stopped before it can be deleted");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    WSConnectionManager *wcm = (WSConnectionManager*)cm;
    UA_String_clear(&cm->eventSource.name);
    UA_KeyValueMap_clear(&cm->eventSource.params);
    UA_free(wcm);
    return UA_STATUSCODE_GOOD;
}

static const char *wsName = "ws";

UA_ConnectionManager *
UA_ConnectionManager_new_WS(const UA_String eventSourceName) {
    WSConnectionManager *wcm = (WSConnectionManager*)
        UA_calloc(1, sizeof(WSConnectionManager));
    if(!wcm)
        return NULL;

    wcm->cm.eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    UA_String_copy(&eventSourceName, &wcm->cm.eventSource.name);
    wcm->cm.eventSource.start = (UA_StatusCode (*)(UA_EventSource*))WS_eventSourceStart;
    wcm->cm.eventSource.stop = (void (*)(UA_EventSource*))WS_eventSourceStop;
    wcm->cm.eventSource.free = (UA_StatusCode (*)(UA_EventSource*))WS_eventSourceDelete;
    wcm->cm.protocol = UA_STRING((char*)(uintptr_t)wsName);
    wcm->cm.openConnection = WS_openConnection;
    wcm->cm.allocNetworkBuffer = NULL;
    wcm->cm.freeNetworkBuffer = NULL;
    wcm->cm.sendWithConnection = WS_sendWithConnection;
    wcm->cm.closeConnection = WS_closeConnection;
    return &wcm->cm;
}
