/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2025 (c) Fraunhofer IOSB (Author: Noel Graf)
 */

#ifndef UA_EVENTLOOP_LIBWEBSOCKETS_WS_H_
#define UA_EVENTLOOP_LIBWEBSOCKETS_WS_H_

#include "eventloop_posix_lws.h"

_UA_BEGIN_DECLS

UA_ConnectionManager *
UA_ConnectionManager_new_WS(const UA_String eventSourceName);

_UA_END_DECLS

#endif /* UA_EVENTLOOP_LIBWEBSOCKETS_WS_H_ */
